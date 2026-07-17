#pragma once

#include <stdint.h>

// =============================================================================
// DisableBallMode - per-class ODF property for EntityDroideka.
//
//   DisableBallMode = 1      // droideka can never roll into ball form
//
// Off by default; omitting the property (or setting 0) keeps stock behaviour.
// Lets a mod use the droideka chassis as a plain walking unit - the roll is
// blocked for the AI as well as the player, which choosing not to press the
// button cannot do.
//
// Build-aware (modtools + Steam): install from dllmain's build-aware section,
// NOT from lua_hooks_install (that one is modtools-only).
// =============================================================================

void droideka_ball_mode_install(uintptr_t exe_base);
void droideka_ball_mode_uninstall();

// Drops the class->flag table. Class objects come from a pool and are rebuilt
// per level, so addresses get reused; stale entries would leak the flag onto an
// unrelated droideka class. On modtools this is called from lua_hooks'
// hooked_init_state(); on other builds this module detours init_state itself.
void droideka_ball_mode_reset();
