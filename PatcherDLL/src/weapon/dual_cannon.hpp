#pragma once

#include <stdint.h>

// ClassLabel "dualcannon": a WeaponCannon with a second model whose fire origin,
// muzzle flash and shoot animation alternate between the two guns. Modtools only.
// See docs/RE/WeaponClassFactory.md.

void dual_cannon_install(uintptr_t exe_base);
void dual_cannon_uninstall();
