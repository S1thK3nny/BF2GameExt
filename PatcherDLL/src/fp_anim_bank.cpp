#include "pch.h"
#include "fp_anim_bank.hpp"

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

static constexpr uintptr_t kUnrelocatedBase = 0x400000u;

static inline void* resolve(uintptr_t exe_base, uintptr_t unrelocated_addr)
{
   return (void*)((unrelocated_addr - kUnrelocatedBase) + exe_base);
}

typedef void(__cdecl* GameLog_t)(const char* fmt, ...);
static GameLog_t get_gamelog()
{
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   return (GameLog_t)((0x7E3D50 - kUnrelocatedBase) + base);
}

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

// ---------------------------------------------------------------------------
// Addresses (BF2_modtools, imagebase 0x400000)
// ---------------------------------------------------------------------------

static constexpr uintptr_t kSetProperty_addr   = 0x0053FA20;
static constexpr uintptr_t kUpdateSoldier_addr = 0x004A9BE0;
static constexpr uintptr_t kAddBank_addr       = 0x004A8FC0;
static constexpr uintptr_t kFindAnimation_addr = 0x004A7900;
static constexpr uintptr_t kHashString_addr    = 0x007E1B70;
static constexpr uintptr_t kMAnim_addr         = 0x00B70E30;  // ZephyrAnim*[48]
static constexpr uintptr_t kAnimNameTable_addr = 0x00A36C88;  // const char*[48]

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
// One-shot logging throttle for UpdateSoldier (avoid flooding)
// ---------------------------------------------------------------------------

static bool g_loggedUpdateOnce = false;

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
   get_gamelog()("[FPAnimBank] Created bank cache [%d] for '%s'\n", idx, bankName);
   return idx;
}

// ---------------------------------------------------------------------------
// Hook: EntitySoldierClass::SetProperty
// ---------------------------------------------------------------------------

static void __fastcall hooked_SetProperty(void* ecx, void* /*edx*/,
                                          unsigned int hash, const char* value)
{
   if (hash == g_fpBankPropHash && g_fpBankPropHash != 0) {
      auto log = get_gamelog();
      log("[FPAnimBank] SetProperty HIT: class=%p value='%s'\n", ecx, value ? value : "(null)");

      if (!value || value[0] == '\0') {
         log("[FPAnimBank]   -> empty value, ignoring\n");
         return;
      }

      int bankIdx = findOrCreateBankCache(value);
      if (bankIdx < 0) return;

      // Check if this class already has a mapping
      for (int i = 0; i < g_classBankCount; i++) {
         if (g_classBanks[i].classPtr == ecx) {
            g_classBanks[i].bankIndex = bankIdx;
            log("[FPAnimBank]   -> updated existing class mapping [%d] -> cache[%d]\n", i, bankIdx);
            return;
         }
      }

      // New mapping
      if (g_classBankCount >= kMaxClassBanks) {
         log("[FPAnimBank]   -> class table full (%d)\n", kMaxClassBanks);
         return;
      }
      g_classBanks[g_classBankCount].classPtr  = ecx;
      g_classBanks[g_classBankCount].bankIndex = bankIdx;
      g_classBankCount++;
      log("[FPAnimBank]   -> new class mapping [%d]: class=%p -> bank '%s' (cache[%d])\n",
          g_classBankCount - 1, ecx, value, bankIdx);
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

   log("[FPAnimBank] Loading bank '%s'...\n", cache->bankName);

   // Register the bank with FirstPerson system so FindAnimation can search it.
   // AddBank returns bool in low bit: 1 = success.
   uint32_t addResult = fn_AddBank(cache->bankName);
   log("[FPAnimBank]   AddBank('%s') returned 0x%08x (%s)\n",
       cache->bankName, addResult, (addResult & 1) ? "OK" : "FAILED");

   int bankNameLen = (int)strlen(cache->bankName);
   char newName[128];

   for (int i = 0; i < kAnimCount; i++) {
      const char* origName = g_animNameTable[i];
      if (!origName || origName[0] == '\0') {
         log("[FPAnimBank]   slot[%2d]: (null/empty) -> skip\n", i);
         continue;
      }

      // Determine the prefix to strip
      int prefixLen = (i >= kDroidekaFirstSlot) ? kDroidekaFPPrefixLen
                                                : kHumanFPPrefixLen;

      int origLen = (int)strlen(origName);
      if (origLen <= prefixLen) {
         log("[FPAnimBank]   slot[%2d]: '%s' too short for prefix, skip\n", i, origName);
         continue;
      }

      // suffix includes the underscore, e.g. "_rifle_idle"
      const char* suffix = origName + prefixLen;
      int suffixLen = origLen - prefixLen;

      if (bankNameLen + suffixLen >= (int)sizeof(newName)) {
         log("[FPAnimBank]   slot[%2d]: name too long, skip\n", i);
         continue;
      }

      memcpy(newName, cache->bankName, bankNameLen);
      memcpy(newName + bankNameLen, suffix, suffixLen + 1);

      void* anim = fn_FindAnimation(newName);
      if (anim) {
         cache->anims[i] = anim;
         log("[FPAnimBank]   slot[%2d]: '%s' -> %p (OK)\n", i, newName, anim);
      } else {
         log("[FPAnimBank]   slot[%2d]: '%s' -> NOT FOUND (will use default)\n", i, newName);
      }
   }

   cache->loaded = true;

   int count = 0;
   for (int i = 0; i < kAnimCount; i++) {
      if (cache->anims[i]) count++;
   }
   log("[FPAnimBank] Bank '%s' load complete: %d/%d animations resolved\n",
       cache->bankName, count, kAnimCount);
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
      // The param passed to UpdateSoldier is the 'intermediate' pointer
      // (charSlot+0x148). EntitySoldierClass* is at intermediate + 0x218.
      void* entityClass = *(void**)((uintptr_t)ctrl + 0x218);
      if (!entityClass || entityClass == (void*)0xFFFFFFFF) {
         original_UpdateSoldier(ecx, nullptr, model, ctrl, aimer);
         return;
      }

      if (!g_loggedUpdateOnce) {
         get_gamelog()("[FPAnimBank] UpdateSoldier: soldier=%p entityClass=%p (searching %d classes)\n",
                       ctrl, entityClass, g_classBankCount);
         for (int j = 0; j < g_classBankCount; j++) {
            get_gamelog()("[FPAnimBank]   registered[%d]: classPtr=%p bankIdx=%d\n",
                          j, g_classBanks[j].classPtr, g_classBanks[j].bankIndex);
         }
      }

      for (int i = 0; i < g_classBankCount; i++) {
         if (g_classBanks[i].classPtr == entityClass) {
            int bankIdx = g_classBanks[i].bankIndex;
            if (bankIdx >= 0 && bankIdx < g_bankCacheCount) {
               cache = &g_bankCaches[bankIdx];
            }
            if (!g_loggedUpdateOnce) {
               get_gamelog()("[FPAnimBank]   -> MATCH at [%d], bankIdx=%d, cache=%p\n",
                             i, bankIdx, cache);
            }
            break;
         }
      }

      if (!cache && !g_loggedUpdateOnce) {
         get_gamelog()("[FPAnimBank]   -> no match for entityClass=%p\n", entityClass);
      }
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      if (!g_loggedUpdateOnce) {
         get_gamelog()("[FPAnimBank] UpdateSoldier: EXCEPTION during pointer navigation\n");
      }
   }

   g_loggedUpdateOnce = true;

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

   int swapped = 0;
   for (int i = 0; i < kAnimCount; i++) {
      if (cache->anims[i]) {
         g_mAnim[i] = cache->anims[i];
         swapped++;
      }
   }

   __try {
      original_UpdateSoldier(ecx, nullptr, model, ctrl, aimer);
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      get_gamelog()("[FPAnimBank] UpdateSoldier CRASHED during original call\n");
   }

   // Restore original mAnim[]
   memcpy(g_mAnim, saved, sizeof(saved));
}

// ---------------------------------------------------------------------------
// Install / Uninstall / Reset
// ---------------------------------------------------------------------------

void fp_anim_bank_install(uintptr_t exe_base)
{
   fn_hash_string   = (fn_hash_string_t)  resolve(exe_base, kHashString_addr);
   fn_AddBank       = (fn_AddBank_t)      resolve(exe_base, kAddBank_addr);
   fn_FindAnimation = (fn_FindAnimation_t)resolve(exe_base, kFindAnimation_addr);

   g_mAnim         = (void**)       resolve(exe_base, kMAnim_addr);
   g_animNameTable = (const char**) resolve(exe_base, kAnimNameTable_addr);

   g_fpBankPropHash = fn_hash_string("FirstPersonAnimationBank");

   original_SetProperty   = (fn_SetProperty_t)  resolve(exe_base, kSetProperty_addr);
   original_UpdateSoldier = (fn_UpdateSoldier_t) resolve(exe_base, kUpdateSoldier_addr);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   LONG r1 = DetourAttach(&(PVOID&)original_SetProperty,  hooked_SetProperty);
   LONG r2 = DetourAttach(&(PVOID&)original_UpdateSoldier, hooked_UpdateSoldier);
   LONG rc = DetourTransactionCommit();

   auto log = get_gamelog();
   log("[FPAnimBank] Install: SetProperty=%ld UpdateSoldier=%ld commit=%ld hash=0x%08x\n",
       r1, r2, rc, g_fpBankPropHash);
   log("[FPAnimBank]   g_mAnim=%p  g_animNameTable=%p\n", g_mAnim, g_animNameTable);

   // Dump the name table so we can verify it's resolved correctly
   for (int i = 0; i < kAnimCount; i++) {
      const char* name = g_animNameTable[i];
      if (name && name[0] != '\0') {
         log("[FPAnimBank]   nameTable[%2d] = '%s'\n", i, name);
      }
   }
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
   get_gamelog()("[FPAnimBank] Reset: clearing %d class mappings, %d bank caches\n",
                 g_classBankCount, g_bankCacheCount);
   memset(g_classBanks, 0, sizeof(g_classBanks));
   g_classBankCount = 0;
   memset(g_bankCaches, 0, sizeof(g_bankCaches));
   g_bankCacheCount = 0;
   g_loggedUpdateOnce = false;
}
