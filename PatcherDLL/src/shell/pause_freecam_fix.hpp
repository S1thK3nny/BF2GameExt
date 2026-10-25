#pragma once

#include <stdint.h>

// Pause-menu Free Camera first-press fix (all builds).
//
// After a match left in free camera, the next match's first Free Camera press
// only flipped the button label.  Clears the stale engine state per mission.
// Always on: this is a missing reset, not a behaviour choice.

void pause_freecam_fix_install(uintptr_t exe_base);

// Per-mission reset. Called from lua_hooks' init_state hook.
void pause_freecam_fix_reset();
