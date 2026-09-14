#pragma once

#include <stdint.h>

// Guards missing soldier animations before the melee damage-ray resolver,
// which dereferences its animation lookup without checking for a missing clip.
//
// With ComboAnimIncrease active, the resolver uses its shared expanded lookup
// and this module leaves the feature's body getter detours alone. Otherwise it
// clamps the two stock body getters to 30 maps and logical indices 0..163.
// Reset-fill 0xFFFFFFFF and unassigned index 0xFF never become live animations.
//
// No INI toggle: entry-byte checks gate installation independently on each
// supported build. See docs/RE/ComboDamageResolver.md.

void combo_damage_anim_guard_install(uintptr_t exe_base);
void combo_damage_anim_guard_uninstall();
