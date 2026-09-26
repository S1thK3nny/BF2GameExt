#pragma once

// =============================================================================
// FoleyFXRegion stale-list crash fix.
//
// THE CRASH
//   A "foleyfx <group>" world region is a FoleyFXRegion, which links itself into
//   the static intrusive list FoleyFXRegion::smList (node at region+0x64, head
//   terminated by its own address).  GameState::PostStateCleanup drops the
//   mission heap and resets PblRegion::sList and RedRegionFactory::sList, but
//   never smList.  The engine even has the reset, FoleyFXRegion::RemoveAll
//   (`smList.next = &smList`, modtools 0x00761590), with no callers; retail's
//   linker stripped it.
//
//   So in a playlist, the first FoleyFXRegion of the next mission runs its ctor,
//   which walks smList to the tail to append itself, and follows the previous
//   mission's nodes into freed memory.  Only a map with foleyfx regions played
//   after another map with foleyfx regions trips it.
//
// THE FIX
//   Do what RemoveAll does on every mission start, from the same init_state hook
//   the other per-level resets use.  LuaHelper::InitState runs at the top of
//   GameState::PreStateInit: after the old heap is gone and before the world
//   (and its regions) loads, so nothing live is ever dropped.
//
//   A plain data write; smList is derived for modtools, Steam and GOG.
// =============================================================================

// Per-mission reset. Called from lua_hooks' init_state hook.
void foleyfx_region_reset();
