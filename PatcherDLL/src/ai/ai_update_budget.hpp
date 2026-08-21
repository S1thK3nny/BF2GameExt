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
// THE DIAL RUNS ON ALL THREE BUILDS; the diagnostic is modtools-only, because it
// hooks ControllerManager::Update and UpdateHighLevel and those addresses are not
// mapped for retail.  Retail compiles the same quota with CMOVcc instead of the
// NEG/SBB idiom above, and gives the non-uber value its own imm32:
//
//   00486403  B9 64000000     MOV    ECX,0x64   ; uber = 100 -- NOT the dial
//   00486408  BE 0A000000     MOV    ESI,0x0A   ; the dial
//   0048640D  0F 45 F1        CMOVNZ ESI,ECX
//
// Same VA and same bytes on Steam and GOG.  Beware the ray-test budget 356 bytes
// later at 0x0048656D, which differs only in its opcode byte -- see game_addrs.
// The installer no-ops where the address is 0.
// =============================================================================

// 0 = stock (10 per turn).  Otherwise the desired budget, clamped to [10, 127].
// modtools needs that ceiling -- its site is a sign-extended imm8, and 0x80 would
// read negative and stop every high-level update.  Retail's imm32 has no such
// limit, but it is held to the same range deliberately: measured demand sits
// under 8, so a wider range on one build only would be a difference in the INI
// with no difference in the game.
extern int g_aiUpdateBudget;

// Hand-added key, deliberately absent from ini_registry.hpp:
// [Diagnostic] AIUpdateDiag.
extern bool g_aiUpdateDiag;

void ai_update_budget_install(uintptr_t exe_base);
void ai_update_budget_uninstall();
