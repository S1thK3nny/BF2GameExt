#pragma once

#include <stdint.h>

// =============================================================================
// Droideka death animation fix.
//
// Walkerdroids never play their death animation, even though every stock
// droideka bank defines one. Regular walkers (ATST/ATTE/ATAT) do.
//
// Also drops the personal shield for the duration of the death animation, by
// running the engine's own shield teardown once on the frame the droideka
// starts dying, and locks out steering while it is dying so the corpse cannot
// turn to face whatever killed it.
//
// Build-aware (modtools + Steam + GOG): install from dllmain's build-aware section,
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
