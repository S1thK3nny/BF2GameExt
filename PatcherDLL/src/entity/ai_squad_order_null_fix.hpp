#pragma once

#include <stdint.h>

// =============================================================================
// AILowLevel::UpdateIndirect null-target crash fix.
//
// Engine bug: the exit-vehicle branch of AILowLevel::UpdateIndirect computes the
// object it is about to give the order to, deliberately sets that pointer to
// NULL when the vehicle pilots itself (PilotType self / vehicleself-with-self-
// pilot), and then virtual-calls it anyway with no null check.  Reaching that
// branch in the affected state is an unconditional access violation reading
// address 0.
//
// Same family as hover_pilot_null_fix / the self-piloted hover crash: another
// UpdateIndirect dereferencing a null it produced itself.
//
// Fixed on modtools, Steam and GOG.  The two codegens differ (register and
// displaced-instruction length) - see the notes in the .cpp.
// =============================================================================

void ai_squad_order_null_fix_install(uintptr_t exe_base);
void ai_squad_order_null_fix_uninstall();
