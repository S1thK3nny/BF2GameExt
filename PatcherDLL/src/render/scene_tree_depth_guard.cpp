#include "pch.h"
#include "scene_tree_depth_guard.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"
#include "core/x86_emit.hpp"
#include "util/install_log.hpp"

#include <detours.h>
#include <stdio.h>
#include <string.h>

// See scene_tree_depth_guard.hpp for the defect this guards.

namespace {

// ---------------------------------------------------------------------------
// Sites
// ---------------------------------------------------------------------------
// subdivide: the self-recursive function, entered once per level load from
//   RedScene::SetupStaticWorld and otherwise only by itself.
// splitTest: the `count > 10` compare that decides whether to allocate the four
//   children. The conditional jump immediately after it is the engine's own
//   "leave this node alone" path, and forcing that jump is how the cap is
//   applied: we take a branch the engine already takes for small nodes rather
//   than inventing a new code path.
//
//   modtools 0x007F00E0, split test 0x007F05A8  `CMP dword [ESP+0x50],0x0A`
//   Steam    0x006E36E0, split test 0x006E3B6A  `CMP ECX,0x0A`
//   GOG      0x006E4780, split test 0x006E4C0A  `CMP ECX,0x0A`
//
// The modtools pair is the one derived by hand rather than with the port tool,
// which is why every byte of it is checked below before anything is armed.
struct GuardSites {
   uintptr_t      subdivide;
   const uint8_t* prologue;
   uint32_t       prologueLen;
   uintptr_t      splitTest;
   const uint8_t* splitTestBytes; // the JLE follows immediately after these
   uint32_t       splitTestLen;
};

const uint8_t kPrologueRetail[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4,
                                   0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04};
const uint8_t kPrologueModtools[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC,
                                     0x04, 0x01, 0x00, 0x00, 0x53, 0x8B, 0x59, 0x18};

const uint8_t kCmpRetail[]   = {0x83, 0xF9, 0x0A};                   // CMP ECX,0x0A
const uint8_t kCmpModtools[] = {0x83, 0x7C, 0x24, 0x50, 0x0A};       // CMP [ESP+0x50],0x0A

constexpr GuardSites kSitesModtools = {0x007F00E0, kPrologueModtools, sizeof(kPrologueModtools),
                                       0x007F05A8, kCmpModtools,      sizeof(kCmpModtools)};
constexpr GuardSites kSitesSteam    = {0x006E36E0, kPrologueRetail,   sizeof(kPrologueRetail),
                                       0x006E3B6A, kCmpRetail,        sizeof(kCmpRetail)};
constexpr GuardSites kSitesGOG      = {0x006E4780, kPrologueRetail,   sizeof(kPrologueRetail),
                                       0x006E4C0A, kCmpRetail,        sizeof(kCmpRetail)};

// A real map's tree is five deep. A terminating split cannot usefully exceed
// the object count, and every level that makes progress removes at least one
// object from the node, so a node still splitting at 32 is going nowhere.
constexpr int kMaxDepth = 32;

// ---------------------------------------------------------------------------
// The branch flip
// ---------------------------------------------------------------------------
// The split test is followed by `JLE rel32` (0F 8E, six bytes) to the node's
// epilogue. Past the cap we overwrite it with `JMP rel32` plus a NOP, which is
// five plus one, so the displacement is the original plus one. Building it from
// the bytes we read means the jump target is never hardcoded and a build whose
// offset differs still gets a correct patch or no patch at all.
uint8_t s_originalJcc[6] = {};
uint8_t s_forcedJmp[6]   = {};
uint8_t* s_jccAddress    = nullptr;

bool build_forced_jump(const uint8_t* jcc)
{
   if (jcc[0] != 0x0F || jcc[1] != 0x8E) return false; // JLE rel32

   x86::jcc32_as_jmp(jcc, s_forcedJmp);

   memcpy(s_originalJcc, jcc, 6);
   return true;
}

bool s_forced   = false;
int  s_depth    = 0;
bool s_reported = false;

void force_leaf(bool on)
{
   if (on == s_forced) return;
   protected_write(s_jccAddress, on ? s_forcedJmp : s_originalJcc, 6);
   s_forced = on;
}

void report_once()
{
   if (s_reported) return;
   s_reported = true;

   install_log("[SceneTreeGuard] depth cap %d reached while building the static world tree. "
               "This map has objects the engine's splitter cannot separate (coincident, stacked "
               "on one x/z, or carrying a broken position), which in stock BF2 is a load-time "
               "STACK_OVERFLOW. One node was left unsplit instead.",
               kMaxDepth);
}

// ---------------------------------------------------------------------------
// Hook
// ---------------------------------------------------------------------------
typedef char(__fastcall* fn_subdivide_t)(void* box);

fn_subdivide_t g_origSubdivide = nullptr;

char __fastcall hooked_subdivide(void* box)
{
   // The outermost call re-establishes the baseline. If a previous build ever
   // unwound out of the recursion the counter and the patch would be left
   // inconsistent, and this makes that self-healing rather than permanent.
   if (s_depth == 0) force_leaf(false);

   const int depth = s_depth++;

   if (depth >= kMaxDepth) {
      force_leaf(true);
      report_once();
   }

   const char result = g_origSubdivide(box);

   --s_depth;
   if (s_depth < kMaxDepth) force_leaf(false);

   return result;
}

} // namespace

void scene_tree_depth_guard_install(uintptr_t exe_base)
{
   const GuardSites* sites = nullptr;
   switch (g_build) {
   case GameBuild::Modtools: sites = &kSitesModtools; break;
   case GameBuild::Steam:    sites = &kSitesSteam;    break;
   case GameBuild::GOG:      sites = &kSitesGOG;      break;
   default:
      install_log("[SceneTreeGuard] not installed: unknown build");
      return;
   }

   uint8_t* const subdivide = (uint8_t*)resolve(exe_base, sites->subdivide);
   uint8_t* const cmp       = (uint8_t*)resolve(exe_base, sites->splitTest);

   // Every byte checked before anything is written. A build that does not match
   // declines rather than patching something it has not recognised.
   if (memcmp(subdivide, sites->prologue, sites->prologueLen) != 0) {
      install_log("[SceneTreeGuard] not installed: subdivide at %08X does not match", (unsigned)sites->subdivide);
      return;
   }
   if (memcmp(cmp, sites->splitTestBytes, sites->splitTestLen) != 0) {
      install_log("[SceneTreeGuard] not installed: split test at %08X does not match", (unsigned)sites->splitTest);
      return;
   }

   s_jccAddress = cmp + sites->splitTestLen;
   if (!build_forced_jump(s_jccAddress)) {
      install_log("[SceneTreeGuard] not installed: expected a JLE rel32 after the split test at %08X",
                  (unsigned)sites->splitTest);
      s_jccAddress = nullptr;
      return;
   }

   g_origSubdivide = (fn_subdivide_t)subdivide;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG attach = DetourAttach(reinterpret_cast<PVOID*>(&g_origSubdivide), hooked_subdivide);
   const LONG commit = DetourTransactionCommit();

   if (attach != NO_ERROR || commit != NO_ERROR) {
      install_log("[SceneTreeGuard] not installed: DetourAttach=%ld commit=%ld", attach, commit);
      g_origSubdivide = nullptr;
      s_jccAddress    = nullptr;
      return;
   }

   install_log("[SceneTreeGuard] installed: static world tree depth capped at %d (subdivide %08X, split test %08X)",
               kMaxDepth, (unsigned)sites->subdivide, (unsigned)sites->splitTest);
}
