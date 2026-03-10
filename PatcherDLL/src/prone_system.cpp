#include "pch.h"
#include "prone_system.hpp"
#include "lua_hooks.hpp"

#include <detours.h>

// =============================================================================
// Prone stance system
//
// Wires up the unused PRONE soldier state (SoldierState 2) that Pandemic
// stubbed out before shipping BF2.
//
//   1. Detours both Controllable::Crouch (0x00543B60) and StandUp (0x005435D0).
//      The game's crouch key is a toggle: STAND->Crouch(), CROUCH->StandUp().
//      We hook StandUp to intercept the CROUCH->STAND toggle and redirect
//      it to PRONE.  From PRONE, StandUp naturally handles PRONE->STAND
//      with headroom collision checks and a prone->crouch fallback.
//
//   2. Patches the Controllable vtable Prone slot (offset 0xA0) from the
//      vanilla "return false" stub to a real function that enters prone.
//
//   3. Exposes EnableProne(0/1) to Lua for runtime toggling.
// =============================================================================

static constexpr uintptr_t kUnrelocatedBase = 0x400000u;

static inline void* resolve(uintptr_t exe_base, uintptr_t unrelocated_addr)
{
    return (void*)((unrelocated_addr - kUnrelocatedBase) + exe_base);
}

typedef void (__cdecl* GameLog_t)(const char* fmt, ...);
static GameLog_t g_gameLog = nullptr;

static GameLog_t get_gamelog_local()
{
    auto base = (uintptr_t)GetModuleHandleW(nullptr);
    return (GameLog_t)((0x7E3D50 - 0x400000u) + base);
}

// ---------------------------------------------------------------------------
// Addresses (unrelocated, BF2_modtools.exe)
// ---------------------------------------------------------------------------
static constexpr uintptr_t kCrouchInner     = 0x00543B60; // Controllable::Crouch inner (__thiscall, ECX=Controllable*)
static constexpr uintptr_t kStandUpInner    = 0x005435D0; // Controllable::StandUp inner (__thiscall, ECX=Controllable*)
static constexpr uintptr_t kSetState        = 0x00406C62; // SetState thunk (__thiscall, ECX=adjusted entity, int state)
static constexpr uintptr_t kGetFoleyFX      = 0x0040E1DD; // GetFoleyFX thunk (__thiscall, ECX=adjusted entity) -> FoleyFX*
static constexpr uintptr_t kGameSoundPlay   = 0x00415451; // GameSound::Play thunk (__thiscall, ECX=GameSound*)
static constexpr uintptr_t kWeaponPlayFoley = 0x0040948F; // Weapon::PlayFoleyFX thunk (__thiscall, ECX=weapon, int type)
static constexpr uintptr_t kProneVtableSlot = 0x00A40718; // Controllable vtable Prone slot (offset 0xA0)
static constexpr uintptr_t kAnimAccessor    = 0x005701F0; // Animation accessor — lacks null param check
static constexpr uintptr_t kSetAction       = 0x00575D50; // SoldierAnimator::SetAction (__thiscall)
static constexpr uintptr_t kProneGuardJnz   = 0x00545BA6; // JNZ in EntitySoldier::Update that kicks out of PRONE

// SoldierState enum values
static constexpr int STATE_STAND  = 0;
static constexpr int STATE_CROUCH = 1;
static constexpr int STATE_PRONE  = 2;

// Controllable struct offsets (from Controllable ptr)
static constexpr int kMState      = 0x514; // int mState (SoldierState)
static constexpr int kWeaponSlot  = 0x512; // int8_t active weapon slot index
static constexpr int kWeaponArray = 0x4F0; // Weapon*[8]

// SoldierAnimator struct offsets
static constexpr int kSoldierAction = 0x70;   // SoldierState mSoldierAction
static constexpr int kMAction       = 0x1FEC; // ActionAnimation mAction
static constexpr int kMPosture      = 0x1FE8; // Posture mPosture

// ActionAnimation enum values for prone transitions (confirmed from SetupPose switch table)
static constexpr int ACTION_STAND_TO_PRONE  = 26; // 0x1A
static constexpr int ACTION_PRONE_TO_STAND  = 27; // 0x1B
static constexpr int ACTION_CROUCH_TO_PRONE = 28; // 0x1C
static constexpr int ACTION_PRONE_TO_CROUCH = 29; // 0x1D

// ---------------------------------------------------------------------------
// Function pointer types
// ---------------------------------------------------------------------------

// __fastcall mirrors __thiscall for Detours: ECX=this, EDX=unused
typedef bool (__fastcall* fn_Stance_t)(void* ecx, void* edx);

// Pure __thiscall — ECX=this, params on stack
typedef bool (__thiscall* fn_SetState_t)(void* adjusted, int state);
typedef void* (__thiscall* fn_GetFoleyFX_t)(void* adjusted);
typedef void (__thiscall* fn_GameSoundPlay_t)(void* gs, void* pos1, void* pos2, int a, int b);
typedef void (__thiscall* fn_WeaponPlayFoley_t)(void* weapon, int type);

// Animation accessor at 0x005701F0: reads (*param)->field_8, returns ushort.
// Crashes when param (ECX) is null — happens when PRONE animation data is missing.
typedef unsigned short (__fastcall* fn_AnimAccessor_t)(void* ecx, void* edx);

// SoldierAnimator::SetAction — called once per frame from EntitySoldier::Render.
// Sets mPosture, mAction, mSoldierAction based on the entity's current SoldierState.
typedef void (__fastcall* fn_SetAction_t)(void* ecx, void* edx, int param_2, void* param_3, unsigned int param_4);

// ---------------------------------------------------------------------------
// Resolved pointers (set during install)
// ---------------------------------------------------------------------------
static fn_Stance_t           original_Crouch    = nullptr;
static fn_Stance_t           original_StandUp   = nullptr;
static fn_SetState_t         fn_setState        = nullptr;
static fn_GetFoleyFX_t       fn_getFoleyFX      = nullptr;
static fn_GameSoundPlay_t    fn_gameSoundPlay   = nullptr;
static fn_WeaponPlayFoley_t  fn_weaponPlayFoley = nullptr;
static fn_AnimAccessor_t     original_animAccessor = nullptr;
static fn_SetAction_t        original_SetAction    = nullptr;

// Vtable patch state
static void** g_proneVtableSlotPtr  = nullptr;
static void*  g_proneVtableSlotOrig = nullptr;

// Runtime toggle (default: enabled)
static bool g_proneEnabled = true;


// ---------------------------------------------------------------------------
// do_prone_transition — enters prone state, plays sounds
//
// Modeled on Crouch inner (0x00543B60):
//   1. SetState(adjusted_entity, PRONE)
//   2. Play soldier FoleyFX stance sound
//   3. Play weapon FoleyFX
// ---------------------------------------------------------------------------
static bool do_prone_transition(void* controllable)
{
    char* adjusted = (char*)controllable - 0x240;

    if (!fn_setState) {
        if (g_gameLog) g_gameLog("[Prone] do_prone: fn_setState is null!\n");
        return false;
    }
    bool ok = fn_setState(adjusted, STATE_PRONE);
    if (!ok) return false;

    __try {
        // Soldier foley: FoleyFX has embedded GameSound structs at 0x10 stride.
        // Crouch-down sound = foley+0xC8.  Prone sound = foley+0xD8 (next slot).
        // If the offset is wrong, GameSound::Play on an empty/null slot is harmless.
        if (fn_getFoleyFX && fn_gameSoundPlay) {
            void* foley = fn_getFoleyFX(adjusted);
            if (foley) {
                void* sound = (char*)foley + 0xD8;
                void* pos1  = (char*)controllable - 0x120; // world pos (struct_base + 0x120)
                void* pos2  = (char*)controllable + 0x2AC;
                fn_gameSoundPlay(sound, pos1, pos2, 0, 1);
            }
        }

        // Weapon foley on stance change
        if (fn_weaponPlayFoley) {
            int8_t slot = *(int8_t*)((char*)controllable + kWeaponSlot);
            if (slot >= 0) {
                void* weapon = *(void**)((char*)controllable + kWeaponArray + slot * 4);
                if (weapon) fn_weaponPlayFoley(weapon, 8);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    return true;
}

// ---------------------------------------------------------------------------
// hooked_Crouch — called when going from STAND -> CROUCH.
//
// The game's crouch key is a TOGGLE: pressing it while standing calls
// Crouch (this function), pressing it while crouching calls StandUp.
// So this hook only fires for STAND->CROUCH.  We keep it as a fallback
// in case other code paths call Crouch from unexpected states.
// ---------------------------------------------------------------------------
static bool __fastcall hooked_Crouch(void* ecx, void* /*edx*/)
{
    if (!g_proneEnabled)
        return original_Crouch(ecx, nullptr);

    int state = *(int*)((char*)ecx + kMState);

    if (state == STATE_PRONE) {
        // Fallback: if something calls Crouch while prone, stand up.
        return original_StandUp(ecx, nullptr);
    }

    return original_Crouch(ecx, nullptr);
}

// ---------------------------------------------------------------------------
// hooked_StandUp — called when the crouch key is pressed while crouched
// (the game treats crouch as a toggle: stand->Crouch(), crouch->StandUp()).
//
// We intercept this to cycle:
//   CROUCH -> PRONE  (new — redirect the "stand up" into prone)
//   PRONE  -> STAND  (original StandUp handles headroom + crouch fallback)
// ---------------------------------------------------------------------------
static bool __fastcall hooked_StandUp(void* ecx, void* /*edx*/)
{
    if (!g_proneEnabled)
        return original_StandUp(ecx, nullptr);

    int state = *(int*)((char*)ecx + kMState);

    if (state == STATE_CROUCH) {
        // CROUCH -> PRONE: intercept the toggle and enter prone instead
        return do_prone_transition(ecx);
    }

    // PRONE -> STAND (or any other state): let original handle it.
    // StandUp already has headroom ray cast checks for prone->stand,
    // with automatic fallback to crouch if ceiling is too low.
    return original_StandUp(ecx, nullptr);
}

// ---------------------------------------------------------------------------
// hooked_animAccessor — null-guard for the animation accessor at 0x005701F0.
//
// The original function dereferences ECX without checking for null.  When the
// soldier is in PRONE state but prone animations aren't loaded, the animation
// system passes a null SoldierAnimation* here and crashes.  Returning 0
// (same as the function's own fallback for *param==0) lets the animation
// system degrade gracefully instead of crashing.
// ---------------------------------------------------------------------------
static unsigned short __fastcall hooked_animAccessor(void* ecx, void* /*edx*/)
{
    if (!ecx) return 0;
    return original_animAccessor(ecx, nullptr);
}

// ---------------------------------------------------------------------------
// hooked_SetAction — fixes prone transition animations.
//
// Pandemic defined ActionAnimation values for prone transitions
// (CROUCH_TO_PRONE, PRONE_TO_STAND, PRONE_TO_CROUCH) and wired up the
// playback side in SetupPose, but SetAction never writes them — it uses
// MOVE or LAND_HARD instead.  This post-hook overrides mAction with the
// correct transition value so the animation system plays the real
// transition animations.
// ---------------------------------------------------------------------------
static void __fastcall hooked_SetAction(void* ecx, void* edx, int param_2, void* param_3, unsigned int param_4)
{
    if (!g_proneEnabled) {
        original_SetAction(ecx, edx, param_2, param_3, param_4);
        return;
    }

    // Save old soldier action before original overwrites it
    int oldState = *(int*)((char*)ecx + kSoldierAction);

    // Let original SetAction run (sets mPosture, mAction, mSoldierAction)
    original_SetAction(ecx, edx, param_2, param_3, param_4);

    // Only fix up if the state actually changed and involves PRONE
    if (oldState == param_2) return;

    int* pAction = (int*)((char*)ecx + kMAction);

    if (oldState == STATE_CROUCH && param_2 == STATE_PRONE) {
        if (g_gameLog) g_gameLog("[Prone] SetAction: CROUCH->PRONE, mAction %d->%d\n", *pAction, ACTION_CROUCH_TO_PRONE);
        *pAction = ACTION_CROUCH_TO_PRONE;
    } else if (oldState == STATE_PRONE && param_2 == STATE_STAND) {
        if (g_gameLog) g_gameLog("[Prone] SetAction: PRONE->STAND, mAction %d->%d\n", *pAction, ACTION_PRONE_TO_STAND);
        *pAction = ACTION_PRONE_TO_STAND;
    } else if (oldState == STATE_PRONE && param_2 == STATE_CROUCH) {
        if (g_gameLog) g_gameLog("[Prone] SetAction: PRONE->CROUCH, mAction %d->%d\n", *pAction, ACTION_PRONE_TO_CROUCH);
        *pAction = ACTION_PRONE_TO_CROUCH;
    }
}

// ---------------------------------------------------------------------------
// vtable_Prone — replaces the vanilla "return false" stub at vtable+0xA0
//
// Called by AI posture system or any code that explicitly invokes Prone().
// ---------------------------------------------------------------------------
static bool __fastcall vtable_Prone(void* ecx, void* /*edx*/)
{
    if (!g_proneEnabled) return false;
    return do_prone_transition(ecx);
}

// ---------------------------------------------------------------------------
// Lua API: EnableProne(0/1)
// ---------------------------------------------------------------------------
int lua_EnableProne(lua_State* L)
{
    int arg = (int)g_lua.tonumber(L, 1);
    g_proneEnabled = (arg != 0);
    if (g_gameLog) g_gameLog("[Prone] %s\n", g_proneEnabled ? "enabled" : "disabled");
    return 0;
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------
void prone_system_install(uintptr_t exe_base)
{
    OutputDebugStringA("[Prone] prone_system_install called\n");
    g_gameLog = get_gamelog_local();
    if (g_gameLog) g_gameLog("[Prone] install starting, exe_base=0x%08x\n", (unsigned)exe_base);

    // Resolve function pointers
    original_Crouch    = (fn_Stance_t)resolve(exe_base, kCrouchInner);
    original_StandUp   = (fn_Stance_t)resolve(exe_base, kStandUpInner);
    fn_setState        = (fn_SetState_t)resolve(exe_base, kSetState);
    fn_getFoleyFX      = (fn_GetFoleyFX_t)resolve(exe_base, kGetFoleyFX);
    fn_gameSoundPlay   = (fn_GameSoundPlay_t)resolve(exe_base, kGameSoundPlay);
    fn_weaponPlayFoley = (fn_WeaponPlayFoley_t)resolve(exe_base, kWeaponPlayFoley);

    // Detour Crouch, StandUp, and the animation accessor.
    // Crouch/StandUp: the game's crouch key is a toggle — STAND->Crouch(),
    // CROUCH->StandUp().  We hook StandUp to intercept CROUCH->STAND and
    // redirect to PRONE.
    // AnimAccessor: null-guard to prevent crashes when prone animations
    // aren't loaded (the original dereferences ECX without a null check).
    original_animAccessor = (fn_AnimAccessor_t)resolve(exe_base, kAnimAccessor);
    original_SetAction    = (fn_SetAction_t)resolve(exe_base, kSetAction);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG r1 = DetourAttach(&(PVOID&)original_Crouch, hooked_Crouch);
    LONG r2 = DetourAttach(&(PVOID&)original_StandUp, hooked_StandUp);
    LONG r3 = DetourAttach(&(PVOID&)original_animAccessor, hooked_animAccessor);
    LONG r4 = DetourAttach(&(PVOID&)original_SetAction, hooked_SetAction);
    LONG rc = DetourTransactionCommit();
    if (g_gameLog) g_gameLog("[Prone] Detour Crouch=%ld StandUp=%ld AnimAcc=%ld SetAction=%ld commit=%ld\n",
                              r1, r2, r3, r4, rc);

    // Patch out the PRONE guard in EntitySoldier::Update.
    // Pandemic left a hardcoded check: if (mState == PRONE) Crouch();
    // that kicks the soldier out of prone every frame.
    // Change the JNZ (0x75) to JMP (0xEB) so the Crouch() call is always skipped.
    {
        uint8_t* pJnz = (uint8_t*)resolve(exe_base, kProneGuardJnz);
        if (*pJnz == 0x75) {
            *pJnz = 0xEB; // JNZ -> JMP (unconditional skip)
            if (g_gameLog) g_gameLog("[Prone] patched PRONE guard at 0x%08x: JNZ->JMP\n",
                                      (unsigned)(uintptr_t)pJnz);
        } else {
            if (g_gameLog) g_gameLog("[Prone] WARNING: expected JNZ (0x75) at 0x%08x, got 0x%02x\n",
                                      (unsigned)(uintptr_t)pJnz, *pJnz);
        }
    }

    // Patch Controllable vtable: Prone slot (offset 0xA0) currently points to
    // a "return false" stub.  Replace with our real implementation.
    // dllmain.cpp has already set all sections PAGE_READWRITE.
    g_proneVtableSlotPtr = (void**)resolve(exe_base, kProneVtableSlot);
    g_proneVtableSlotOrig = *g_proneVtableSlotPtr;
    *g_proneVtableSlotPtr = (void*)&vtable_Prone;
    if (g_gameLog) g_gameLog("[Prone] vtable Prone patched: 0x%08x -> 0x%08x\n",
                              (uintptr_t)g_proneVtableSlotOrig, (uintptr_t)&vtable_Prone);
}

void prone_system_uninstall()
{
    // Restore vtable entry
    if (g_proneVtableSlotPtr && g_proneVtableSlotOrig) {
        DWORD oldProt;
        if (VirtualProtect(g_proneVtableSlotPtr, sizeof(void*), PAGE_READWRITE, &oldProt)) {
            *g_proneVtableSlotPtr = g_proneVtableSlotOrig;
            VirtualProtect(g_proneVtableSlotPtr, sizeof(void*), oldProt, &oldProt);
        }
    }

    // Detach hooks
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (original_Crouch)       DetourDetach(&(PVOID&)original_Crouch, hooked_Crouch);
    if (original_StandUp)      DetourDetach(&(PVOID&)original_StandUp, hooked_StandUp);
    if (original_animAccessor) DetourDetach(&(PVOID&)original_animAccessor, hooked_animAccessor);
    if (original_SetAction)    DetourDetach(&(PVOID&)original_SetAction, hooked_SetAction);
    DetourTransactionCommit();
}
