#pragma once

#include <stdint.h>

// =============================================================================
// Grappling Hook Landing Fix
//
// The engine's OrdnanceGrapplingHook::Update removes the soldier's collision
// body on arrival and replaces it with a soft body, leaving the soldier stuck.
// This hook wraps Update and restores normal collision after a successful
// grapple arrival.
//
// Call grapple_fix_install()   from lua_hooks_install().
// Call grapple_fix_uninstall() from lua_hooks_uninstall().
// =============================================================================

void grapple_fix_install(uintptr_t exe_base);
void grapple_fix_uninstall();
