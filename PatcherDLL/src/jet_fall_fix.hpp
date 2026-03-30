#pragma once

#include <stdint.h>

// Fix JET -> FALL animation bug.
//
// When AI soldiers exit the jetpack state (JET_JUMP or JET_HOVER),
// EndJetJump transitions them to FALL state but the SoldierAnimator's
// mSoldierAction field still holds the JET state value. The animation
// system doesn't have a JET->FALL transition so it defaults to walk/run,
// making the soldier run mid-air.
//
// Normal JUMP->FALL works because the animator sees mSoldierAction=JUMP
// transitioning to FALL, which has proper animation handling.
//
// Fix: Detours hook on EndJetJump. After the original sets state to FALL,
// override mOldState to JUMP and set the SoldierAnimator's mSoldierAction
// to JUMP so the animation system treats it as a JUMP->FALL transition.

void jet_fall_fix_install(uintptr_t exe_base);
void jet_fall_fix_uninstall();
