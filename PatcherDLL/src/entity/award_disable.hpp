#pragma once

#include <stdint.h>

// =============================================================================
// DisableAwardBuffs / DisableAwardWeapons - [Features] INI toggles.
//
//   DisableAwardBuffs   = 1   // no permanent combat-award buffs
//   DisableAwardWeapons = 1   // no combat-award weapons
//
// Both off by default; leaving them at 0 keeps stock behaviour exactly.
//
// Combat awards ("medals") are earned per class and, once the career level is
// high enough, become permanently available. Four of the nine grant a passive
// that never expires; the other five hand out an upgraded weapon. This lets a
// mod take either group away without touching the buff system itself - officer
// buff weapons and buff pickups keep working unchanged.
//
// Build-aware (modtools + Steam + GOG): install from dllmain's build-aware
// section.
// =============================================================================

// Set from the INI before award_disable_install runs.
extern bool g_disableAwardBuffs;
extern bool g_disableAwardWeapons;

void award_disable_install(uintptr_t exe_base);
void award_disable_uninstall();
