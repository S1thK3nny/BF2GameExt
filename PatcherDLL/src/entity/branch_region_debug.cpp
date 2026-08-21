#include "pch.h"
#include "branch_region_debug.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <detours.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// =============================================================================
// EntityPath branch-region diagnostic.
//
// Purely a diagnostic: it changes no behaviour, it only narrates the three
// steps that have to line up for a path node's BranchRegion("x") to resolve:
//
//   1. LoadUtil::ProcessRegionInfo -> RedRegionFactory::Find(regionName)
//        which factory claims each region (prefix match on mNamePrefix)
//   2. EntityPath::BranchRegionFactory::CreateRegion(desc, name)
//        rejects unless desc->mTypeId == PblHash("sphere"), then registers
//        under PblHash(strchr(name,' ')) -- the pointer stays ON the space
//   3. EntityPath::BranchRegion::FindByID(hash)
//        what the path property asks for, and whether the list has anything
//        in it at that moment
//
// Five separate theories about why this fails have been wrong, each derived
// from static reading. This replaces that with facts.
//
// Output goes to BF2GameExt.log through the CRT rather than the engine logger:
// the first cut of this module used get_gamelog() and produced no output at
// all, and the logger was one of the two unknowns (the other being whether the
// detours attached, which is why the install line reports its return codes).
//
// modtools only -- the addresses are not derived for retail, and the installer
// no-ops elsewhere.
// =============================================================================

bool g_branchRegionDebugEnabled = false;

namespace {

void dbg_log(const char* fmt, ...)
{
   FILE* f = nullptr;
   if (fopen_s(&f, "BF2GameExt.log", "a") != 0 || !f) return;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fclose(f);
}

// FNV-1a with case folded by OR 0x20 -- the engine's PblHash, matching the mod
// tools' own Hash.exe (verified: it reproduces the three water-property hashes
// recorded in render/water_texture_count_fix.cpp).
uint32_t pbl_hash(const char* s)
{
   uint32_t h = 2166136261u;
   for (; s && *s; ++s) {
      h ^= (uint8_t)(*s | 0x20);
      h *= 16777619u;
   }
   return h;
}

typedef void*(__cdecl* fn_Find_t)(const char* name);
typedef void*(__stdcall* fn_CreateRegion_t)(void* desc, const char* name);
typedef void*(__cdecl* fn_FindByID_t)(uint32_t hash);

fn_Find_t         g_origFind         = nullptr;
fn_CreateRegion_t g_origCreateRegion = nullptr;
fn_FindByID_t     g_origFindByID     = nullptr;

// PblList<BranchRegion>::_iCount, so every line reports how many branch regions
// exist at that instant.
// Per-build sites.  Retail is filled in as it is verified; a build whose entry
// is still zero simply does not install, so this file's behaviour is unchanged
// for any build that has not been ported yet.
//
// NOTE the three globals are DIFFERENT THINGS and are easy to conflate:
//   listCount       modtools 0x00AD345C - the live branch-region count
//   factoryListHead modtools 0x00E5F578 - RedRegionFactory::sList, the terminator
//                                          node of the factory list
//   (FindByID itself reads a third global, modtools 0x00AD3454, eight bytes below
//    listCount.)
// The factory walk also assumes  factory = node - 8  and  prefix = node - 4,
// which are layout facts and need their own confirmation per build.
struct DbgSites {
   uintptr_t listCount;
   uintptr_t factoryListHead;
   uintptr_t defaultFactory;
   uintptr_t find;
   uintptr_t createRegion;
   uintptr_t findByID;
};

constexpr DbgSites kDbgModtools = {
   0x00AD345C, 0x00E5F578, 0x00E5F57C, 0x008224C0, 0x005E4C90, 0x005E4C20,
};
// TODO(retail): pending verification, see ROADMAP `## Debugging`.  CreateRegion
// is already known to be 0x004D0F00 on both retail builds.
constexpr DbgSites kDbgSteam = {0, 0, 0, 0, 0x004D0F00, 0};
constexpr DbgSites kDbgGOG   = {0, 0, 0, 0, 0x004D0F00, 0};

const DbgSites* s_dbg = nullptr;

uint32_t* g_listCount = nullptr;

uint32_t live_count()
{
   return g_listCount ? *g_listCount : 0xFFFFFFFFu;
}

// A world has hundreds of regions; only narrate the ones whose name could
// possibly reach BranchRegionFactory.
bool interesting(const char* name)
{
   return name && _strnicmp(name, "entitypathbranch", 16) == 0;
}

// RedRegionFactory layout (confirmed against the Phantom PDB and modtools
// Find): +0 vftable, +4 mNamePrefix, +8 intrusive list node. Find walks the
// list from sList and returns the FIRST factory whose prefix matches, comparing
// only strlen(prefix) bytes -- so a shorter prefix registered earlier wins, and
// an EMPTY (non-null) prefix matches every region name in the world.
uint8_t* g_factoryListHead = nullptr; // &sList (the terminator node)
void*    g_defaultFactory  = nullptr;

void dump_factory_list()
{
   if (!g_factoryListHead) return;

   dbg_log("[BranchDbg] --- registered region factories (in Find order) ---");

   uint8_t* node = *(uint8_t**)g_factoryListHead;
   for (int i = 0; i < 64 && node && node != g_factoryListHead; ++i) {
      uint8_t*    factory = node - 8;
      const char* prefix  = *(const char**)(node - 4);
      dbg_log("[BranchDbg]   [%2d] factory=%p vtbl=%p prefix=%p \"%s\" len=%u",
              i, factory, *(void**)factory, (void*)prefix,
              prefix ? prefix : "(null)", prefix ? (unsigned)strlen(prefix) : 0u);
      node = *(uint8_t**)node;
   }
   dbg_log("[BranchDbg] --- end of factory list (default factory = %p) ---",
           g_defaultFactory);
}

void* __cdecl hooked_find(const char* name)
{
   void* factory = g_origFind(name);

   if (interesting(name)) {
      static bool dumped = false;
      if (!dumped) { dumped = true; dump_factory_list(); }

      const char* prefix = factory ? *(const char**)((uint8_t*)factory + 4) : nullptr;
      dbg_log("[BranchDbg] Find(\"%s\") -> factory %p vtbl=%p prefix=\"%s\"  (live=%u)",
              name, factory, factory ? *(void**)factory : nullptr,
              prefix ? prefix : "(null)", live_count());
   }
   return factory;
}

void* __stdcall hooked_create_region(void* desc, const char* name)
{
   const uint32_t typeId = desc ? *(uint32_t*)((char*)desc + 0x60) : 0;
   const char* space = name ? strchr(name, ' ') : nullptr;

   void* result = g_origCreateRegion(desc, name);

   dbg_log("[BranchDbg] CreateRegion(\"%s\") typeId=%08x (sphere=%08x) idFrom=\"%s\" "
           "hash=%08x -> %p  (live=%u)",
           name ? name : "(null)", typeId, pbl_hash("sphere"),
           space ? space : "(NO SPACE)", space ? pbl_hash(space) : 0, result, live_count());
   return result;
}

void* __cdecl hooked_find_by_id(uint32_t hash)
{
   void* result = g_origFindByID(hash);
   dbg_log("[BranchDbg] FindByID(%08x) -> %p  (live=%u)", hash, result, live_count());
   return result;
}

} // namespace

void branch_region_debug_install(uintptr_t exe_base)
{
   if (!g_branchRegionDebugEnabled) return;

   switch (g_build) {
   case GameBuild::Modtools: s_dbg = &kDbgModtools; break;
   case GameBuild::Steam:    s_dbg = &kDbgSteam;    break;
   case GameBuild::GOG:      s_dbg = &kDbgGOG;      break;
   default:
      dbg_log("[BranchDbg] install skipped: unknown build");
      return;
   }
   const DbgSites& D = *s_dbg;

   // All or nothing.  A partially-mapped build would hook some of the chain and
   // walk a null list for the rest, which reads as "the feature is broken"
   // rather than "the feature is not ported here".
   if (!D.listCount || !D.factoryListHead || !D.defaultFactory ||
       !D.find || !D.createRegion || !D.findByID) {
      dbg_log("[BranchDbg] install skipped: this build is not ported yet");
      return;
   }

   g_listCount       = (uint32_t*)resolve(exe_base, D.listCount);
   g_factoryListHead = (uint8_t*)resolve(exe_base, D.factoryListHead); // RedRegionFactory::sList
   g_defaultFactory  = resolve(exe_base, D.defaultFactory);

   g_origFind         = (fn_Find_t)resolve(exe_base, D.find);
   g_origCreateRegion = (fn_CreateRegion_t)resolve(exe_base, D.createRegion);
   g_origFindByID     = (fn_FindByID_t)resolve(exe_base, D.findByID);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG a1 = DetourAttach(reinterpret_cast<PVOID*>(&g_origFind), hooked_find);
   const LONG a2 = DetourAttach(reinterpret_cast<PVOID*>(&g_origCreateRegion), hooked_create_region);
   const LONG a3 = DetourAttach(reinterpret_cast<PVOID*>(&g_origFindByID), hooked_find_by_id);
   const LONG rc = DetourTransactionCommit();

   dbg_log("[BranchDbg] install: Find=%p CreateRegion=%p FindByID=%p count=%p"
           " attach=(%ld,%ld,%ld) commit=%ld  liveAtInstall=%u",
           (void*)g_origFind, (void*)g_origCreateRegion, (void*)g_origFindByID,
           (void*)g_listCount, a1, a2, a3, rc, live_count());
}

void branch_region_debug_uninstall()
{
   if (!g_origFind) return;

   dbg_log("[BranchDbg] final live count = %u", live_count());

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(reinterpret_cast<PVOID*>(&g_origFind), hooked_find);
   DetourDetach(reinterpret_cast<PVOID*>(&g_origCreateRegion), hooked_create_region);
   DetourDetach(reinterpret_cast<PVOID*>(&g_origFindByID), hooked_find_by_id);
   DetourTransactionCommit();
   g_origFind = nullptr;
}
