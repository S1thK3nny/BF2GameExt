#pragma once

#include <stdint.h>

// =============================================================================
// Install / uninstall the EntityCarrier bug fixes.
// Call entity_carrier_fixes_install() from lua_hooks_install()  (modtools only).
// Call entity_carrier_fixes_uninstall() from lua_hooks_uninstall().
// =============================================================================

void entity_carrier_fixes_install(uintptr_t exe_base);
void entity_carrier_fixes_uninstall();
