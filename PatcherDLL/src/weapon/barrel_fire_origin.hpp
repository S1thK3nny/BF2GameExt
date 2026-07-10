#pragma once

#include <stdint.h>

// =============================================================================
// Barrel fire origin
//
// Relocates the projectile fire origin to the weapon's barrel hardpoint
// (Weapon::mFirePointMatrix) instead of the hardcoded chest-level aimer point.
// Pure vtable detour + struct-memory reads.
//
// GOG not derived.
// Install from dllmain's build-aware section, after game_build_select().
// =============================================================================

extern bool g_useBarrelFireOrigin;   // INI [Fixes] BarrelFireOriginFix

void barrel_fire_origin_install(uintptr_t exe_base);
void barrel_fire_origin_uninstall();
