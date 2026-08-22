#pragma once

#include <stdint.h>

// =============================================================================
// Grappling Hook
//
// Re-enables the cut grappling hook by repairing the engine's own
// implementation rather than replacing it.  Three defects are fixed:
//   - the cable renders untextured,
//   - the pull animation is driven with an invalid animation id,
//   - the soldier can be left with no collision body.
//
// Modtools only.  Call grapple_install() from lua_hooks_install() and
// grapple_uninstall() from lua_hooks_uninstall().
// =============================================================================

void grapple_install(uintptr_t exe_base);
void grapple_uninstall();
