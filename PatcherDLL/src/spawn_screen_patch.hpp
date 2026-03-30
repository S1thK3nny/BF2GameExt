#pragma once

#include <stdint.h>

// Port Xbox spawn screen flow to PC.
// Replaces the single combined ifs_pc_SpawnSelect with the two-screen
// Xbox flow: ifs_charselect (class select) + ifs_mapselect (spawn point).
// INI: [Hooks] SpawnScreen=1
void spawn_screen_install(uintptr_t exe_base);
void spawn_screen_uninstall();
