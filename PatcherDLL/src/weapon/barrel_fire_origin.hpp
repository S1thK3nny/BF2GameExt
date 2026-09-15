#pragma once

#include <stdint.h>

// =============================================================================
// Barrel fire origin
//
// Relocates the projectile fire origin to the weapon's barrel hardpoint
// (Weapon::mFirePointMatrix) instead of the hardcoded chest-level aimer point.
// Pure vtable detours + struct-memory reads: OverrideAimer (slot 0x70) moves the
// origin, Render (slot 0x8C) keeps a reflection region's mirrored duplicate draw
// from leaving that matrix reflected.
//
// modtools, Steam and GOG.
// Install from dllmain's build-aware section, after game_build_select().
// =============================================================================

extern bool g_useBarrelFireOrigin;   // INI [Fixes] BarrelFireOriginFix

void barrel_fire_origin_install(uintptr_t exe_base);
void barrel_fire_origin_uninstall();

// For weapons with a second muzzle (dual_cannon). True only when this turn's
// OverrideAimer moved `weapon`'s origin to its barrel; `outDir` is then the direction
// from `muzzle` to the same impact point, under the same AI correction budget.
// False means the vanilla aimer is in charge this turn, so leave the shot alone.
bool barrel_fire_origin_aim_from(void* weapon, const float muzzle[3], float outDir[3]);

// Forget every cached impact point. Call on level load, when aimers are freed.
void barrel_fire_origin_reset_targets();
