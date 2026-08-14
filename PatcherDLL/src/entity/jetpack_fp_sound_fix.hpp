#pragma once

#include <stdint.h>

// =============================================================================
// Jetpack sound lost on switching to first person.
//
// Engine bug, present on all three builds: entering first person tears down the
// jetpack's engine sound as well as its visual effect, and nothing starts the
// sound again while the jet is still running.  The player hears the jetpack's
// turn-off sound the moment the view changes and then flies on in silence.
//
// EntitySoldier::SetFirstPersonView calls EntitySoldier::TurnOffJetEffect, which
// does two unrelated things: it shuts down the jet VehicleEngine (the sound) and
// it destroys the jet FLEffect (the exhaust, which hangs off the third person
// model and so genuinely does have to go).  Only the second one is wanted here.
//
// The fix redirects that one call to a shim that performs the effect teardown
// alone, leaving the engine running.  The soldier's normal per-frame path still
// turns it off when the jet stops.  See the .cpp for the per-build details.
// =============================================================================

void jetpack_fp_sound_fix_install(uintptr_t exe_base);
void jetpack_fp_sound_fix_uninstall();
