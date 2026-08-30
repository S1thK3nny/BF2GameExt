#pragma once

#include <stdint.h>

// =============================================================================
// "First shot in first person plays no animation" fix.
//
// Engine bug, present on all three builds and soldiers only (vehicles use
// FirstPersonRenderable::UpdateCockpit, which has none of this state machine).
//
// The FP shoot animation is edge-triggered by Weapon::mFiredFlag, a latch
// Weapon::SignalFire sets on every shot.  UpdateSoldier consumes the latch and
// picks the shoot state - and then, further down the same call, a transition
// block overwrites whatever state was just computed with mTransitionAnim for as
// long as fTransitionTimer < fTransitionTimerMax.  The latch is already spent by
// then, so the shot's animation is dropped rather than deferred.
//
// Entering first person arms exactly that: FirstPersonRenderable::Activate sets
// mBlendHandsDown, which UpdateSoldier turns into a 1.0 second blend back to
// idle.  Every shot fired inside that second is swallowed, which is why it reads
// as "the first shot does nothing" and why waiting a moment makes it work.
//
// The fix exempts the two shoot states (2 and 3, the only states the fire branch
// produces) from the transition override.  The timer still advances, so the
// hands-down blend plays out exactly as before around the shot.
//
// See docs/RE/FirstPersonAnimationSystem.md for the full trace.
// =============================================================================

void fp_fire_animation_fix_install(uintptr_t exe_base);
void fp_fire_animation_fix_uninstall();
