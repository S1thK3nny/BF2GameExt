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

// CollisionManager::RayHit, shared so other modules do not have to duplicate the
// LTCG register marshalling this file already owns (see the thunk in the .cpp).
//
//   dir        UNIT vector; the engine scales it by maxDist itself.
//   outHit     receives the CollisionObject* that was hit, or null on a clean miss.
//              The engine zeroes it on entry, so it must never be null.
//   exclude    base GameObject* pointers to ignore - the same pointer kind a
//              PblHandle<GameObject> holds.  CollisionManager::RayCallback compares
//              each candidate's GetGameObject() against this list.
//   mask       collision mask; 0x9A is soldiers, vehicles, terrain and statics,
//              and deliberately excludes water (0x100).
//
// Returns hitDistance / maxDist, 1.0 meaning nothing was hit.  Returns 1.0 with
// *outHit null when the ray API is not resolved on this build, so a caller that
// treats "no hit" as "clear" degrades to always-clear rather than crashing.
float engine_ray_hit(const float* start, const float* dir, float maxDist, void** outHit,
                     void** exclude, int excludeCount, int mask);
void barrel_fire_origin_uninstall();

// For weapons with a second muzzle (dual_cannon). True only when this turn's
// OverrideAimer moved `weapon`'s origin to its barrel; `outDir` is then the direction
// from `muzzle` to the same impact point, under the same AI correction budget.
// False means the vanilla aimer is in charge this turn, so leave the shot alone.
bool barrel_fire_origin_aim_from(void* weapon, const float muzzle[3], float outDir[3]);

// Forget every cached impact point. Call on level load, when aimers are freed.
void barrel_fire_origin_reset_targets();
