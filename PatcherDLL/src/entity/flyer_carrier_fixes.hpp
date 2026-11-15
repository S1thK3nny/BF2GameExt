#pragma once

#include <stdint.h>

// =============================================================================
// Install / uninstall the EntityCarrier bug fixes.
// Call entity_carrier_fixes_install() from lua_hooks_install()  (modtools only).
// Call entity_carrier_fixes_uninstall() from lua_hooks_uninstall().
// =============================================================================

// INI [Diagnostic] CarrierDiag: log every carrier's flight to BF2GameExt.log.
extern bool g_carrierDiag;

void entity_carrier_fixes_install(uintptr_t exe_base);
void entity_carrier_fixes_uninstall();
