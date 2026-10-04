#pragma once

#include <stdint.h>

// =============================================================================
// Camera shake: the stock shake redrawn as a smooth blast, and ODF-driven shake
// for what the local player does and what happens to them.
//
//   FireShake        weapon                 each shot
//   SwingShake       melee weapon           each swing, in place of FireShake
//   StrikeShake      melee weapon           a swing landing on something
//   SwingBlockedShake melee weapon          a swing that something blocks
//   BlockShake       melee weapon           blocking a melee strike
//   DeflectShake     melee weapon           deflecting a bolt or beam
//   HitShake         any unit               taking damage
//   LandShake        soldier, walker, hover landing a jump or fall
//   RollShake        soldier                a combat roll (third person)
//   SprintShake      soldier                sprinting (third person)
//   StepShake        walker                 a foot landing, rolled toward it
//   JumpShake        walker, hover          a jump starting
//   MoveShake        hover                  moving, by how fast
//   BoostShake       flyer, walker, hover   going fast: by default boosting
//   TurnShake        flyer, walker          a flyer turning hard for a while;
//                                           a walker turning on the spot
//   BrakeShake       flyer                  slowing down with the brake held
//   CollisionShake   flyer, hover           bumping into something; a hover
//                                           tilts toward what it hit
//   TrickRollShake   flyer                  a barrel roll
//   TrickFlipShake   flyer                  a flip
//   TakeoffShake     flyer                  lifting off
//   LandingShake     flyer                  touching down
//   BlastShake       any unit               explosions, walker deaths and flyer
//                                           crashes; on by default
//
// Each takes the same detail properties after its name: Pitch, Yaw, Roll (in
// degrees; "min max" picks one each time), Push (metres back along the view),
// Length (seconds; for a shake that lasts, its fade in and out together),
// Rise (share of the length, rising or fading in), Rate (swings per second),
// Limit (how many can run at once, added together, up to 8; for BlastShake,
// where it levels off), Threshold (when a flyer's boost, turn, brake or collision
// shake, a walker's step, boost or landing, or a hover's movement, boost,
// landing or collision plays, in its own measure),
// Steady (the boost and brake shakes' share
// once the unit gets where it is going), PushOnce (the push goes out and
// back once instead of swinging with the rate; on except for BlastShake) and
// Teammates (whether StrikeShake counts a swing that lands only on teammates;
// on unless set to 0).
// The reticule is kept on the unshaken view.
// Anything a class leaves out comes from the defaults in camera_shake_core.hpp.
// Every property inherits through ClassParent; apart from BlastShake, a shake
// does nothing until an ODF sets one of its properties.
//
// The shake moves only the rendered view, after the aim has been taken from the
// unshaken camera, so shots land where they would without it. Client-local: it
// changes nothing in the simulation or on the network (whether a client runs
// the melee and hover collision code it watches for its own unit is not known
// yet). Modtools, Steam and GOG. The research and every address are in
// docs/RE/CameraShake.md.
// =============================================================================

// The shakes, in the order of their ODF names (kShakeNames in the .cpp).
enum CameraShakeChannel {
   kShakeFire,
   kShakeHit,
   kShakeLand,
   kShakeRoll,
   kShakeSprint,
   kShakeBrake,
   kShakeBlast,
   kShakeBoost,
   kShakeTurn,
   kShakeCollision,
   kShakeTrickRoll,
   kShakeTrickFlip,
   kShakeTakeoff,
   kShakeLanding,
   kShakeSwing,
   kShakeStrike,
   kShakeBlock,
   kShakeDeflect,
   kShakeSwingBlocked,
   kShakeStep,
   kShakeJump,
   kShakeMove,
   kCameraShakeChannels
};

void camera_shake_install(uintptr_t exe_base);
