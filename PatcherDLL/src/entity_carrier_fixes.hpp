#pragma once

#include <stdint.h>

// =============================================================================
// EntityCarrier / EntityCarrierClass struct layouts (from RE)
// =============================================================================

struct Vec3 { float x, y, z; };

// EntityCarrier::CargoSlot  (+0x1DD0 in EntityCarrier, stride 0x14 = 20 bytes)
struct CargoSlot {
   Vec3  mOffset;    // +0x00  attach-point offset from carrier origin (PblVector3)
   void* mObjectPtr; // +0x0C  PblHandle::ptr  — raw pointer to cargo entity
   int   mObjectGen; // +0x10  PblHandle::generation — validated against cargo->field_0x204
};
static_assert(sizeof(CargoSlot) == 0x14, "CargoSlot size mismatch");

// EntityCarrierClass::CargoInfo  (+0x1180 in EntityCarrierClass, stride 0x10 = 16 bytes)
struct CargoInfo {
   unsigned int mHash;   // +0x00  node name hash (kCargoNodeName property)
   Vec3         mOffset; // +0x04  attach-point offset vector (kCargoNodeOffset property)
};
static_assert(sizeof(CargoInfo) == 0x10, "CargoInfo size mismatch");

// =============================================================================
// Install / uninstall the EntityCarrier bug fixes.
// Call entity_carrier_fixes_install() from lua_hooks_install()  (modtools only).
// Call entity_carrier_fixes_uninstall() from lua_hooks_uninstall().
// =============================================================================

void entity_carrier_fixes_install(uintptr_t exe_base);
void entity_carrier_fixes_uninstall();
