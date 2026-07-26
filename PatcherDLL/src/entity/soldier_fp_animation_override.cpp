#include "pch.h"
#include "soldier_fp_animation_override.hpp"
#include "core/resolve.hpp"

#include <cstring>
#include <detours.h>

// =============================================================================
// First-Person Animation Bank Override + FP Sprint Animation
//
// Allows soldier classes to specify a custom FP animation bank via ODF:
//   FirstPersonAnimationBank = sbdroidfp
//
// Two hooks:
//   1. EntitySoldierClass::SetProperty — intercepts the custom property and
//      stores a class->bank mapping.
//   2. FirstPersonRenderable::UpdateSoldier — swaps the global mAnim[48]
//      array with the custom bank's animations for the duration of the call.
//      Also detects sprint state and substitutes _sprint animations for _run.
//
// Animations not found in the custom bank fall through to the default
// (humanfp / droidekafp).
//
// FP Sprint:
//   The engine's FP state machine has no sprint state — it uses state 1 (run)
//   for all forward movement and just scales playback rate. Sprint is detected
//   by reading EntitySoldier::mState (== 3 when sprinting, set by
//   EntitySoldier::Sprint); see s_stateOffset for the per-build offset.
//   When sprinting, the _run animation slot is temporarily swapped with a
//   _sprint animation (e.g. humanfp_rifle_sprint) before calling UpdateSoldier.
//   Sprint animations are optional — if absent, the engine falls back to _run.
// =============================================================================

// ---------------------------------------------------------------------------
// Game function types
// ---------------------------------------------------------------------------

using fn_hash_string_t    = uint32_t(__cdecl*)(const char*);
using fn_AddBank_t        = uint32_t(__cdecl*)(const char*);     // returns bool (low bit)
using fn_FindAnimation_t  = void*(__cdecl*)(const char*);        // returns ZephyrAnim*

// EntitySoldierClass::SetProperty — __thiscall(this, uint hash, const char* value)
using fn_SetProperty_t = void(__fastcall*)(void* ecx, void* edx,
                                           unsigned int hash, const char* value);

// FirstPersonRenderable::UpdateSoldier — __thiscall(this, RedModel*, Controllable*, Aimer*)
using fn_UpdateSoldier_t = void(__fastcall*)(void* ecx, void* edx,
                                             void* model, void* ctrl, void* aimer);


static constexpr int kAnimCount           = 48;
static constexpr int kHumanFPPrefixLen    = 7;   // strlen("humanfp")
static constexpr int kDroidekaFPPrefixLen = 10;  // strlen("droidekafp")
static constexpr int kDroidekaFirstSlot   = 44;

// FP sprint constants
static constexpr int kStatesPerWeapon     = 11;
static constexpr int kHumanWeaponClasses  = 4;
static constexpr int kRunState            = 1;    // FP state index for run
static constexpr uint32_t kSprintActive   = 3;     // mState value when sprinting (build-invariant)

// Suffixes appended to bank names to find sprint animations
static const char* kSprintSuffixes[kHumanWeaponClasses] = {
   "_rifle_sprint",
   "_bazooka_sprint",
   "_tool_sprint",    // weapon class 2 (tool)
   "_tool_sprint",    // weapon class 3 (grenade — shares tool anims)
};

// ---------------------------------------------------------------------------
// Trampolines
// ---------------------------------------------------------------------------

static fn_SetProperty_t   original_SetProperty   = nullptr;
static fn_UpdateSoldier_t original_UpdateSoldier  = nullptr;

// ---------------------------------------------------------------------------
// Custom property hash (computed at install time)
// ---------------------------------------------------------------------------

static uint32_t g_fpBankPropHash = 0;

// ---------------------------------------------------------------------------
// Class -> Bank mapping
// ---------------------------------------------------------------------------

static constexpr int kMaxClassBanks = 64;
static constexpr int kMaxBankCaches = 16;

struct FPBankEntry {
   void* classPtr;    // EntitySoldierClass*
   int   bankIndex;   // index into g_bankCaches
};

struct FPAnimCache {
   char   bankName[64];
   void*  anims[kAnimCount];                    // ZephyrAnim*, nullptr = use default
   void*  sprintAnims[kHumanWeaponClasses];     // per-weapon-class sprint overrides
   bool   loaded;
};

static FPBankEntry  g_classBanks[kMaxClassBanks] = {};
static int          g_classBankCount = 0;

static FPAnimCache  g_bankCaches[kMaxBankCaches] = {};
static int          g_bankCacheCount = 0;

// ---------------------------------------------------------------------------
// Default (humanfp) sprint animations — loaded lazily
// ---------------------------------------------------------------------------

static void*  g_defaultSprintAnims[kHumanWeaponClasses] = {};
static bool   g_defaultSprintLoaded = false;
static bool   g_wasSprinting = false;

// ---------------------------------------------------------------------------
// Resolved function pointers
// ---------------------------------------------------------------------------

static fn_hash_string_t   fn_hash_string   = nullptr;

// FirstPerson::AddBank / FindAnimation take their char* argument on the stack
// (__cdecl) on modtools, but in ECX on the release builds (Steam/GOG, LTCG).
// Store the resolved address plus a convention flag and marshal at the call site
// (fn_AddBank_t / fn_FindAnimation_t above name the modtools __cdecl shape).
static uintptr_t s_addBankAddr   = 0;
static uintptr_t s_findAnimAddr  = 0;
static bool      s_ecxConvention = false;  // true = arg in ECX (release builds)

// Offsets of the soldier's class pointer and mState, RELATIVE to the
// UpdateSoldier `ctrl`.  `ctrl` is the Controllable subobject at entity+0x240 on BOTH builds
// (EntitySoldier ctor: modtools, and Steam `LEA ECX,[EDI+0x240]` @ 0x4de8ad).
// What differs is where the class pointer and mState sit inside EntitySoldier:
//   modtools: class = ctrl+0x218 (entity+0x458), state = ctrl+0x514
//   Steam:    class = ctrl-0x238 (GameObject::mClass @ entity+0x8 — stored by
//                                 the root base ctor 0x491720 `MOV [ESI+8],EAX`
//                                 from the EntitySoldierClass* threaded down
//                                 EntitySoldier ctor 0x4de850 -> 0x5350d0 ->
//                                 0x4ba0f0; runtime scan confirmed the match),
//             state = ctrl+0x504 (EntitySoldier::mState @ entity+0x744, per
//                                 SetState 0x4ee2c0 `MOV EAX,[ESI+0x744]`).
static intptr_t  s_classOffset = 0x218;
static uintptr_t s_stateOffset = 0x514;

static uint32_t call_add_bank(const char* name)
{
   if (!s_addBankAddr) return 0;
   return s_ecxConvention
      ? ((uint32_t(__fastcall*)(const char*))s_addBankAddr)(name)
      : ((fn_AddBank_t)s_addBankAddr)(name);
}

static void* call_find_animation(const char* name)
{
   if (!s_findAnimAddr) return nullptr;
   return s_ecxConvention
      ? ((void*(__fastcall*)(const char*))s_findAnimAddr)(name)
      : ((fn_FindAnimation_t)s_findAnimAddr)(name);
}

// ---------------------------------------------------------------------------
// Resolved global pointers
// ---------------------------------------------------------------------------

static void**        g_mAnim         = nullptr;  // -> mAnim[48]
static const char**  g_animNameTable = nullptr;   // -> s_AnimNameTable[48]

// ---------------------------------------------------------------------------
// Helper: find or create a bank cache entry by name
// ---------------------------------------------------------------------------

static int findOrCreateBankCache(const char* bankName)
{
   for (int i = 0; i < g_bankCacheCount; i++) {
      if (_stricmp(g_bankCaches[i].bankName, bankName) == 0)
         return i;
   }

   if (g_bankCacheCount >= kMaxBankCaches) {
      get_gamelog()("[FPAnimBank] Bank cache full (%d), ignoring '%s'\n",
                    kMaxBankCaches, bankName);
      return -1;
   }

   int idx = g_bankCacheCount++;
   strncpy_s(g_bankCaches[idx].bankName, sizeof(g_bankCaches[idx].bankName),
             bankName, _TRUNCATE);
   memset(g_bankCaches[idx].anims, 0, sizeof(g_bankCaches[idx].anims));
   memset(g_bankCaches[idx].sprintAnims, 0, sizeof(g_bankCaches[idx].sprintAnims));
   g_bankCaches[idx].loaded = false;
   return idx;
}

// ---------------------------------------------------------------------------
// Helper: load sprint animations for a given bank name
// ---------------------------------------------------------------------------

static void loadSprintAnims(const char* bankName, void* out[kHumanWeaponClasses])
{
   int bankNameLen = (int)strlen(bankName);
   char name[128];

   for (int wc = 0; wc < kHumanWeaponClasses; wc++) {
      int suffixLen = (int)strlen(kSprintSuffixes[wc]);
      if (bankNameLen + suffixLen >= (int)sizeof(name)) continue;

      memcpy(name, bankName, bankNameLen);
      memcpy(name + bankNameLen, kSprintSuffixes[wc], suffixLen + 1);

      out[wc] = call_find_animation(name);
   }
}

// ---------------------------------------------------------------------------
// Hook: EntitySoldierClass::SetProperty
// ---------------------------------------------------------------------------

// Compute the property-name hash lazily on first ODF parse. It cannot be done
// at install time: the install window runs with the exe sections marked
// non-executable (see dllmain), so calling game code (hash_string) faults under
// DEP on the retail builds.
static void ensurePropHash()
{
   if (g_fpBankPropHash == 0 && fn_hash_string)
      g_fpBankPropHash = fn_hash_string("FirstPersonAnimationBank");
}

static void __fastcall hooked_SetProperty(void* ecx, void* /*edx*/,
                                          unsigned int hash, const char* value)
{
   ensurePropHash();

   if (hash == g_fpBankPropHash && g_fpBankPropHash != 0) {
      if (!value || value[0] == '\0') return;

      int bankIdx = findOrCreateBankCache(value);
      if (bankIdx < 0) return;

      for (int i = 0; i < g_classBankCount; i++) {
         if (g_classBanks[i].classPtr == ecx) {
            g_classBanks[i].bankIndex = bankIdx;
            return;
         }
      }

      if (g_classBankCount >= kMaxClassBanks) {
         get_gamelog()("[FPAnimBank] Class table full (%d)\n", kMaxClassBanks);
         return;
      }
      g_classBanks[g_classBankCount].classPtr  = ecx;
      g_classBanks[g_classBankCount].bankIndex = bankIdx;
      g_classBankCount++;
      return;
   }

   original_SetProperty(ecx, nullptr, hash, value);
}

// ---------------------------------------------------------------------------
// Lazy bank loading — called on first UpdateSoldier encounter
// ---------------------------------------------------------------------------

static void loadBankCache(FPAnimCache* cache)
{
   uint32_t addResult = call_add_bank(cache->bankName);
   if (!(addResult & 1)) {
      get_gamelog()("[FPAnimBank] AddBank('%s') FAILED (0x%08x)\n", cache->bankName, addResult);
   }

   int bankNameLen = (int)strlen(cache->bankName);
   char newName[128];

   for (int i = 0; i < kAnimCount; i++) {
      const char* origName = g_animNameTable[i];
      if (!origName || origName[0] == '\0') continue;

      int prefixLen = (i >= kDroidekaFirstSlot) ? kDroidekaFPPrefixLen
                                                : kHumanFPPrefixLen;

      int origLen = (int)strlen(origName);
      if (origLen <= prefixLen) continue;

      const char* suffix = origName + prefixLen;
      int suffixLen = origLen - prefixLen;

      if (bankNameLen + suffixLen >= (int)sizeof(newName)) continue;

      memcpy(newName, cache->bankName, bankNameLen);
      memcpy(newName + bankNameLen, suffix, suffixLen + 1);

      void* anim = call_find_animation(newName);
      if (anim) {
         cache->anims[i] = anim;
      }
   }

   // Also load sprint animations for this bank
   loadSprintAnims(cache->bankName, cache->sprintAnims);

   cache->loaded = true;

   int count = 0;
   for (int i = 0; i < kAnimCount; i++) {
      if (cache->anims[i]) count++;
   }

   if (count == 0) {
      get_gamelog()("[FPAnimBank] Bank '%s': NO animations resolved (0/%d)\n",
                   cache->bankName, kAnimCount);
   }
}

// ---------------------------------------------------------------------------
// Hook: FirstPersonRenderable::UpdateSoldier
// ---------------------------------------------------------------------------

static void __fastcall hooked_UpdateSoldier(void* ecx, void* /*edx*/,
                                            void* model, void* ctrl, void* aimer)
{
   if (!ctrl) {
      original_UpdateSoldier(ecx, nullptr, model, ctrl, aimer);
      return;
   }

   FPAnimCache* cache = nullptr;
   bool isSprinting = false;

   __try {
      // Detect sprint: mState == 3
      uint32_t sprintState = *(uint32_t*)((uintptr_t)ctrl + s_stateOffset);
      isSprinting = (sprintState == kSprintActive);

      // Look up custom bank override
      if (g_classBankCount > 0) {
         void* entityClass = *(void**)((intptr_t)ctrl + s_classOffset);
         if (entityClass && entityClass != (void*)0xFFFFFFFF) {
            for (int i = 0; i < g_classBankCount; i++) {
               if (g_classBanks[i].classPtr == entityClass) {
                  int bankIdx = g_classBanks[i].bankIndex;
                  if (bankIdx >= 0 && bankIdx < g_bankCacheCount)
                     cache = &g_bankCaches[bankIdx];
                  break;
               }
            }
         }
      }
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
   }

   // Fast path: no bank override and not sprinting
   if (!cache && !isSprinting) {
      original_UpdateSoldier(ecx, nullptr, model, ctrl, aimer);
      return;
   }

   // Lazy-load bank if needed
   if (cache && !cache->loaded) {
      loadBankCache(cache);
   }

   // Lazy-load default sprint anims on first sprint
   if (isSprinting && !cache && !g_defaultSprintLoaded) {
      loadSprintAnims("humanfp", g_defaultSprintAnims);
      g_defaultSprintLoaded = true;
   }

   // Save global mAnim[], apply overrides, call original, restore
   void* saved[kAnimCount];
   memcpy(saved, g_mAnim, sizeof(saved));

   // Bank override: overlay custom anims (non-null only)
   if (cache) {
      for (int i = 0; i < kAnimCount; i++) {
         if (cache->anims[i])
            g_mAnim[i] = cache->anims[i];
      }
   }

   // Sprint override: swap _run slots with _sprint animations
   if (isSprinting) {
      void** sprintAnims = cache ? cache->sprintAnims : g_defaultSprintAnims;
      for (int wc = 0; wc < kHumanWeaponClasses; wc++) {
         if (sprintAnims[wc])
            g_mAnim[wc * kStatesPerWeapon + kRunState] = sprintAnims[wc];
      }
   }

   // The FP state machine only calls SetAnimation when the state changes.
   // Both running and sprinting map to state 1 (run), so transitioning between
   // them doesn't trigger a new SetAnimation call. Invalidate the cached FP
   // state (ecx+0x1608) when sprint status changes to force re-evaluation.
   if (isSprinting != g_wasSprinting) {
      *(int*)((uintptr_t)ecx + 0x1608) = -1;
      g_wasSprinting = isSprinting;
   }

   original_UpdateSoldier(ecx, nullptr, model, ctrl, aimer);

   // Restore original mAnim[]
   memcpy(g_mAnim, saved, sizeof(saved));
}

// ---------------------------------------------------------------------------
// Install / Uninstall / Reset
// ---------------------------------------------------------------------------

void fp_anim_bank_install(uintptr_t exe_base)
{
   // Build-aware. The FP renderable layout (mAnim[48], mCurrentWeapon +0x1600,
   // cached state +0x1608) and the EntitySoldier fields the UpdateSoldier hook
   // reads (mClass @ ctrl+0x218, mState @ ctrl+0x514) are build-invariant
   // (PDB-confirmed); only the addresses and the AddBank/FindAnimation calling
   // convention differ per build.
   if (g_build == GameBuild::Unknown)
      return;

   if (!g_addr->fp_update_soldier || !g_addr->soldier_class_set_property ||
       !g_addr->anim_add_bank || !g_addr->anim_find_animation ||
       !g_addr->fp_anim_array || !g_addr->anim_name_table || !g_addr->hash_string)
      return;

   fn_hash_string  = (fn_hash_string_t)resolve(exe_base, g_addr->hash_string);

   s_addBankAddr   = (uintptr_t)resolve(exe_base, g_addr->anim_add_bank);
   s_findAnimAddr  = (uintptr_t)resolve(exe_base, g_addr->anim_find_animation);
   s_ecxConvention = (g_build != GameBuild::Modtools);  // release passes arg in ECX

   if (g_build == GameBuild::Steam || g_build == GameBuild::GOG) {
      s_classOffset = -0x238;  // GameObject::mClass @ entity+0x8, ctrl @ entity+0x240
      s_stateOffset = 0x504;   // EntitySoldier::mState @ entity+0x744
   }

   g_mAnim         = (void**)       resolve(exe_base, g_addr->fp_anim_array);
   g_animNameTable = (const char**) resolve(exe_base, g_addr->anim_name_table);

   // g_fpBankPropHash is computed lazily in hooked_SetProperty (see ensurePropHash).

   original_SetProperty   = (fn_SetProperty_t)  resolve(exe_base, g_addr->soldier_class_set_property);
   original_UpdateSoldier = (fn_UpdateSoldier_t) resolve(exe_base, g_addr->fp_update_soldier);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_SetProperty,   hooked_SetProperty);
   DetourAttach(&(PVOID&)original_UpdateSoldier, hooked_UpdateSoldier);
   DetourTransactionCommit();
}

void fp_anim_bank_uninstall()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_SetProperty)  DetourDetach(&(PVOID&)original_SetProperty,  hooked_SetProperty);
   if (original_UpdateSoldier) DetourDetach(&(PVOID&)original_UpdateSoldier, hooked_UpdateSoldier);
   DetourTransactionCommit();
}

void fp_anim_bank_reset()
{
   memset(g_classBanks, 0, sizeof(g_classBanks));
   g_classBankCount = 0;
   memset(g_bankCaches, 0, sizeof(g_bankCaches));
   g_bankCacheCount = 0;
   memset(g_defaultSprintAnims, 0, sizeof(g_defaultSprintAnims));
   g_defaultSprintLoaded = false;
   g_wasSprinting = false;
}
