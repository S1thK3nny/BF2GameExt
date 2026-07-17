#pragma once

#include <stdint.h>

// =============================================================================
// Droideka death animation fix.
//
// Walkerdroids never play their death animation, even though every stock
// droideka bank defines one. Regular walkers (ATST/ATTE/ATAT) do.
//
// Build-aware (modtools + Steam): install from dllmain's build-aware section,
// while .text is still writable.  INI: [Fixes] DroidekaDeathAnimation=1
// =============================================================================

extern bool g_droidekaDeathAnimEnabled;

void droideka_death_anim_install(uintptr_t exe_base);
void droideka_death_anim_uninstall();
