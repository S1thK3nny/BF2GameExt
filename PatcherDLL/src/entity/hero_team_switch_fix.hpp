#pragma once

#include <stdint.h>

// =============================================================================
// "Cannot switch teams after dying as a hero" fix.
//
// Engine bug, present on all three builds: Character::mHeroFlag is written in
// exactly one place, Character::Spawn, and nothing ever clears it.  Dying does
// not reset it, so a player who dies as a hero keeps mHeroFlag set for as long
// as they are dead - and Character::ChangeTeam refuses outright when that flag
// is set.  The refusal lasts until the next spawn overwrites the flag from the
// newly spawned unit, which is why respawning as a regular unit "fixes" it.
//
// The fix makes the refusal apply only while the character actually has a unit,
// so a live hero still cannot switch sides but a dead one can.  See the .cpp
// for the per-build codegen.
// =============================================================================

void hero_team_switch_fix_install(uintptr_t exe_base);
void hero_team_switch_fix_uninstall();
