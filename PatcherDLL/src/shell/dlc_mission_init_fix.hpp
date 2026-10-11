#pragma once

#include <stdint.h>

// DLC mission-list initialization fix — port of PrismaticFlower's upstream
// e8b6fa7.  Launching straight into a mod map from the commandline fails
// because the DLC mission list is only populated by GameState::ShellState::Enter.
// If MissionState is entered without the shell ever having run, we enter and
// immediately exit the shell state first so addon scripts get a full, normal
// Lua context.

extern bool g_dlcMissionInitFixEnabled;

void dlc_mission_init_fix_install(uintptr_t exe_base);
void dlc_mission_init_fix_uninstall();

// DLC mission table size once the DLC Mission Limit Extension applies (stock 500).
constexpr uint32_t kDlcMissionLimit = 0x1000;

// Install callback of the DLC Mission Limit Extension patch set. Lifts the
// unofficial v1.3 patch's Lua-side 500 mission cap to the raised table size.
bool dlc_v13_mission_cap_install(uintptr_t exe_base);
