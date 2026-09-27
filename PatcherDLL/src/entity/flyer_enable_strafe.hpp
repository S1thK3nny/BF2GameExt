#pragma once

#include <stdint.h>

// =============================================================================
// EnableStrafe - per-class ODF property for EntityFlyer.
//
//   EnableStrafe    = 1      // strafe axis moves the flyer sideways
//   StrafeSpeed     = 15.0   // stock property: sideways speed at full stick
//   StrafeRollAngle = 0.2    // stock property: lean while strafing (0 = none)
//
// Off by default; omitting the property (or setting 0) keeps stock behaviour.
// With it on, the strafe axis (left stick X, A/D) no longer rolls the flyer;
// roll stays available through the alternate control mode, which rolls on the
// turn axis. See the .cpp header for how the engine's cut strafe path is fed.
//
// Build-aware (modtools + Steam + GOG): install from dllmain's build-aware
// section, NOT from lua_hooks_install (that one is modtools-only).
// =============================================================================

void flyer_enable_strafe_install(uintptr_t exe_base);
void flyer_enable_strafe_uninstall();

// Drops the class->flag table. Class objects are rebuilt per level and their
// addresses get reused, so stale entries would leak the flag onto an unrelated
// flyer class. On modtools this is called from lua_hooks' hooked_init_state();
// on other builds this module detours init_state itself.
void flyer_enable_strafe_reset();
