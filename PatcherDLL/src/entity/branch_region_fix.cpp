#include "pch.h"
#include "branch_region_fix.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <detours.h>
#include <string.h>

// =============================================================================
// EntityPath branch regions, part 2: the id alias.
//
// Part 1 is the vtable-slot repair in patch_table.cpp ("EntityPath Branch
// Region Fix"), which is what makes BranchRegionFactory::CreateRegion run at
// all. With that in place regions are created -- but under an id nobody would
// ever write by hand.
//
// CreateRegion derives the id with
//
//     p = strchr(name, ' ');      // p points AT the space
//     hash = PblHash(p);          // hashed FROM the space, inclusive
//
// so a region named "entitypathbranch dropzone1" registers " dropzone1", while
// a path node saying BranchRegion("dropzone1") looks up "dropzone1". Verified
// live: registration produced 0x5c8e3e3b and the lookup asked for 0x13a09f91.
// Had the original used p + 1 the two would agree.
//
// This replaces the id derivation with the one the original clearly intended:
// the text AFTER the separator. A region named "entitypathbranch dropzone1"
// then answers to BranchRegion("dropzone1"), which is what a mapper would
// write. Nobody has been relying on the leading-space spelling -- the feature
// has never worked for anyone -- so nothing is kept compatible with it.
//
// The engine's own CreateRegion is deliberately not called: doing both would
// register every region twice, the second time under an id nobody uses. The
// object is built with the engine's allocator and constructor and links itself
// into the same global list, so PostStateCleanup frees it as usual.
//
// Derived for modtools, Steam and GOG; the installer no-ops on any build whose
// address is 0.
// =============================================================================

bool g_branchRegionFixEnabled = true;

namespace {

// __stdcall(desc, name) -- takes both args on the stack and RETs 8; `this` is
// unused, which is why it can be hooked without a thiscall shim.
typedef void*(__stdcall* fn_CreateRegion_t)(void* desc, const char* name);


fn_CreateRegion_t g_origCreateRegion = nullptr;

// PblHash: FNV-1a with case folded by OR 0x20. Matches the mod tools' Hash.exe.
uint32_t pbl_hash(const char* s)
{
   uint32_t h = 2166136261u;
   for (; s && *s; ++s) {
      h ^= (uint8_t)(*s | 0x20);
      h *= 16777619u;
   }
   return h;
}

// BranchRegion::mHashID -- verified at +0x20 on modtools (FindByID compares
// [region+0x20]) and on the retail builds (the constructor writes param_2 to
// this+0x20). The release builds pass the radius in XMM2, so their constructor
// signature differs from modtools' -- which is exactly why the id is corrected
// on the finished object rather than by constructing one ourselves.
constexpr uint32_t kHashIdOffset = 0x20;

// Let the engine build and link the region exactly as it always has, then
// re-stamp the id with the text AFTER the separator. This is portable across
// all three builds: it needs only the function to hook and one field offset,
// no allocator, no constructor signature, no calling convention to match.
void* __stdcall hooked_create_region(void* desc, const char* name)
{
   void* region = g_origCreateRegion(desc, name);
   if (!region || !name) return region;

   const char* space = strchr(name, ' ');
   if (!space || !space[1]) return region; // "entitypathbranch" with no id

   // The engine hashed from the space inclusive, so a region named
   // "entitypathbranch dropzone1" registered " dropzone1". Stamp "dropzone1".
   *(uint32_t*)((char*)region + kHashIdOffset) = pbl_hash(space + 1);
   return region;
}

} // namespace

void branch_region_fix_install(uintptr_t exe_base)
{
   if (!g_branchRegionFixEnabled) return;
   if (g_addr->branch_region_create == 0) return; // not derived for this build

   // Only meaningful once the vtable slot actually reaches CreateRegion; if that
   // patch set was disabled or failed to verify, the engine never calls this
   // function and the hook simply never fires.
   g_origCreateRegion = (fn_CreateRegion_t)resolve(exe_base, g_addr->branch_region_create);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG rd = DetourAttach(reinterpret_cast<PVOID*>(&g_origCreateRegion), hooked_create_region);
   const LONG rc = DetourTransactionCommit();

   if (rd != NO_ERROR || rc != NO_ERROR) g_origCreateRegion = nullptr;
}

void branch_region_fix_uninstall()
{
   if (!g_origCreateRegion) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(reinterpret_cast<PVOID*>(&g_origCreateRegion), hooked_create_region);
   DetourTransactionCommit();
   g_origCreateRegion = nullptr;
}
