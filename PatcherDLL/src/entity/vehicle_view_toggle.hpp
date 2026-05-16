#pragma once

#include <stdint.h>

// =============================================================================
// Vehicle First/Third-Person View Toggle Fix
//
// Hovers and walkers (EntityHover, EntityWalker) cannot switch between first
// and third person at runtime unless ForceMode is set in the ODF.  The change-
// view gate in each vehicle's Update calls vtable+0x3C on the controllable-
// aimer subobject and skips the toggle if the result is non-zero.  For these
// two classes that slot is a const-true stub (0x005124E0 / 0x00551B10), so
// the toggle is never reached.  EntityFlyer has real logic in that slot, which
// is why flyers work.
//
// Fix: rewrite each vtable's +0x3C entry to point at the const-false thunk
// already present at +0x40 of the same vtable.  The gate's first call then
// returns 0, falls through to the second call (also const-false), and the
// toggle proceeds.
// =============================================================================

void vehicle_view_toggle_install(uintptr_t exe_base);
void vehicle_view_toggle_uninstall();
