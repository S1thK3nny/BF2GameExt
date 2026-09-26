#pragma once

#include <stdint.h>

// =============================================================================
// FoleyFX regions: the stale-list crash fix, and the region lookup the engine
// never wired up.
//
// THE REGIONS
//   A "foleyfx <group>" world region is a FoleyFXRegion: RedRegion base, the
//   FoleyFXGroup* it names at +0x60, and a node at +0x64 in the static intrusive
//   list FoleyFXRegion::smList (head terminated by its own address).
//
// THE CRASH
//   GameState::PostStateCleanup drops the mission heap and resets
//   PblRegion::sList and RedRegionFactory::sList, but never smList.  The engine
//   even has the reset, FoleyFXRegion::RemoveAll (`smList.next = &smList`,
//   modtools 0x00761590), with no callers; retail's linker stripped it.
//
//   So in a playlist, the first FoleyFXRegion of the next mission runs its ctor,
//   which walks smList to the tail to append itself, and follows the previous
//   mission's nodes into freed memory.  Only a map with foleyfx regions played
//   after another map with foleyfx regions trips it.
//
//   Fix: do what RemoveAll does on every mission start, from the same init_state
//   hook the other per-level resets use.  LuaHelper::InitState runs at the top of
//   GameState::PreStateInit: after the old heap is gone and before the world (and
//   its regions) loads, so nothing live is ever dropped.  A plain data write;
//   smList is derived for modtools, Steam and GOG.
//
// THE LOOKUP
//   The regions were built and linked but never read: FoleyFXRegion::FindRegion
//   has no callers, and FoleyFXCollider::CollisionCallback picks the foley group
//   from the collision type alone - water, terrain, or the object's ODF
//   FoleyFXGroup.  So a region never changed a single sound.
//
//   We retarget the CALL that fetches the terrain group.  If the collision point
//   is inside a foleyfx region whose group loaded, that region's group is used
//   instead; otherwise the stock terrain group.  Terrain only, by design: water
//   and objects keep their own sounds, so a region acts as a surface patch.
//   SetupFoleyFX already ignores an unchanged group, so walking in and out of a
//   region switches cleanly both ways.
//
//   The region walk is our own, not FindRegion, because retail stripped
//   FindRegion.  All three builds; the position sits in ESI on modtools and in
//   EDI on Steam/GOG, so each gets its own register-transparent stub.
// =============================================================================

void foleyfx_region_install(uintptr_t exe_base);
void foleyfx_region_uninstall();

// Per-mission reset. Called from lua_hooks' init_state hook.
void foleyfx_region_reset();
