#pragma once

#include <stdint.h>

// Replaces the hardcoded 15 second multiplayer respawn delay with the host's own
// number ([Features] MPSpawnDelay, in seconds, defaulting to the same 15).
//
// Both Lua spawn-delay callbacks read the mission script's argument and then
// discard it whenever networking is on:
//
//     delay    = luaL_checknumber(1);
//     variance = luaL_checknumber(2);
//     if (netEnabled) delay = 15.0f;      // the script's value never lands
//     SpawnManager::SetSpawnDelay(sInstance, delay, variance, team);
//
// so no mission script and no host can change online respawn timing.  This
// substitutes the configured value for that 15.  It touches only the branch the
// engine already gates on netEnabled, so singleplayer is untouched on purpose.
//
// Only the host needs it.  Only the host runs Character::Spawn, and a client's
// countdown arrives as two raw fields over the wire in SpawnDisplay::Read, so
// clients follow a host-side change with nothing installed.
//
// Modtools carries the constant as an immediate and is patched in place; the
// retail builds load it from a shared .rdata literal with ~50 xrefs, so there the
// instruction's operand is repointed at a float this DLL owns instead.  See
// docs/RE/SpawnDelaySystem.md.

// Seconds. Clamped to [0.1, 300]; 15 is what the engine already does, so it is
// both the default and a no-op that leaves the sites untouched.
extern float g_mpSpawnDelay;

void mp_spawn_delay_install(uintptr_t exe_base);
