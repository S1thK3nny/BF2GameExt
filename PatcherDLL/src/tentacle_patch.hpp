#pragma once

#include <stdint.h>

// Tentacle bone limit increase: 4 → 9 tentacles.
// Hooks Constructor + DoTentacles with Detours, batch-calls original
// sub-functions (UpdatePositions, EnforceCollisions, UpdatePose) to
// avoid reimplementing simulation math.
bool patch_tentacle_limit(uintptr_t exe_base);
void unpatch_tentacle_limit();
