#pragma once

#include <stdint.h>

// =============================================================================
// Animation Bank Append — sub-bank merging across .lvl files
//
// Hooks AnimationFinder::_AddBank so that numbered sub-banks loaded from a
// later .lvl file are picked up even if the base bank was already iterated.
//
// Use case: the vanilla "human" bank is human_0 through human_4.  To add
// new animations (e.g. prone) without overwriting those (which mods may
// replace), they go in human_5.  A mod's dc:ingame.lvl loads human_0-4
// first, then vanilla ingame.lvl loads human_0-5.  The engine's NULL guard
// preserves the mod's 0-4 and creates human_5 in the hash table, but the
// bank iteration already finished.  This hook catches human_5 on the next
// _AddBank call and appends it.
//
// Works for any bank, not just human.  New animations must be in a new
// numbered sub-bank — individual animations within an existing sub-bank
// cannot be appended (first-loaded version wins).
// =============================================================================

void anim_bank_append_install(uintptr_t exe_base);
void anim_bank_append_uninstall();
