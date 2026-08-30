#pragma once

#include <stdint.h>

// =============================================================================
// AI decision rate -- the LOD interval dial.
//
// UnitController::UpdateHighLevel (modtools 0x005A0370) ends by re-queueing
// itself:
//
//   controller[0x1E0] = GameLoop::GetMissionTime() + GetUpdateRate(this)
//
// and UnitController::GetUpdateRate is nothing but a base rate multiplied by a
// five-entry table indexed by the controller's LOD tier (UnitController+0x3AC):
//
//   modtools 0x0059E7B0   steam 0x006634E0   gog 0x00664580
//   phantom  0x0078B3B0   UnitController::GetUpdateRate
//
//     tier 0  WICKED_LOW  x 4.00        tier 3  NORMAL  x 1.00
//     tier 1  LOWER       x 3.00        tier 4  HIGH    x 0.25
//     tier 2  LOW         x 2.00
//
// The base rate is the agent's own (virtual +0x48 on the agent at
// UnitController+0x2C0) or 1.0 when there is no agent, so with a stock agent the
// table IS the interval in seconds.
//
// GetUpdateRate has EXACTLY ONE caller -- UpdateHighLevel's re-queue, at
// modtools 0x005A084E.  That is what makes scaling the table safe: the numbers
// feed nothing but the scheduler key, so there is no second consumer to surprise.
//
// WHY THIS AND NOT AIUpdateBudget.  The budget rations how many controllers may
// be serviced per turn; this table sets how many WANT servicing.  Measurement on
// a 263-unit match put the scheduler at 1.49 of its 10 -- about 15% of capacity,
// nothing queueing -- because UpdateLodState grades each unit purely on distance
// to the nearest HUMAN player, leaving 256 of 263 units on a two-to-four second
// decision interval.  Supply was never the problem; demand was.  So this is the
// dial that actually moves AI responsiveness at range, and the budget is the one
// you raise afterwards only if this pushes demand into the cap.
//
// A second effect worth knowing: UpdateLodState runs INSIDE UpdateHighLevel, so
// a tier-0 unit only re-checks its own distance every four seconds.  Shortening
// the intervals shortens that re-check too, so units promote to a faster tier
// sooner when a player closes on them.
//
// ENCODING.  The five floats are stored differently per build, so the address
// registry holds the address of each FLOAT rather than of the instruction, which
// makes both encodings identical to the patcher:
//
//   modtools  five `MOV [ESP+n],imm32` in .text, 8 bytes apart
//   retail    tiers 0-3 folded into one 16-byte .rdata constant read by MOVAPS,
//             tier 4 still an imm32 in .text
//
// The retail constant is referenced only by GetUpdateRate -- its +4, +8 and +0xC
// have no xrefs of their own, so it is not a pooled literal shared with other
// code and can be rewritten in place.
// =============================================================================

// Multiple of the stock decision rate.  1.0 is stock; 2.0 makes every tier think
// twice as often; below 1.0 slows them down to buy frames back.  Clamped to
// [0.25, 4.0], and no tier is ever taken below 0.25 s -- the engine's own
// closest-to-player interval -- so this only ever closes the gap between distant
// and nearby AI, it never invents a faster-than-stock rate.
extern float g_aiDecisionRate;

void ai_decision_rate_install(uintptr_t exe_base);
void ai_decision_rate_uninstall();
