#pragma once

#include <stdint.h>

// =============================================================================
// Soldier upper-body animation crash guard
//
// SoldierAnimator's upper-body update (FUN_00578970, reached via the thunk in
// vtable slot 0x00A4A6F0) reads the incoming SoldierAnimation's +8 field (the
// pointer to its anim data block) WITHOUT a null/validity check, at 0x005789e8:
//     MOV EDI,[EBX+8] ; EDI = anim->+8
//     MOV CL,[EDI]    ; <-- CTD when +8 is garbage
//
// When a soldier class exceeds the hard 16-distinct-bank-NAME cap, AddBank
// (0x5704b0) returns -1 for the 17th name, the resulting animation map entry
// resolves through _GetBank(-1) (reads before the registry array → garbage),
// and the SoldierAnimation handed to this update has a dangling +8 → the game
// crashes mid-match. See memory/soldier-animation-bank-limit.
//
// This guard swaps the vtable slot for a wrapper that validates anim->+8 is
// readable; if not, it skips the update for that frame (the unit's upper body
// simply doesn't animate) instead of crashing, and logs the offending anim id
// once so the over-limit class can be identified and reworked to <=16 names.
// =============================================================================

void soldier_upperbody_anim_guard_install(uintptr_t exe_base);
void soldier_upperbody_anim_guard_uninstall();
