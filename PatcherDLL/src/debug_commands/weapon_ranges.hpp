#pragma once

#include <stdint.h>

// =============================================================================
// ShowWeaponRanges — console debug command
//
// Draws flat range circles around soldiers showing weapon AI ranges:
//   Red    = MinRange
//   Green  = OptimalRange
//   Blue   = MaxRange
//   Yellow = AI comfort band (inner / outer)
//
// Usage: type "ShowWeaponRanges" in the ~ console to toggle.
// =============================================================================

void debug_weapon_ranges_install(uintptr_t exe_base);
void debug_weapon_ranges_late_init();
void debug_weapon_ranges_uninstall();

// Called from the shared FreeCamera::Update hook (owned by hover_springs)
void debug_weapon_ranges_freecam_tick();
