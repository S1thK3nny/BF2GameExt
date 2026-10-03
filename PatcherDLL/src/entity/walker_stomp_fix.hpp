#pragma once

#include <cmath>
#include <stdint.h>

// =============================================================================
// Walkers with StompDetectionType 1 land their steps at any frame rate.
//
// EntityWalker::DoFootImpactEffects counts a type 1 walker's step (its stomp
// effect, footstep sound and controller rumble, and so GameExt's StepShake)
// the first update an armed foot drops less than 0.1 in, and a foot is armed
// again only by a drop of more than 0.1 in a single update. A drop in one
// update is the foot's speed times the update's length, so the test is a
// speed that rises with the frame rate: 3 m/s at 30 updates a second, 6 at
// 60, 14.4 at 144. Measured with [Diagnostic] WalkerFootDiag at 60 fps
// (2026-10-03), an AT-TE's feet come down at 4 to 5.5 m/s walking and 7 to 12
// boosting, so most walking steps never landed and every boosting one did.
//
// THE FIX. Type 1's reads of the 0.1 read a value of GameExt's instead, set
// before each walker's update to 3 m/s times that update's length: BF2's own
// test at 30 updates a second, kept at any frame rate. EntityWalker::
// UpdateState is detoured for the length, its first argument; it calls
// DoFootImpactEffects once, for its own walker. The constant itself stays,
// since UpdateState compares another field with it too.
//
//                                 modtools    Steam       GOG
//   EntityWalker::UpdateState     0x0055B3C0  0x00502890  0x00502890   detoured
//     thiscall(float dt, float, float, float*, float), RET 0x14
//   type 1's re-arm test          0x00555D0F  0x00500888  0x00500888   operand moved
//   type 1's landing test         (the same)  0x0050089D  0x0050089D   operand moved
//   the 0.1 they read             0x00A2C074  0x007B1F60  0x007B2ED8
// modtools makes one FCOMP [0.1] serve both tests; retail reads the 0.1 with
// COMISS XMM0,[0.1] to re-arm and MOVSS XMM1,[0.1] to land.
//
// Only how walkers' steps look and sound changes, each machine for its own
// view: the landed bits feed nothing else (docs/RE/CameraShake.md, Walkers).
//
// INI: [Fixes] WalkerStompFix = 1 (default), 0 for stock. Stays set only if
// installed; the walker foot diagnostic reads it.
// =============================================================================

extern bool g_walkerStompFix;

// The speed type 1's test stands for: BF2's 0.1 an update at 30 updates a
// second.
constexpr float kWalkerStompSpeed = 3.0f;

// The drop type 1 tests in an update `dt` seconds long. A paused or broken
// update keeps the last one.
inline float walker_stomp_drop(float dt, float last)
{
   return dt > 0.0f && std::isfinite(dt) ? kWalkerStompSpeed * dt : last;
}

void walker_stomp_fix_install(uintptr_t exe_base);
void walker_stomp_fix_uninstall();
