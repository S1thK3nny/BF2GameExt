#pragma once

#include <stdint.h>

// =============================================================================
// Cloth simulation fixes
//
// Three hooks on EntityCloth:
//
//   InternalUpdate           - fixed 1/30 s simulation timestep with the
//                              accumulator clamped to 0.13333 s (ported from
//                              the Phantom dev build; retail integrates at the
//                              raw, unclamped frame delta, which is what lets
//                              particles tunnel straight through the collision
//                              volumes), plus a raised solver iteration count
//                              (every shipped call site passes 1).
//
//   EnforceCollisions        - corrects the implicit Verlet velocity for any
//                              particle that collision displaced, so the
//                              correction is not undone on the next step, and
//                              resets non-finite particles to their rest
//                              position so a single NaN cannot permanently
//                              kill the cloth.
//
//   EnforceCylinderCollision - fixes the doubled half-height (the authored
//                              `height` is the FULL height, so vanilla's
//                              volume was 4x too tall) and the axis push that
//                              always went along +axis regardless of which
//                              cap the particle entered through.
//
// Installed from dllmain's patch block while the sections are still RW.
// =============================================================================

void cloth_collision_fix_install(uintptr_t exe_base);
void cloth_collision_fix_uninstall();
