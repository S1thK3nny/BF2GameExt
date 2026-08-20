#pragma once

#include <stdint.h>

// =============================================================================
// AI high-level update budget.
//
// ControllerManager::Update services at most TEN UnitControllers per simulation
// turn.  The bound is computed branchlessly at modtools 0x005999C2:
//
//   005999C2  CALL 0x0040298C     ; AIUtil::IsUberMode()
//   005999C7  NEG  AL             ; CF = 1 iff uber
//   005999C9  SBB  EAX,EAX        ; 0 or 0xFFFFFFFF, whatever EAX held
//   005999CB  AND  EAX,0x5A       ; 0 or 90
//   005999CE  ADD  EAX,0x0A       ; -> 10, or 100 in uber mode
//   005999D1  MOV  EBX,EAX        ; the bound
//   00599A69  CMP  EBP,EBX / JL   ; the loop's only exit
//
// The units sit in a priority queue ordered by next-update-time, and each one
// serviced is re-keyed to now + GetUpdateRate(), so the pass drains "the ten most
// overdue" and stops.
//
// WHY THAT LOOKS LIKE STANDING AROUND, rather than like slowness: the budget
// gates UpdateHighLevel (0x005A0370) -- LOD, vision, the threat manager and the
// command FSM, i.e. everything that issues an ORDER.  UpdateLowLevel (0x0059E880)
// runs uncapped for every controller every turn, ticking the navigator and the
// weapon trigger.  So a unit that misses its slot keeps walking wherever it was
// already going and keeps shooting at whatever it already had, but never makes a
// NEW decision.  It finishes its order and then waits.
//
// Demand is set by the LOD tier, not by unit count: tier 4 (within 25 units of a
// human player) wants service every 0.25 s, tier 3 every 1.0 s, and the slow
// tiers every 2 to 4 s.  So whether 10/turn actually binds depends on how the
// population is distributed, and two independent analyses disagreed about where
// the crossover sits -- one put it around 150 units, another argued it may not
// bind at typical counts at all.
//
// That disagreement is why this ships with a MEASUREMENT and a stock default.
// The diagnostic counts turns against high-level updates: if the ratio is pinned
// at exactly the budget, the budget is binding and raising it will help.  If it
// sits below, the budget is not the constraint and raising it would only burn
// frame time.  Measure before choosing a number.
//
// modtools only -- the same quota exists in the retail builds but is compiled
// with CMOVcc rather than this NEG/SBB idiom, so its address has to be derived
// separately.  The installer no-ops where the address is 0.
// =============================================================================

// 0 = stock (10 per turn).  Otherwise the desired budget, clamped to [10, 127]:
// the site is an 8-bit immediate added to a sign-extended base.
extern int g_aiUpdateBudget;

// Hand-added key, deliberately absent from ini_registry.hpp: [AI] AIUpdateDiag.
extern bool g_aiUpdateDiag;

void ai_update_budget_install(uintptr_t exe_base);
void ai_update_budget_uninstall();
