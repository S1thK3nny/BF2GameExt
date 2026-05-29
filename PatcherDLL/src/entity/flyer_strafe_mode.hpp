#pragma once

#include <stdint.h>

// =============================================================================
// Flyer Strafe Mode
//
// Re-enables the abandoned EntityFlyer "strafe" feature on a per-class basis
// and replaces the normal turn->roll ("rolling") behaviour with sideways
// strafing for flyer classes that opt in via a new ODF property:
//
//   [EntityClass]
//   FlyerStrafeMode = 1          // 0/off (default), 1/on
//   StrafeSpeed     = 25.0       // lateral speed (units/sec at full stick)
//   StrafeRollAngle = 0.0        // optional visual lean while strafing
//                                //   (0 = fly perfectly level; +/- sets lean direction)
//
// When enabled, the player's turn axis (mControlTurn) no longer rolls/yaws the
// flyer; instead it drives lateral (strafe) velocity along the flyer's right
// axis.  Forward thrust and pitch are unchanged.
//
// Only the LOCAL human player's flyer is affected (mPlayerId == 0); AI and
// remote-piloted flyers of the same class keep vanilla rolling so their
// navigation is unaffected.
//
// Background: StrafeSpeed / StrafeRollAngle are parsed into EntityFlyerClass
// by the engine but every runtime use is multiplied by 0.0 (dead code).  This
// module reinstates them through an EntityFlyer::Update hook.
// =============================================================================

void flyer_strafe_mode_install(uintptr_t exe_base);
void flyer_strafe_mode_uninstall();
void flyer_strafe_mode_reset();
