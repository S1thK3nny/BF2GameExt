#include "pch.h"
#include "prone_lvl_load.hpp"
#include "soldier_prone.hpp"
#include "core/resolve.hpp"
#include "lua/lua_hooks.hpp"

#pragma warning(disable: 4996) // _snprintf deprecation

#include <stdio.h>
#include <string.h>
#include <detours.h>

// =============================================================================
// Prone LVL auto-load.  See prone_lvl_load.hpp for the rationale.
//
// Two engine functions are involved:
//
//   Lua_Callbacks::ReadDataFile  (modtools 0x0046a790 / Steam 0x0058ac50)
//     The lua_CFunction bound to the script-side ReadDataFile().  Plain cdecl
//     on both builds, so it detours normally.
//
//   LoadUtil::ReadDataFile       (modtools 0x004538b0 / Steam 0x00579c30)
//     bool ReadDataFile(name, heapArg, hashes, hashCount, lvlNames, lvlCount).
//     This is the actual level reader.  It returns false when the file can't
//     be opened, which is a clean failure with no fatal dialog: it just tears
//     down its PblFile and returns.  Calling conventions differ.  On modtools
//     it is cdecl with six stack args, all zero for a plain whole-file read.
//     On Steam it is LTCG: the name arrives in ECX, four caller-cleaned stack
//     args follow (the first of which the function never reads), and it RETs 0.
//     That needs the naked thunk below.  Declaring it __fastcall would make the
//     compiler assume callee cleanup and leak 16 bytes of stack per call.
// =============================================================================

static const char kIngameLvl[] = "ingame.lvl";
static const char kProneLvl[]  = "prone.lvl";

// Low-res graft (see the graft section below): the vanilla human low-res bank,
// the donor bank shipped in prone.lvl, and the pose name soldier_prone.cpp
// patches into low-res name-table slot 2.
static const char kTargetBank[] = "humanlz";
static const char kDonorBank[]  = "pronelz";
static const char kProneAnim[]  = "rifle_prone_idle_emote";

using fn_lua_cb_t = int(__cdecl*)(lua_State* L);
using fn_read_data_file_mt_t =
    bool(__cdecl*)(const char* name, int a2, int a3, void* a4, unsigned a5, void* a6);
using fn_PostLoad_t  = void  (__cdecl*)(void);
using fn_PblHash_t   = void  (__thiscall*)(uint32_t* outHash, const char* str);
using fn_TempHash_t  = uint32_t(__cdecl*)(const char* str);
using fn_HashFind_t  = void* (__cdecl*)(void* table, int size, uint32_t hash);
using fn_HashStore_t = int   (__cdecl*)(void* table, int size, uint32_t hash, void* value);

static fn_lua_cb_t original_lua_read_data_file = nullptr;
static uintptr_t   s_read_data_file            = 0;   // LoadUtil::ReadDataFile

static fn_PostLoad_t  original_PostLoad = nullptr;
static fn_PblHash_t   fn_pblHash        = nullptr;
static fn_TempHash_t  fn_tempHash       = nullptr;
static fn_HashFind_t  fn_hashFind       = nullptr;
static fn_HashStore_t fn_hashStore      = nullptr;
static void*          g_animHashTable   = nullptr;

// [Features] Prone as configured in the INI.  g_proneEnabled is the *live*
// flag and we clear it when prone.lvl is missing, so the configured value has
// to be remembered separately to allow a later mission to re-enable prone.
static bool s_proneConfigured = false;

// ---------------------------------------------------------------------------
// LoadUtil::ReadDataFile, per-build call shims
// ---------------------------------------------------------------------------

__declspec(naked) static bool __cdecl call_read_data_file_steam(const char* /*name*/)
{
   __asm {
      push ebp
      mov  ebp, esp
      push 0                          // lvlNames
      push 0                          // lvlCount
      push 0                          // hashes
      push 0                          // (unused by the callee)
      mov  ecx, [ebp + 8]             // name
      call dword ptr [s_read_data_file]
      add  esp, 16                    // caller-cleaned
      mov  esp, ebp
      pop  ebp
      ret
   }
}

static bool read_data_file(const char* name)
{
   if (!s_read_data_file) return false;

   if (g_build == GameBuild::Modtools)
      return ((fn_read_data_file_mt_t)s_read_data_file)(name, 0, 0, nullptr, 0, nullptr);

   return call_read_data_file_steam(name);
}

// ---------------------------------------------------------------------------
// hooked_lua_read_data_file reads prone.lvl on the heels of every ingame.lvl
// ---------------------------------------------------------------------------

static int __cdecl hooked_lua_read_data_file(lua_State* L)
{
   // Read argument 1 before running the original: the callback leaves the
   // stack intact, but reading it up front keeps us independent of that.
   const char* name = (L && g_lua.tolstring) ? g_lua.tolstring(L, 1, nullptr) : nullptr;
   const bool  isIngame = name && _stricmp(name, kIngameLvl) == 0;

   const int result = original_lua_read_data_file(L);

   if (isIngame && s_proneConfigured) {
      const bool ok = read_data_file(kProneLvl);
      g_proneEnabled = ok;

      if (!ok) {
         if (auto fn_log = get_gamelog())
            fn_log("[Prone] %s not found in data\\_lvl_pc\\, so prone is disabled "
                   "for this mission\n", kProneLvl);
      }
   }

   return result;
}

// =============================================================================
// Low-res prone pose graft.
//
// Distant soldiers are drawn by SoldierAnimatorLowResClass, not the
// AnimationFinder.  It bakes a small fixed pose array in PostLoad from names
// composed as "<bank>_<anim>", which is "humanlz_..." for humans.
// soldier_prone.cpp repoints pose slot 2 at "rifle_prone_idle_emote", but
// stock humanlz has no such animation, so the lookup misses and PostLoad
// substitutes CROUCH ("Can't find lowres animation %s, using CROUCH").
//
// Sub-bank appending can't fix this.  The low-res system only scans
// "<bank>_0".."<bank>_9" when the *bare* name is absent from the global table,
// and vanilla ships humanlz un-suffixed, so the exact-name hit short-circuits
// the scan.  We graft one level down instead.  A bank's animations live in a
// PblHashTable private to that bank (ZephyrAnimBank+0x04, 256 key slots then
// 256 value slots), keyed by the hash of the fully composed name.  Inserting
// there never touches SoldierAnimatorClass::mAnimBankOld[], so it costs no
// animation-bank array slot (the 18 to 128 raise) and no distinct bank name
// (the hard 16-name cap in SetupBodyMasks).
//
// Engine pieces:
//   SoldierAnimatorLowResClass::PostLoad (mt 0x00586E60 / steam 0x00647D40)
//     Called from FrontLine::PostLoad at mission start, after every .lvl is
//     read.  A pre-hook lands before any pose is baked, once per mission, which
//     matches the cadence we need since banks are freed between missions.
//   PblHashTableCode::_Store (mt 0x007E1A90 / steam 0x00726F60)
//     __cdecl(table, size, hash, value).  It is first-wins, so it will not
//     overwrite an existing key, and it has NO full-table guard: probing a
//     completely full table spins forever.  Hence the occupancy check.
//   PblTEMPHash (mt 0x007E1C10 / steam 0x00726D80)
//     CRC32-style hash for animation names *inside* a bank.  This is NOT the
//     same as PblHash (FNV-1a), which keys bank names in the global
//     RedAnimation table.  The two are not interchangeable.
//
// A missing or mismatched donor is non-fatal: it just leaves the vanilla
// CROUCH fallback in place.
// =============================================================================

// RedAnimation (PDB-confirmed; identical on both builds)
static constexpr int kRA_SkeletonShared = 0x0C;  // ZephyrSkeletonShared*
static constexpr int kRA_ZephyrAnimBank = 0x14;  // ZephyrAnimBank*
static constexpr int kRA_bFind          = 0x24;  // bool, "this bank is in use"

static constexpr int kZSS_NumJoints     = 0x04;  // ZephyrSkeletonShared, uint

// ZephyrAnimBank table: _Find/_Store take the doubled slot count (they halve it)
static constexpr int kZAB_Table         = 0x04;
static constexpr int kZAB_TableSize     = 512;
static constexpr int kZAB_Slots         = kZAB_TableSize / 2;

static constexpr int kAnimTableSize     = 0x800;  // global RedAnimation table

// Global RedAnimation lookup, mirroring the engine's own fallback (the low-res
// class tries the bare name, then "<name>_0").
static void* find_bank(const char* name)
{
   uint32_t hash = 0;
   fn_pblHash(&hash, name);

   void* entry = fn_hashFind(g_animHashTable, kAnimTableSize, hash);
   if (entry) return entry;

   char sub[64];
   _snprintf(sub, sizeof(sub), "%s_0", name);
   sub[sizeof(sub) - 1] = '\0';
   fn_pblHash(&hash, sub);
   return fn_hashFind(g_animHashTable, kAnimTableSize, hash);
}

static void* bank_table(void* bank)
{
   void* zab = *(void**)((char*)bank + kRA_ZephyrAnimBank);
   return zab ? (char*)zab + kZAB_Table : nullptr;
}

static uint32_t joint_count(void* bank)
{
   void* skel = *(void**)((char*)bank + kRA_SkeletonShared);
   return skel ? *(uint32_t*)((char*)skel + kZSS_NumJoints) : 0;
}

// _Store probes until it finds an empty key, so any free slot is reachable, but
// a completely full table would loop forever.  Keep a small margin.
static bool table_has_room(void* table)
{
   const uint32_t* keys = (const uint32_t*)table;
   int used = 0;
   for (int i = 0; i < kZAB_Slots; ++i)
      if (keys[i] != 0) ++used;
   return used < (kZAB_Slots - 4);
}

static void graft_prone_pose()
{
   auto fn_log = get_gamelog();

   void* target = find_bank(kTargetBank);
   if (!target) return;                     // no human low-res bank this mission

   void* targetTable = bank_table(target);
   if (!targetTable) return;

   char targetName[128];
   _snprintf(targetName, sizeof(targetName), "%s_%s", kTargetBank, kProneAnim);
   targetName[sizeof(targetName) - 1] = '\0';
   const uint32_t targetKey = fn_tempHash(targetName);

   // Already present.  A mod shipping its own humanlz with the prone animation
   // baked in needs no help, and _Store is first-wins anyway.
   if (fn_hashFind(targetTable, kZAB_TableSize, targetKey))
      return;

   void* donor = find_bank(kDonorBank);
   if (!donor) {
      if (fn_log)
         fn_log("[Prone] low-res bank '%s' not found in %s, so distant prone "
                "soldiers will use the CROUCH pose\n", kDonorBank, kProneLvl);
      return;
   }

   void* donorTable = bank_table(donor);
   if (!donorTable) return;

   char donorName[128];
   _snprintf(donorName, sizeof(donorName), "%s_%s", kDonorBank, kProneAnim);
   donorName[sizeof(donorName) - 1] = '\0';

   void* anim = fn_hashFind(donorTable, kZAB_TableSize, fn_tempHash(donorName));
   if (!anim) {
      if (fn_log)
         fn_log("[Prone] '%s' missing from low-res bank '%s'\n", donorName, kDonorBank);
      return;
   }

   // Animation tracks are bone-indexed against the bank's skeleton, so a donor
   // munged on the wrong skeleton would drive the wrong joints.  Matching joint
   // counts aren't proof of identity, but they catch the realistic mistake of
   // building the donor against the high-res human skeleton.
   const uint32_t donorJoints  = joint_count(donor);
   const uint32_t targetJoints = joint_count(target);
   if (donorJoints == 0 || donorJoints != targetJoints) {
      if (fn_log)
         fn_log("[Prone] low-res skeleton mismatch: '%s' has %u joints and '%s' has %u, "
                "so the graft was refused.  Re-munge %s against the %s skeleton\n",
                kDonorBank, donorJoints, kTargetBank, targetJoints,
                kDonorBank, kTargetBank);
      return;
   }

   if (!table_has_room(targetTable)) {
      if (fn_log)
         fn_log("[Prone] animation table for '%s' is full, so %s could not be grafted\n",
                kTargetBank, targetName);
      return;
   }

   fn_hashStore(targetTable, kZAB_TableSize, targetKey, anim);

   // Mark the donor referenced, the way the engine marks every bank it uses.
   *(uint8_t*)((char*)donor + kRA_bFind) = 1;
}

// Graft first, then let the engine bake its pose array.
static void __cdecl hooked_PostLoad()
{
   if (g_proneEnabled)
      graft_prone_pose();

   original_PostLoad();
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------

void prone_lvl_load_install(uintptr_t exe_base)
{
   if (g_build == GameBuild::Unknown) return;
   if (g_addr->lua_read_data_file == 0 || g_addr->load_util_read_data_file == 0) return;

   // Snapshot the INI setting, then hold prone off until prone.lvl is proven
   // present.  Without the animations the state machine has nothing to play.
   s_proneConfigured = g_proneEnabled;
   if (!s_proneConfigured) return;
   g_proneEnabled = false;

   s_read_data_file = (uintptr_t)resolve(exe_base, g_addr->load_util_read_data_file);
   original_lua_read_data_file =
       (fn_lua_cb_t)resolve(exe_base, g_addr->lua_read_data_file);

   // The low-res graft is skipped where any of its pieces are unmapped (GOG).
   if (g_addr->lowres_postload && g_addr->pbl_hash_table_store &&
       g_addr->pbl_temp_hash && g_addr->pbl_hash_table_find &&
       g_addr->hash_string_thiscall && g_addr->anim_hash_table) {
      original_PostLoad = (fn_PostLoad_t)resolve(exe_base, g_addr->lowres_postload);
      fn_pblHash        = (fn_PblHash_t)  resolve(exe_base, g_addr->hash_string_thiscall);
      fn_tempHash       = (fn_TempHash_t) resolve(exe_base, g_addr->pbl_temp_hash);
      fn_hashFind       = (fn_HashFind_t) resolve(exe_base, g_addr->pbl_hash_table_find);
      fn_hashStore      = (fn_HashStore_t)resolve(exe_base, g_addr->pbl_hash_table_store);
      g_animHashTable   = (void*)         resolve(exe_base, g_addr->anim_hash_table);
   }

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_lua_read_data_file, hooked_lua_read_data_file);
   if (original_PostLoad)
      DetourAttach(&(PVOID&)original_PostLoad, hooked_PostLoad);
   DetourTransactionCommit();
}

void prone_lvl_load_uninstall()
{
   if (!original_lua_read_data_file) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_lua_read_data_file, hooked_lua_read_data_file);
   if (original_PostLoad)
      DetourDetach(&(PVOID&)original_PostLoad, hooked_PostLoad);
   DetourTransactionCommit();
}
