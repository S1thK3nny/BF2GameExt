#pragma once

#include <stdint.h>

// GameExt-only ODF property overrides.
//
// A property whose name ends in "@GameExt" is applied as the property it names,
// but only when this DLL is loaded:
//
//     WeaponName         = "vanilla_weapon"
//     WeaponName@GameExt = "gameext_weapon"
//
// A vanilla exe hashes the second name to something no SetProperty recognises
// and silently ignores it, so the same ODF works in both. User-facing docs are
// in docs/user/ODF_PROPERTIES.md; the measurements this rests on are in the
// header comment of the .cpp.
//
// Always on: an ODF that does not use the suffix is untouched, so the suffix
// itself is the toggle.

void odf_gameext_props_install(uintptr_t exe_base);
void odf_gameext_props_uninstall();

// Count of overrides applied since load, for diagnostics.
uint32_t odf_gameext_props_applied();
