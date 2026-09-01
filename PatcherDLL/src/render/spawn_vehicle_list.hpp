#pragma once

#include <stdint.h>

// Restores the BF1 spawn-screen behaviour where highlighting a command post
// lists the vehicles that spawn there.
//
// BF2 shipped every part of this except the code that produces the text:
//
//   * the stock .hud files already declare the text element
//     (Text("player1spawnvehicle") in data/Common/hud/PC/1playerhud.hud), already
//     positioned and already bound to spawnDisplay.enable/.disable, so it appears
//     and disappears with the spawn screen exactly as intended;
//   * the HUD event player%d.spawnDisplay.vehicle is still registered as
//     type_String in HUD::GameEvents::Open on every build;
//   * VehicleSpawn still records its command post and its per-team vehicle
//     classes, because normal vehicle spawning depends on both.
//
// Only SpawnDisplay::UpdateSpawnpointText was cut (Phantom 0x007D8AD0 is a bare
// RET 4 with no callers; modtools and Steam dropped it, and SpawnVehicleList
// with it).  Nothing has ever sent that event a value, so the element renders
// blank.  We supply the string; no data changes are required.
//
// Full write-up, including the BF1 original and the address tables:
// docs/RE/CommandPostVehicleList.md
//
//   INI: [Features] SpawnVehicleList=1
//
// Modtools and Steam only.  The GOG address set has not been derived yet, so the
// feature declines there (and on unidentified builds) and logs why.

extern bool g_spawnVehicleListEnabled;

void spawn_vehicle_list_install(uintptr_t exe_base);
void spawn_vehicle_list_uninstall();
