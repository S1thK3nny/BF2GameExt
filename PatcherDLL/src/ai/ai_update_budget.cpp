#include "pch.h"
#include "ai_update_budget.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <detours.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

// See ai_update_budget.hpp for the mechanism and why this measures first.

int  g_aiUpdateBudget = 0;
bool g_aiUpdateDiag   = false;

namespace {

// ---------------------------------------------------------------------------
// modtools sites
// ---------------------------------------------------------------------------
// The budget immediate lives in game_addrs: modtools writes the SIGN-EXTENDED
// imm8 of `ADD EAX,0x0A`, retail writes the imm32 of `MOV ESI,0x0A` that feeds
// its CMOVNZ.  Both are clamped to the same 10..127 range -- retail's imm32 has
// no encoding ceiling, but measurement puts real demand under 8, so a wider
// range on one build only would be a difference in the INI with no difference in
// the game.
constexpr int kMinBudget = 10;
constexpr int kMaxBudget = 127;

// ControllerManager::Update itself, and the queue it drains.  The global holds a
// POINTER to the list object; the object's +4 is the first node, each node's +4
// is the next, and +0xC is the UnitController payload.  The list doubles as its
// own tail sentinel -- the terminator's payload slot is null, which is the exit
// the engine's own loop tests for (modtools 0x005999F6 TEST ESI,ESI).
//
// The node offsets, the next-update-time key at +0x1E0 and the LOD tier at
// +0x3AC are IDENTICAL on all three builds -- verified from the live encodings,
// not assumed.
//
// UpdateHighLevel is __fastcall(this) on every build, and on every build it is
// the function that tail-calls GetUpdateRate; that is how the retail ones were
// identified rather than by any byte pattern.  Note the retail loop calls it
// FIRST and modtools calls it second, so call ORDER is not an identifier here.
struct DiagSites {
   uintptr_t mgrUpdate;
   uintptr_t list;
   uintptr_t updateHighLevel;
   bool      mgrUpdateFloatInXmm0;
};

// THE CONVENTION DIFFERS AND IT IS NOT OPTIONAL.  modtools' Update is a plain
// __cdecl taking its float on the stack.  Both retail builds were compiled with
// whole-program optimisation, which gave this internal function a private
// convention: it reads the delta straight out of XMM0 and never touches
// [EBP+8].  Hooking it with the modtools prototype would hand the counter
// garbage and, worse, let the compiler clobber XMM0 before the original ever
// sees it.  Retail therefore goes through a naked shim that preserves XMM0 and
// tail-jumps to the trampoline.  See hooked_mgr_update_xmm0 below.
constexpr DiagSites kDiagModtools = {0x005997A0, 0x00B8EE70, 0x005A0370, false};
constexpr DiagSites kDiagSteam    = {0x00486370, 0x01E30934, 0x00663970, true};
constexpr DiagSites kDiagGOG      = {0x00486370, 0x01E31DCC, 0x00664A10, true};

constexpr uint32_t  kNodeNext            = 0x04;
constexpr uint32_t  kNodePayload         = 0x0C;

constexpr uint32_t kLodTier = 0x3AC; // UnitController's LOD tier, 0..4

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------
volatile LONG s_turns      = 0; // ControllerManager::Update calls
volatile LONG s_highLevel  = 0; // UpdateHighLevel calls inside them
int  s_peakControllers = 0;
int  s_peakTierCount[5] = {};
int  s_reports = 0;

uint8_t* s_budgetAddr  = nullptr;
uint32_t s_budgetOrig  = 0;
uint32_t s_budgetWidth = 0;

// Signatures READ from the images, not inferred:
//   0x005997A0  ControllerManager::Update  __cdecl (float dt)   -- a free
//               function taking one stack argument, NOT a method.  MODTOOLS
//               ONLY: retail passes that float in XMM0 instead (see DiagSites).
//   0x005A0370  UnitController::UpdateHighLevel  __thiscall (this)  -- ECX only,
//               NO stack arguments, so it RETs 0.
// Getting the second wrong is not survivable: declaring a stack argument it does
// not have makes the hook RET 4 and pop four bytes nobody pushed, unwinding the
// stack a little further on every single high-level update.
using fn_mgr_update_t = void(__cdecl*)(float dt);
using fn_high_level_t = void(__fastcall*)(void* self, void* edx);

fn_mgr_update_t g_origMgrUpdate = nullptr;
fn_high_level_t g_origHighLevel = nullptr;

uint8_t** g_listSlot = nullptr;

const DiagSites* s_diag = nullptr;

void diag_log(const char* fmt, ...)
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

// Walk the queue: how many controllers exist, and how they are spread across LOD
// tiers.  Tier decides how often each one WANTS service, so the spread is what
// turns a raw count into a demand figure.
void sample_population()
{
   if (!g_listSlot || !*g_listSlot) return;

   uint8_t* const list = *g_listSlot;
   uint8_t*       node = *reinterpret_cast<uint8_t**>(list + kNodeNext);

   int total = 0;
   int tiers[5] = {};

   for (int i = 0; i < 8192 && node; ++i) {
      uint8_t* const ctrl = *reinterpret_cast<uint8_t**>(node + kNodePayload);
      if (!ctrl) break; // the terminator's payload is null

      ++total;
      const uint32_t tier = *reinterpret_cast<uint32_t*>(ctrl + kLodTier);
      if (tier < 5) ++tiers[tier];

      node = *reinterpret_cast<uint8_t**>(node + kNodeNext);
   }

   if (total > s_peakControllers) {
      s_peakControllers = total;
      memcpy(s_peakTierCount, tiers, sizeof(tiers));
   }
}

void report()
{
   const LONG turns = s_turns;
   const LONG hl    = s_highLevel;
   if (turns <= 0) return;

   // The decisive ratio.  Pinned at the budget means the queue is always deeper
   // than the pass can drain, so every extra unit steals decisions from another.
   // Comfortably below means the budget is not what is holding the AI back.
   const int budget = (g_aiUpdateBudget > 0) ? g_aiUpdateBudget : 10;
   const LONG per100 = (hl * 100) / turns;

   diag_log("[AIBudget] turns=%ld  highLevelUpdates=%ld  per turn=%ld.%02ld  budget=%d%s",
            turns, hl, per100 / 100, per100 % 100, budget,
            (per100 >= (LONG)budget * 100 - 5) ? "   <-- SATURATED" : "");
   diag_log("[AIBudget]   peak controllers=%d  by LOD tier "
            "[0]=%d [1]=%d [2]=%d [3]=%d [4]=%d  (4 = nearest a player, fastest)",
            s_peakControllers, s_peakTierCount[0], s_peakTierCount[1],
            s_peakTierCount[2], s_peakTierCount[3], s_peakTierCount[4]);

   if (s_peakControllers > 0) {
      // Round-robin latency: at N controllers and B serviced per turn, a given
      // unit waits N/B turns between decisions.
      const int turnsPerUnit = (s_peakControllers + budget - 1) / budget;
      diag_log("[AIBudget]   at peak that is one decision per unit every %d turns",
               turnsPerUnit);
   }
}

void __fastcall hooked_high_level(void* self, void* edx)
{
   InterlockedIncrement(&s_highLevel);
   g_origHighLevel(self, edx);
}

// The per-turn bookkeeping, with no argument of its own so it can be shared by
// both the modtools hook and the retail shim.  Split out precisely so the shim
// has something plain to call while XMM0 is parked on the stack.
void count_turn_pre()
{
   InterlockedIncrement(&s_turns);
}

void count_turn_post()
{
   const LONG n = s_turns;

   // Sampling rather than every turn: the walk is O(controllers) and this runs
   // inside the simulation tick.
   if ((n % 120) == 0) sample_population();

   if ((n == 300 || (n % 1800) == 0) && s_reports < 40) {
      ++s_reports;
      report();
   }
}

// Retail only.  ControllerManager::Update takes its delta in XMM0, so the hook
// cannot be an ordinary C function: the compiler is free to use XMM registers
// for anything, and the original would then be handed a corrupted delta.  This
// saves XMM0 across the counting work and TAIL-JUMPS to the trampoline, so the
// original sees the exact stack and register state the caller set up.
__declspec(naked) void hooked_mgr_update_xmm0()
{
   // Only XMM0 is an input: the original writes ECX, EDX and EAX before ever
   // reading them (verified at Steam 0x0048638B), so clobbering the caller-saved
   // integer registers across these calls is safe. The stack is left exactly as
   // the caller built it -- retail's Update takes NO stack argument.
   __asm {
      sub    esp, 16
      movups [esp], xmm0          // park the delta
      call   count_turn_pre
      movups xmm0, [esp]          // restore before the original runs
      add    esp, 16
      mov    eax, g_origMgrUpdate // the trampoline, which Detours rewrites
      call   eax
      call   count_turn_post
      ret
   }
}

void __cdecl hooked_mgr_update(float dt)
{
   const LONG n = InterlockedIncrement(&s_turns);

   g_origMgrUpdate(dt);

   // Sampling rather than every turn: the walk is O(controllers) and this runs
   // inside the simulation tick.
   if ((n % 120) == 0) sample_population();

   // First report early so a run can be confirmed working without waiting, then
   // every ~30 s at 60 Hz, capped so a long session cannot fill the log.
   if ((n == 300 || (n % 1800) == 0) && s_reports < 40) {
      ++s_reports;
      report();
   }
}

} // namespace

void ai_update_budget_install(uintptr_t exe_base)
{
   // --- the dial -----------------------------------------------------------
   // Runs on every build that names the immediate.  The width IS the build
   // difference: naming the two operands separately keeps the caller from having
   // to know which codegen it is looking at.
   if (g_aiUpdateBudget > 0) {
      const uintptr_t va    = g_addr->ai_update_budget_imm8 ? g_addr->ai_update_budget_imm8
                                                            : g_addr->ai_update_budget_imm32;
      const uint32_t  width = g_addr->ai_update_budget_imm8 ? 1u : 4u;

      if (va == 0) {
         diag_log("[AIBudget] no budget immediate for this build -- left stock");
      } else {
         int n = g_aiUpdateBudget;
         if (n < kMinBudget) n = kMinBudget;   // below stock there is nothing to gain
         if (n > kMaxBudget) n = kMaxBudget;

         uint8_t* const site = reinterpret_cast<uint8_t*>(resolve(exe_base, va));
         uint32_t       cur  = 0;
         memcpy(&cur, site, width);

         if (cur == 0x0A) {
            s_budgetAddr  = site;
            s_budgetOrig  = cur;
            s_budgetWidth = width;
            const uint32_t v = (uint32_t)n;
            memcpy(site, &v, width);
         } else {
            diag_log("[AIBudget] budget site %08X reads %u, expected 10 -- left stock",
                     (unsigned)va, cur);
         }
      }
   }

   // --- the measurement ----------------------------------------------------
   if (!g_aiUpdateDiag) return;

   switch (g_build) {
   case GameBuild::Modtools: s_diag = &kDiagModtools; break;
   case GameBuild::Steam:    s_diag = &kDiagSteam;    break;
   case GameBuild::GOG:      s_diag = &kDiagGOG;      break;
   default:
      diag_log("[AIBudget] diagnostic: unknown build -- not installed");
      return;
   }
   const DiagSites& D = *s_diag;

   g_listSlot      = reinterpret_cast<uint8_t**>(resolve(exe_base, D.list));
   g_origMgrUpdate = reinterpret_cast<fn_mgr_update_t>(resolve(exe_base, D.mgrUpdate));
   g_origHighLevel = reinterpret_cast<fn_high_level_t>(resolve(exe_base, D.updateHighLevel));

   // Retail's Update takes its delta in XMM0, so it gets the naked shim; only
   // modtools may use the plain C hook.
   PVOID const mgrDetour = D.mgrUpdateFloatInXmm0
                              ? reinterpret_cast<PVOID>(&hooked_mgr_update_xmm0)
                              : reinterpret_cast<PVOID>(&hooked_mgr_update);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG a1 = DetourAttach(reinterpret_cast<PVOID*>(&g_origMgrUpdate), mgrDetour);
   const LONG a2 = DetourAttach(reinterpret_cast<PVOID*>(&g_origHighLevel), hooked_high_level);
   const LONG rc = DetourTransactionCommit();

   diag_log("[AIBudget] diagnostic install: Update=%p HighLevel=%p attach=(%ld,%ld) commit=%ld",
            (void*)g_origMgrUpdate, (void*)g_origHighLevel, a1, a2, rc);

   if (rc != NO_ERROR) g_origMgrUpdate = nullptr;
}

void ai_update_budget_uninstall()
{
   if (g_origMgrUpdate) {
      diag_log("[AIBudget] --- final ---");
      report();

      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourDetach(reinterpret_cast<PVOID*>(&g_origMgrUpdate),
                   s_diag && s_diag->mgrUpdateFloatInXmm0
                      ? reinterpret_cast<PVOID>(&hooked_mgr_update_xmm0)
                      : reinterpret_cast<PVOID>(&hooked_mgr_update));
      DetourDetach(reinterpret_cast<PVOID*>(&g_origHighLevel), hooked_high_level);
      DetourTransactionCommit();
      g_origMgrUpdate = nullptr;
   }

   // Sections are re-protected by now, so this cannot be a plain store.
   if (s_budgetAddr) protected_write(s_budgetAddr, &s_budgetOrig, s_budgetWidth);
   s_budgetAddr = nullptr;
}
