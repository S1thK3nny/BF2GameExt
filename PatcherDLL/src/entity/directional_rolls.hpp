#pragma once

#include <stdint.h>

// =============================================================================
// UseDirectionalRolls: soldiers roll to the side with side-dive animations.
//
// STOCK. A roll can already go any way the stick points except mostly back:
// EntitySoldier::Update moves a rolling soldier by its move and strafe input.
// Its animation is always DIVE, "diveforward": for a roll to the side,
// SoldierAnimator::SetupPose turns the body toward the move (the DIVE case
// passes atan2 of mMovement against the body's right and forward rows as the
// leg angle, RotateLowerBody turns the root by it and the upper body twists
// back toward the aim), so the soldier dives forward, turned sideways.
// Directional jumps show how BF2 does it when it wants to: an ODF switch
// (UseDirectionalJumps, a class flag the animator copies), a rule in
// SoldierAnimator::SetAction (faster than 2 m/s and the sideways part at least
// the forward part: jump_left or jump_right), names looked up like any other
// animation, and a fallback (a missing jump_left plays jump).
//
// THIS. A soldier class with UseDirectionalRolls = 1 (inherited through
// ClassParent) does the same for rolls. When such a soldier starts a roll, the
// roll is sorted by SetAction's rule for jumps, from the body's move that
// frame. A roll to the side plays "diveleft" or "diveright", found the way
// AnimationFinder finds any action animation for the soldier's animation map:
// "<bank>_<weapon>_<anim>" with "_upper" or "_lower", then plain, then
// "_full", trying the map's bank and its parents for the map's weapon, then
// for each parent weapon (so the pistol takes the tool's and then the rifle's,
// and a custom bank its parent bank's), hashed with PblTEMPHash and looked up
// in the loaded banks by SoldierAnimatorClass::FindAnimation. A half of the
// body without one keeps diveforward's, the way a missing jump_left keeps
// jump's; a side with neither half rolls as stock.
//
// For the length of that soldier's SetupPose, the DIVE entries for its map
// point at copies of diveforward's SoldierAnimations carrying the side dive
// (blend data and all), and mMovement is turned a quarter turn back in the
// body's plane, so the leg angle SetupPose aims for points the dive's root a
// quarter turn from the soldier's real move. A side dive moves a quarter turn
// from its root, so it follows the move the way a forward dive does: a roll
// straight to the side starts facing the aim, and when the soldier turns with
// the camera mid-roll (the roll's mTurnFactor turns the body) the root turns
// back to stay on the move. Both are put back as SetupPose returns. Forward
// rolls, soldiers without the property and every other action are untouched.
//
// The tables, the maps and banks they are looked up through, and the lookup
// itself come from soldier_anim_tables, which reads them where the exe keeps
// them or, with [LimitIncreases] ComboAnimIncrease, where that keeps them.
//
// Only the animation changes. The roll's movement and length are the engine's
// (it ends at three quarters of human_rifle diveforward's length whatever the
// weapon, so a side dive should be at least that long), and nothing is sent
// over the network: each machine picks for every soldier it draws, AI and
// remote players included, from the move it sees.
//
//                                 modtools    Steam       GOG
//   SoldierAnimator::SetupPose    0x0057C490  0x0063FAA0  0x00640B40  detoured
//     thiscall(RedPose*), RET 4
//   SoldierAnimatorClass::sInstance  0x00B8D3C4  0x01EAFB1C  0x01EB0FD0
//   GetUpperBodyActionAnimation   0x0057DCA0  0x00643940  0x006449E0  checked*
//   GetLowerBodyActionAnimation   0x0057DCC0  0x00643960  0x00644A00  checked*
//   SoldierAnimatorClass::FindAnimation (thiscall(hash, name), RET 8)
//                                 0x0057DE40  0x006442A0  0x00645340
//   s_aBank / s_aWeapon / s_aMap  0x00ACECF8  0x007E9440  0x007EA070
//                                 0x00ACF198  0x007E9070  0x007EA330
//                                 0x00ACF558  0x007E9700  0x007EA8E0
//   GameLoop::sClientDeltaTime    0x00C6A9AC  0x01E56058  0x01E574F0
// * By soldier_anim_tables, unless ComboAnimIncrease replaced them: not
// hooked, their code is the stock action table's layout (SoldierAnimator.h).
// Research: docs/RE/SoldierActionAnimations.md.
//
// No INI key: the ODF property is the switch.
// =============================================================================

void directional_rolls_install(uintptr_t exe_base);
void directional_rolls_uninstall();

// Forgets the last level's animations; from LuaHelper::InitState.
void directional_rolls_reset();
