#pragma once

#include <stdint.h>

// Walker foot diagnostic. For the walker the local player drives, logs each
// step of each foot to BF2GameExt.log: how far the foot came down, the largest
// drop BF2 saw in a single update of it, and whether BF2 counted the step as a
// landing. The stomp effect, the footstep sound and StepShake all come from
// that count, and a StompDetectionType 1 walker counts a step only once the
// foot comes down fast enough (more than 0.1 in one update, or 3 m/s with
// WalkerStompFix), so this shows whether a walker's feet get there
// (docs/RE/CameraShake.md, Walkers).
//
// INI: [Diagnostic] WalkerFootDiag = 0 (off, the default) or 1. Read-only: it
// detours EntityWalker::DoFootImpactEffects to look before and after each
// call, and only while on. All three builds.
extern bool g_walkerFootDiag;

void walker_foot_diag_install(uintptr_t exe_base);
void walker_foot_diag_uninstall();
