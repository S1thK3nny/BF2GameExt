#pragma once

#include <stdint.h>

// =============================================================================
// Droideka death animation fix.
//
// Walkerdroids never play their death animation, even though every stock
// droideka bank defines one. Regular walkers (ATST/ATTE/ATAT) do.
//
// Also drops the personal shield for the duration of the death animation, by
// widening WeaponShield::Update's existing droideka ball-state test to cover
// the dying/dead states.
//
// Build-aware (modtools + Steam): install from dllmain's build-aware section,
// while .text is still writable.  INI: [Fixes] DroidekaDeathAnimation=1
// =============================================================================

extern bool g_droidekaDeathAnimEnabled;

void droideka_death_anim_install(uintptr_t exe_base);
void droideka_death_anim_uninstall();

// Hooks WeaponShield::Update to record which shield belongs to which droideka,
// so the death path can tear it down (see the .cpp header comment).  Called from
// lua_hooks_install; derives its own build state, so it does not care that
// droideka_death_anim_install runs later in dllmain.
void droideka_shield_tracker_install(uintptr_t exe_base);
