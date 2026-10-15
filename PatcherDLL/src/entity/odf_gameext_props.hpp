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

// Listeners for GameExt's own ODF properties, fed from the same reader hooks.
//
// A property handler sees every property of every entity, explosion, ordnance
// and weapon class as the class reads it, with an @GameExt override already
// applied as the property it names. It returns true to consume the property,
// which then never reaches the class's SetProperty.
//
// A derive handler sees each entity and weapon class as it is created from its
// base: the ClassParent class, or the class type's own root class. A child
// starts as a copy of its parent, before its own properties are read, so this
// is where a property kept outside the class object has to be inherited.
//
// A property handler also sees what a mission script sets with the stock
// SetClassProperty(class, property, value) on an entity class, as it would the
// same line in the class's ODF: the class it names, already loaded, and the
// value as text (a boolean as "1" or "0"). Unless registered with `scripts`
// false, for a property whose meaning hangs on the lines read before it.
//
// Register from an installer, before any level loads; up to eight of each.
using OdfPropertyHandler = bool (*)(void* cls, uint32_t hash, const char* value);
using OdfDeriveHandler   = void (*)(const void* parent, void* child);
bool odf_add_property_handler(OdfPropertyHandler handler, bool scripts = true);
bool odf_add_derive_handler(OdfDeriveHandler handler);
