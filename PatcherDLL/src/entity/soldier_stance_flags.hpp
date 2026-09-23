#pragma once

#include <stdint.h>

// =============================================================================
// DisableProne / DisableCrouch - per-class ODF properties for EntitySoldier.
//
//   DisableProne  = 1     // this unit can never go prone (player or AI)
//   DisableCrouch = 1     // this unit can never crouch   (player or AI)
//
// Both default to 0 and inherit through ClassParent. IsAcklay = 1 implies
// DisableProne. The stance code that consumes these is entity/soldier_prone.cpp.
// =============================================================================

void soldier_stance_flags_install(uintptr_t exe_base);
void soldier_stance_flags_uninstall();

// Queries take the EntitySoldierClass*. A null class answers false.
bool soldier_class_prone_disabled(const void* cls);
bool soldier_class_crouch_disabled(const void* cls);
