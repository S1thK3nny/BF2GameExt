#include "pch.h"
#include "disguise_model_override.hpp"
#include "core/resolve.hpp"

#include <cstring>
#include <detours.h>

// =============================================================================
// WeaponDisguise Extension
//
// Adds a custom ODF property to WeaponDisguise:
//
// DisguiseModel = modelname
//   After vanilla raise (SetClass to enemy), overrides the entity's model
//   with a specific GameModel looked up by name hash. Must be loaded in memory.
//   If set to " " (space), suppresses model override entirely (keeps original).
//
// If not set, vanilla behavior is preserved (clone first enemy soldier).
//
// NOTE: DisguiseAnimation was investigated but shelved. The SoldierAnimator
// caches all animation data at spawn time (8KB+ of ZephyrPose state) and
// provides no runtime "refresh with different bank" API. Changing mAnimationBank
// on the class has no effect — the animator never re-reads it after init.
// Implementing runtime animation bank switching would require deep hooks into
// the ZephyrSkeleton/Pose system. See git history for the attempted approaches.
// =============================================================================

// ---------------------------------------------------------------------------
// Game function types
// ---------------------------------------------------------------------------

using fn_hash_string_t = uint32_t(__cdecl*)(const char*);

// WeaponDisguiseClass::SetProperty
using fn_SetProperty_t = void(__fastcall*)(void* ecx, void* edx,
                                           unsigned int hash, const char* value);

// Disguise raise/drop — __thiscall(this)
using fn_DisguiseFunc_t = void(__fastcall*)(void* ecx, void* edx);

// PblHashTableCode::_Find
using fn_HashTableFind_t = void(__cdecl*)(uint32_t* table, int tableParam,
                                          uint32_t hash);

// PDB: EntityGeometry+0x0130 = GameModel* mModel
static constexpr int kEntityGeom_mModel_offset = 0x130;

// ---------------------------------------------------------------------------
// Resolved pointers
// ---------------------------------------------------------------------------

static fn_hash_string_t    fn_hash_string     = nullptr;
static fn_HashTableFind_t  fn_hashtable_find   = nullptr;
static uint32_t*           g_gameModelTable    = nullptr;

// ---------------------------------------------------------------------------
// Trampolines
// ---------------------------------------------------------------------------

static fn_SetProperty_t    original_SetProperty     = nullptr;
static fn_DisguiseFunc_t   original_DisguiseRaise    = nullptr;
static fn_DisguiseFunc_t   original_DisguiseDrop     = nullptr;

// ---------------------------------------------------------------------------
// Custom property hash
// ---------------------------------------------------------------------------

static uint32_t g_disguiseModelPropHash = 0;

// ---------------------------------------------------------------------------
// Per-class configuration
// ---------------------------------------------------------------------------

static constexpr int kMaxDisguiseConfigs = 32;

struct DisguiseConfig {
    void*    classPtr;
    char     modelName[64];
    uint32_t modelNameHash;
    bool     suppressModel;         // DisguiseModel = " " -> suppress
};

static DisguiseConfig g_configs[kMaxDisguiseConfigs] = {};
static int            g_configCount = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static DisguiseConfig* findOrCreateConfig(void* classPtr)
{
    for (int i = 0; i < g_configCount; i++) {
        if (g_configs[i].classPtr == classPtr)
            return &g_configs[i];
    }
    if (g_configCount >= kMaxDisguiseConfigs) return nullptr;
    DisguiseConfig* cfg = &g_configs[g_configCount++];
    memset(cfg, 0, sizeof(*cfg));
    cfg->classPtr = classPtr;
    return cfg;
}

static const DisguiseConfig* findConfig(void* classPtr)
{
    for (int i = 0; i < g_configCount; i++) {
        if (g_configs[i].classPtr == classPtr)
            return &g_configs[i];
    }
    return nullptr;
}

static void* findGameModel(uint32_t nameHash)
{
    if (!g_gameModelTable || !fn_hashtable_find || nameHash == 0)
        return nullptr;
    void* result = nullptr;
    __asm {
        push nameHash
        push 0x800
        push g_gameModelTable
        call fn_hashtable_find
        add  esp, 12
        mov  result, eax
    }
    return result;
}

// Get EntityGeometry* (struct_base) from WeaponDisguise this.
// Replicates the vanilla raise function's MI vtable chain:
//   obj = *(weapon+0x6C)
//   adjustedThis = obj + 0x18
//   vtable = *(obj+0x18)
//   entity = vtable[8](adjustedThis)
static void* getEntityGeometry(void* weaponThis)
{
    __try {
        uintptr_t obj = *(uintptr_t*)((uintptr_t)weaponThis + 0x6C);
        if (!obj) return nullptr;
        uintptr_t vtable = *(uintptr_t*)(obj + 0x18);
        if (!vtable) return nullptr;
        typedef void*(__thiscall* VFunc_t)(void*);
        VFunc_t fn = (VFunc_t)(*(uintptr_t*)(vtable + 0x20));
        return fn((void*)(obj + 0x18));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void* getWeaponClass(void* weaponThis)
{
    __try { return *(void**)((uintptr_t)weaponThis + 0x64); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ---------------------------------------------------------------------------
// Hook: WeaponDisguiseClass::SetProperty
// ---------------------------------------------------------------------------

static void __fastcall hooked_SetProperty(void* ecx, void* /*edx*/,
                                          unsigned int hash, const char* value)
{
    if (hash == g_disguiseModelPropHash && g_disguiseModelPropHash != 0) {
        if (!value) return;
        DisguiseConfig* cfg = findOrCreateConfig(ecx);
        if (!cfg) return;

        if (value[0] == '\0' || (value[0] == ' ' && value[1] == '\0')) {
            cfg->suppressModel = true;
            cfg->modelName[0] = '\0';
            cfg->modelNameHash = 0;
        } else {
            cfg->suppressModel = false;
            strncpy_s(cfg->modelName, sizeof(cfg->modelName), value, _TRUNCATE);
            cfg->modelNameHash = fn_hash_string(value);
        }
        return;
    }

    original_SetProperty(ecx, nullptr, hash, value);
}

// ---------------------------------------------------------------------------
// Hook: Disguise raise
//
// 1. Save original model BEFORE vanilla SetClass changes it
// 2. Run vanilla (SetClass + team flip + STATE_ON)
// 3. If DisguiseModel = " ": restore saved model (suppress)
// 4. If DisguiseModel = name: override with custom model
// ---------------------------------------------------------------------------

static void __fastcall hooked_DisguiseRaise(void* ecx, void* edx)
{
    auto log = get_gamelog();

    void* weaponClass = getWeaponClass(ecx);
    const DisguiseConfig* cfg = weaponClass ? findConfig(weaponClass) : nullptr;

    if (!cfg) {
        original_DisguiseRaise(ecx, edx);
        return;
    }

    // Save original model BEFORE vanilla SetClass changes it
    void* entityBase = getEntityGeometry(ecx);
    void* savedModel = nullptr;
    if (entityBase) {
        __try { savedModel = *(void**)((uintptr_t)entityBase + kEntityGeom_mModel_offset); }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Run vanilla (SetClass + team flip + STATE_ON)
    original_DisguiseRaise(ecx, edx);

    // Override model after vanilla
    entityBase = getEntityGeometry(ecx);
    if (!entityBase) return;

    __try {
        if (cfg->suppressModel) {
            if (savedModel) {
                *(void**)((uintptr_t)entityBase + kEntityGeom_mModel_offset) = savedModel;
            }
        }
        else if (cfg->modelName[0] != '\0') {
            void* customModel = findGameModel(cfg->modelNameHash);
            if (customModel) {
                *(void**)((uintptr_t)entityBase + kEntityGeom_mModel_offset) = customModel;
            } else {
                log("[DisguiseExt] GameModel '%s' not found\n", cfg->modelName);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        log("[DisguiseExt] Exception in model override\n");
    }
}

// ---------------------------------------------------------------------------
// Hook: Disguise drop — vanilla handles everything, just passthrough
// ---------------------------------------------------------------------------

static void __fastcall hooked_DisguiseDrop(void* ecx, void* edx)
{
    original_DisguiseDrop(ecx, edx);
}

// ---------------------------------------------------------------------------
// Install / Uninstall / Reset
// ---------------------------------------------------------------------------

void disguise_ext_install(uintptr_t exe_base)
{
    using namespace game_addrs::modtools;
    fn_hash_string    = (fn_hash_string_t)resolve(exe_base, hash_string);
    fn_hashtable_find = (fn_HashTableFind_t)resolve(exe_base, pbl_hash_table_find);
    g_gameModelTable  = (uint32_t*)resolve(exe_base, game_model_table);

    g_disguiseModelPropHash = fn_hash_string("DisguiseModel");

    original_SetProperty    = (fn_SetProperty_t)resolve(exe_base, disguise_set_property);
    original_DisguiseRaise  = (fn_DisguiseFunc_t)resolve(exe_base, disguise_raise);
    original_DisguiseDrop   = (fn_DisguiseFunc_t)resolve(exe_base, disguise_drop);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG r1 = DetourAttach(&(PVOID&)original_SetProperty,    hooked_SetProperty);
    LONG r2 = DetourAttach(&(PVOID&)original_DisguiseRaise,  hooked_DisguiseRaise);
    LONG r3 = DetourAttach(&(PVOID&)original_DisguiseDrop,   hooked_DisguiseDrop);
    LONG rc = DetourTransactionCommit();
    (void)r1; (void)r2; (void)r3; (void)rc;
}

void disguise_ext_uninstall()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (original_SetProperty)    DetourDetach(&(PVOID&)original_SetProperty,    hooked_SetProperty);
    if (original_DisguiseRaise)  DetourDetach(&(PVOID&)original_DisguiseRaise,  hooked_DisguiseRaise);
    if (original_DisguiseDrop)   DetourDetach(&(PVOID&)original_DisguiseDrop,   hooked_DisguiseDrop);
    DetourTransactionCommit();
}

void disguise_ext_reset()
{
    memset(g_configs, 0, sizeof(g_configs));
    g_configCount = 0;
}
