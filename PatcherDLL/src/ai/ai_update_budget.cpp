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
// The budget immediate.  `ADD EAX,0x0A` is 83 /0 with a SIGN-EXTENDED imm8, so
// the in-place ceiling is 0x7F -- 0x80 and above would read as negative and the
// loop's `CMP EBP,EBX / JL` would then never run a single update.
constexpr uintptr_t kBudgetImm8 = 0x005999D0;

// ControllerManager::Update itself, and the queue it drains.  0x00B8EE70 holds a
// POINTER to the list object; the object's +4 is the first node, each node's +4
// is the next, and +0xC is the UnitController payload.  The list doubles as its
// own tail sentinel -- the terminator's payload slot is null, which is the exit
// the engine's own loop tests for (0x005999F6 TEST ESI,ESI).
constexpr uintptr_t kControllerMgrUpdate = 0x005997A0;
constexpr uintptr_t kUnitControllerList  = 0x00B8EE70;
constexpr uint32_t  kNodeNext            = 0x04;
constexpr uint32_t  kNodePayload         = 0x0C;

// UnitController::UpdateHighLevel -- the thing the budget actually rations.
constexpr uintptr_t kUpdateHighLevel = 0x005A0370;

constexpr uint32_t kLodTier = 0x3AC; // UnitController's LOD tier, 0..4

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------
volatile LONG s_turns      = 0; // ControllerManager::Update calls
volatile LONG s_highLevel  = 0; // UpdateHighLevel calls inside them
int  s_peakControllers = 0;
int  s_peakTierCount[5] = {};
int  s_reports = 0;

uint8_t* s_budgetAddr = nullptr;
uint8_t  s_budgetOrig = 0;

// Signatures READ from the images, not inferred:
//   0x005997A0  ControllerManager::Update  __cdecl (float dt)   -- a free
//               function taking one stack argument, NOT a method.
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

void __cdecl hooked_mgr_update(float dt)
{
   const LONG n = InterlockedIncrement(&s_turns);

   g_origMgrUpdate(dt);

   // Sampling rather than every turn: the walk is O(controllers) and this runs
   // inside the simulation tick.
   if ((n % 120) == 0) sample_population();

   // ~30 s at 60 Hz, and capped so a long session cannot fill the log.
   if ((n % 1800) == 0 && s_reports < 40) {
      ++s_reports;
      report();
   }
}

} // namespace

void ai_update_budget_install(uintptr_t exe_base)
{
   if (g_build != GameBuild::Modtools) return;

   // --- the dial -----------------------------------------------------------
   if (g_aiUpdateBudget > 0) {
      int n = g_aiUpdateBudget;
      if (n < 10) n = 10;   // below stock there is nothing to gain
      if (n > 0x7F) n = 0x7F;

      uint8_t* const site = reinterpret_cast<uint8_t*>(resolve(exe_base, kBudgetImm8));
      if (*site == 0x0A) {
         s_budgetAddr = site;
         s_budgetOrig = *site;
         *site = (uint8_t)n;
      } else {
         diag_log("[AIBudget] budget site %08X reads %02X, expected 0A -- left stock",
                  (unsigned)kBudgetImm8, *site);
      }
   }

   // --- the measurement ----------------------------------------------------
   if (!g_aiUpdateDiag) return;

   g_listSlot      = reinterpret_cast<uint8_t**>(resolve(exe_base, kUnitControllerList));
   g_origMgrUpdate = reinterpret_cast<fn_mgr_update_t>(resolve(exe_base, kControllerMgrUpdate));
   g_origHighLevel = reinterpret_cast<fn_high_level_t>(resolve(exe_base, kUpdateHighLevel));

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG a1 = DetourAttach(reinterpret_cast<PVOID*>(&g_origMgrUpdate), hooked_mgr_update);
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
      DetourDetach(reinterpret_cast<PVOID*>(&g_origMgrUpdate), hooked_mgr_update);
      DetourDetach(reinterpret_cast<PVOID*>(&g_origHighLevel), hooked_high_level);
      DetourTransactionCommit();
      g_origMgrUpdate = nullptr;
   }

   // Sections are re-protected by now, so this cannot be a plain store.
   if (s_budgetAddr) protected_write(s_budgetAddr, &s_budgetOrig, 1);
   s_budgetAddr = nullptr;
}
