#pragma once

#include <stdint.h>

// =============================================================================
// CollisionManager::RayHit - shared accessor.
//
//   float RayHit(PblVector3* start, PblVector3* dir, float maxDist,
//                CollisionObject** outHit, PblVector3* outNormal,
//                GameObject** exclude, int excludeCount, int flags, bool)
//
// Returns the hit fraction of maxDist; 1.0 means nothing was hit.  outHit is
// written unconditionally on entry, so it must never be null.  outNormal may be
// null; when supplied it receives the unit surface normal at the hit.
//
// Calling conventions differ by build and getting it wrong is the exact mistake
// that produced the old aim-assist crash (every stack argument shifts one slot):
//
//   Modtools  plain __cdecl, all nine arguments pushed, result in ST(0)
//   Steam/GOG LTCG - ECX = start, EDX = dir, XMM2 = maxDist, the other six
//                    pushed, caller-cleans, result in XMM0
//
// ray_hit_get() hands back a callable that always presents the __cdecl form
// above, marshalling onto the release layout through a naked thunk when needed,
// so callers never have to care which build they are on.
// =============================================================================

typedef float(__cdecl* RayHit_t)(const void* start, const void* dir, float maxDist,
                                 void** outHit, void* outNormal, void** exclude,
                                 int excludeCount, int flags, int lastArg);

// Resolves RayHit for the active build.  Idempotent - several features want the
// raycast and each calls this from its own installer.  Returns false when the
// build is unknown or has no address for it, in which case ray_hit_get() stays
// null and callers are expected to degrade rather than fail.
bool ray_hit_init(uintptr_t exe_base);

// The resolved entry point, or null until ray_hit_init() has succeeded.
RayHit_t ray_hit_get();

// The engine's own aim-ray mask: EntitySoldier::UpdateWeaponAndAimer uses exactly
// these to resolve TargetInfo::mAimPoint, i.e. to answer "what is this player
// aiming at".  0x80 = terrain, 0x100 = water.  Ordnance::Update sweeps the world
// with 0x90, a subset; RayHit's own default when flags == 0 is 0xBE, a superset
// whose two extra categories are unidentified.
static constexpr int kRayFlagsAim = 0x9A;
