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
//   region switches cleanly both ways.  A region whose group has no sound for
//   the unit's class counts as absent, so the unit gets the ground sound rather
//   than keeping whatever it had last (possibly an object it stepped off).
//
//   The region walk is our own, not FindRegion, because retail stripped
//   FindRegion.  All three builds; the position sits in ESI on modtools and in
//   EDI on Steam/GOG, so each gets its own register-transparent stub.
//
// SAME-NAME GROUPS
//   Every world sound lvl defines its own terrain_foley, metal_foley, ... and the
//   loader never merges them: each FoleyFXGroup() chunk is a new group, so a
//   mission loading five world sound lvls holds five of each.  The engine then
//   picks copies two different ways: FoleyFXGroup::Read overwrites smTerrain/smWater, so
//   the LAST copy wins there, while FoleyFXGroup::FindByID (regions, object
//   ODFs) returns the FIRST.  And a copy only lists that world's soldiers, so any
//   other class finds no entry, and SetupFoleyFX then keeps whatever sound the
//   unit had before - silence, or a stale one from the last object it touched.
//
//   Two changes, both by retargeting a single CALL:
//   - A region uses the latest copy of its group, the same rule as terrain.
//   - SetupFoleyFX's FindFoleyFX fills gaps: whatever the group itself answers
//     (its exact entry or its class-0 fallback) stands; only when it has nothing
//     does the most recently loaded same-name copy with that class supply it.
//     Nothing that already had a sound changes, and a mission with one world
//     sound lvl (every stock one) has no duplicates to fill from.
// =============================================================================

void foleyfx_region_install(uintptr_t exe_base);
void foleyfx_region_uninstall();

// Per-mission reset. Called from lua_hooks' init_state hook.
void foleyfx_region_reset();
