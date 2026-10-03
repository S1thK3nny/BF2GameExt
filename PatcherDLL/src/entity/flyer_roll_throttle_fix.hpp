#pragma once

#include <stdint.h>

// =============================================================================
// Flyers keep their full throttle and roll while both are held.
//
// PlayerController::Update reads the forward and back input (mControlMove) and
// the sideways input (mControlStrafe) as one stick, and scales both down to its
// rim when together they reach past it:
//
//    len = sqrt(move * move + strafe * strafe);
//    if (len > 1) { move /= len; strafe /= len; }
//
// That keeps a soldier running diagonally (W and A) from going about 1.4x as
// fast as one running straight. Every controllable a player drives goes through
// it, and a flyer uses the two inputs as its throttle and its roll, so holding
// the throttle while rolling reaches the flyer as 0.71 of each: it slows toward
// a lower speed (MidSpeed plus 0.71 of the way to MaxSpeed) and rolls more
// slowly until the roll ends, then speeds back up. Nothing in that code looks
// at the kind of unit; it is the soldier rule carried over.
//
// THE FIX. The compare against 1 becomes a jump to a shim that skips the
// scaling when the controllable is an EntityFlyer (by its GameObject's RTTI)
// and otherwise runs the original compare. A held key stays 1; a stick inside
// the rim is untouched either way.
//
// Sites, the compare (the scaling it skips follows; the stores come after):
//   modtools 0x0059BB37  FCOM [1.0]; FNSTSW AX; TEST AH,0x41; JNE  (EBX = this)
//   steam    0x0061A81F  COMISS XMM1,[1.0]; JBE                    (EDI = this)
//   gog      0x0061B88F  the same as Steam
// mOwner, the Controllable being driven, is at this + 4 on every build. Read
// on Phantom (PlayerController::Update 0x0071CAA0, the compare at 0x0071D013)
// and found by the same code on the others.
//
// Only a player's own flyer changes, on whichever machine runs their
// controller. Online behaviour is untested: see docs/user/MULTIPLAYER.md.
// =============================================================================

extern bool g_flyerRollThrottleFix;

void flyer_roll_throttle_fix_install(uintptr_t exe_base);
void flyer_roll_throttle_fix_uninstall();
