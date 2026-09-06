#pragma once

#include <stdint.h>

// =============================================================================
// Out-of-range soldier animation index crash fixes.
//
// Two engine bugs with one shared cause: SoldierAnimatorClass::AnimationMap
// stores 164 animation slots per map and nothing in the engine range-checks an
// animation index against that.
//
//  1. The animation getters are bare table reads.  An index past 163 addresses
//     the NEXT map's block and returns whatever sits there - a non-NULL value
//     that is not a pointer - and every consumer only tests for NULL.  An
//     unpopulated slot is 0xFFFFFFFF (the maps are `rep stosd`-filled with -1),
//     which also survives a NULL test.  Both are dereferenced.
//
//     [LimitIncreases] ComboAnimIncrease is what makes this reachable: it moves
//     the "no animation" sentinel from 164 to 254 at 25 sites so the engine
//     starts handing out indices it has nowhere to store.
//
//  2. Combo::Attack::_ResolveDamageData, the melee damage-ray resolver, does
//     not even NULL-check its lookup, unlike its sibling
//     Combo::ResolveForWeapon which does.
//
// The getter clamp fixes (1) for every consumer at once by making an
// out-of-range index and an unpopulated slot both return the engine's own
// "no animation" answer, which every caller but one already handles.  The
// resolver guard covers that one caller.
//
// Indices past 163 therefore stop crashing, but the animation authored there is
// still unreachable until the per-map block is widened - see
// docs/RE/ComboAnimationLimit.md.
//
// No INI toggle: the byte/entry checks are the safety, not an off-switch.
// Fixed on modtools, Steam and GOG.
// =============================================================================

void combo_damage_anim_guard_install(uintptr_t exe_base);
void combo_damage_anim_guard_uninstall();
