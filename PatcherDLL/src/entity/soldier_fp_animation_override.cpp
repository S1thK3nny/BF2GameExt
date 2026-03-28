#include "pch.h"
#include "soldier_fp_animation_override.hpp"
#include "core/resolve.hpp"

#include <cstring>
#include <detours.h>

// =============================================================================
// First-Person Animation Bank Override
//
// Allows soldier classes to specify a custom FP animation bank via ODF:
//   FirstPersonAnimationBank = sbdroidfp
//
// Two hooks:
//   1. EntitySoldierClass::SetProperty — intercepts the custom property and
//      stores a class->bank mapping.
//   2. FirstPersonRenderable::UpdateSoldier — swaps the global mAnim[48]
//      array with the custom bank's animations for the duration of the call.
//
// Animations not found in the custom bank fall through to the default
// (humanfp / droidekafp).
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

// ---------------------------------------------------------------------------
// Resolved function pointers
// ---------------------------------------------------------------------------

static fn_hash_string_t   fn_hash_string   = nullptr;
static fn_AddBank_t       fn_AddBank       = nullptr;
static fn_FindAnimation_t fn_FindAnimation = nullptr;

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
   void*  anims[kAnimCount];  // ZephyrAnim*, nullptr = use default
   bool   loaded;
};

static FPBankEntry  g_classBanks[kMaxClassBanks] = {};
static int          g_classBankCount = 0;

static FPAnimCache  g_bankCaches[kMaxBankCaches] = {};
static int          g_bankCacheCount = 0;

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
   g_bankCaches[idx].loaded = false;
   return idx;
}

// ---------------------------------------------------------------------------
// Hook: EntitySoldierClass::SetProperty
// ---------------------------------------------------------------------------

static void __fastcall hooked_SetProperty(void* ecx, void* /*edx*/,
                                          unsigned int hash, const char* value)
{
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
   auto log = get_gamelog();

   uint32_t addResult = fn_AddBank(cache->bankName);
   if (!(addResult & 1)) {
      log("[FPAnimBank] AddBank('%s') FAILED (0x%08x)\n", cache->bankName, addResult);
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

      void* anim = fn_FindAnimation(newName);
      if (anim) {
         cache->anims[i] = anim;
      }
   }

   cache->loaded = true;

   int count = 0;
   for (int i = 0; i < kAnimCount; i++) {
      if (cache->anims[i]) count++;
   }
   // Only log if something went wrong (partial load)
   if (count == 0) {
      log("[FPAnimBank] Bank '%s': NO animations resolved (0/%d)\n",
          cache->bankName, kAnimCount);
   }
}

// ---------------------------------------------------------------------------
// Hook: FirstPersonRenderable::UpdateSoldier
// ---------------------------------------------------------------------------

static void __fastcall hooked_UpdateSoldier(void* ecx, void* /*edx*/,
                                            void* model, void* ctrl, void* aimer)
{
   if (!ctrl || g_classBankCount == 0) {
      original_UpdateSoldier(ecx, nullptr, model, ctrl, aimer);
      return;
   }

   FPAnimCache* cache = nullptr;

   __try {
      // The param is the 'intermediate' pointer (charSlot+0x148).
      // EntitySoldierClass* is at intermediate + 0x218.
      void* entityClass = *(void**)((uintptr_t)ctrl + 0x218);
      if (!entityClass || entityClass == (void*)0xFFFFFFFF) {
         original_UpdateSoldier(ecx, nullptr, model, ctrl, aimer);
         return;
      }

      for (int i = 0; i < g_classBankCount; i++) {
         if (g_classBanks[i].classPtr == entityClass) {
            int bankIdx = g_classBanks[i].bankIndex;
            if (bankIdx >= 0 && bankIdx < g_bankCacheCount) {
               cache = &g_bankCaches[bankIdx];
            }
            break;
         }
      }
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
   }

   if (!cache) {
      original_UpdateSoldier(ecx, nullptr, model, ctrl, aimer);
      return;
   }

   // Lazy load on first use
   if (!cache->loaded) {
      loadBankCache(cache);
   }

   // Swap: save global mAnim[], overlay custom anims (non-null only)
   void* saved[kAnimCount];
   memcpy(saved, g_mAnim, sizeof(saved));

   for (int i = 0; i < kAnimCount; i++) {
      if (cache->anims[i]) {
         g_mAnim[i] = cache->anims[i];
      }
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
   using namespace game_addrs::modtools;
   fn_hash_string   = (fn_hash_string_t)  resolve(exe_base, hash_string);
   fn_AddBank       = (fn_AddBank_t)      resolve(exe_base, anim_add_bank);
   fn_FindAnimation = (fn_FindAnimation_t)resolve(exe_base, anim_find_animation);

   g_mAnim         = (void**)       resolve(exe_base, fp_anim_array);
   g_animNameTable = (const char**) resolve(exe_base, anim_name_table);

   g_fpBankPropHash = fn_hash_string("FirstPersonAnimationBank");

   original_SetProperty   = (fn_SetProperty_t)  resolve(exe_base, fp_anim_set_property);
   original_UpdateSoldier = (fn_UpdateSoldier_t) resolve(exe_base, fp_update_soldier);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   LONG r1 = DetourAttach(&(PVOID&)original_SetProperty,  hooked_SetProperty);
   LONG r2 = DetourAttach(&(PVOID&)original_UpdateSoldier, hooked_UpdateSoldier);
   LONG rc = DetourTransactionCommit();

   (void)r1; (void)r2; (void)rc;
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
}
