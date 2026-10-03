#pragma once

#include <stdint.h>

// =============================================================================
// EntityFlyer - offsets from the start of the object: the `this` of
// EntityFlyer::DoTrick, and what Trackable::GetGameObject returns for it
// (GameObject is its primary base). EntityFlyer_data starts at +0x578 on
// modtools and +0x538 on Steam and GOG; the fields below keep their PDB order.
//
//   field                  data   modtools  Steam/GOG  read at
//   mVelocity (PblVector3) +0x08  0x580     0x540      RecalculateSpeed  mt 0x4F304A  st 0x4ABD07
//   mState                 +0x2C  0x5A4     0x564      TakeOff / Land (flyer_boost_animation.cpp)
//   flag byte              +0x7C  0x5F4     0x5B4      DoTrick TEST [..],3 mt 0x4F3DBF;
//                                                      bit 0x01 rolling (RollAdd), 0x02 flipping
//                                                      (FlipAdd), 0x04 boosting
//   mGetSpeedSpeed         +0x80  0x5F8     -          RecalculateSpeed FSTP mt 0x4F3072: the
//                                                      speed along the flyer's forward axis
//   mInLandingRegionFactor +0x84  0x5FC     -          GetFlyerMaxSpeed mt 0x4F0956: non-zero
//                                                      inside a landing region, where BF2
//                                                      caps the flyer's speeds
//   mTrick                 +0x98  0x610     0x5D0      DoTrick: 0 on a trick, -1.0 when refused
//   mClass                 +0xF4  0x66C     0x62C      DoTrick mt 0x4F3D19  st/gog 0x4B18FB
//
// The flyer's matrix keeps its forward axis at +0x110 on modtools, the row
// RecalculateSpeed takes mVelocity along (mt 0x4F3050 on).
//
// Its Controllable part, at +0x240 on modtools (EntityFlyer::Update reaches the
// flyer from it with LEA ECX, [EBX - 0x240] at 0x4FD86D), holds mControlMove,
// the forward and back input, at +0x80, so +0x2C0 on the flyer: Update loads
// it (mt 0x4FD376) and turns it into the speed it steers toward (mt 0x4FECF9
// on), BoostSpeed while boosting with one, else MidSpeed plus the input's
// share of the way to MaxSpeed, or back toward MinSpeed. mControlStrafe, the
// roll input, follows at +0x84 (+0x2C4 on the flyer), read next with a 0.3 dead
// zone (mt 0x4FD39D). PlayerController::Update (Phantom 0x0071CAA0) treats the
// two as one stick and scales both down when together they reach past its
// rim, so full throttle held through a roll reaches the flyer as 0.71.
//
// EntityFlyerClass keeps the ODF's MinSpeed, MidSpeed, MaxSpeed and BoostSpeed
// together from +0x88C (Phantom and modtools: GetFlyerMinSpeed 0x4F0A31,
// GetFlyerMidSpeed 0x4F09CB, GetFlyerMaxSpeed 0x4F096B, RecalculateSpeed's
// BoostSpeed 0x4F2FDA), and PitchRate and TurnRate at +0x8A0 and +0x8A4
// (Update, mt 0x4FD752 and 0x4FD758).
//
// A 0 below is a field not yet read on that build; what needs it stays off.
// Details: docs/RE/CameraShake.md.
// =============================================================================

namespace layout::Flyer {

// EntityFlyer::State (PDB enum): 0 LANDED, 1 TAKEOFF, 2 FLYING, 3 LANDING,
// 4 CRASHING, 5 CRASHED.
constexpr int     kStateLanded  = 0;
constexpr int     kStateTakeoff = 1;
constexpr int     kStateFlying  = 2;
constexpr int     kStateLanding = 3;
constexpr uint8_t kFlagRoll    = 0x01; // flag byte: rolling (a side roll, or the roll of a flip)
constexpr uint8_t kFlagFlip    = 0x02; // flag byte: flipping
constexpr uint8_t kFlagBoost   = 0x04; // flag byte: boosting

struct Offsets {
   uint32_t velocity;
   uint32_t state;
   uint32_t flags;
   uint32_t trick;
   uint32_t cls;
   uint32_t speed;          // mGetSpeedSpeed
   uint32_t forward;        // the matrix's forward axis, x y z
   uint32_t classSpeeds;    // in the class: MinSpeed, then MidSpeed, MaxSpeed, BoostSpeed
   uint32_t classTurnRates; // in the class: PitchRate, then TurnRate
   uint32_t move;           // mControlMove: forward and back input, -1 to 1
   uint32_t landing;        // mInLandingRegionFactor
   uint32_t roll;           // mControlStrafe: the roll input, -1 to 1
};

constexpr Offsets kModtools = { 0x580, 0x5A4, 0x5F4, 0x610, 0x66C, 0x5F8, 0x110, 0x88C, 0x8A0, 0x2C0, 0x5FC, 0x2C4 };
constexpr Offsets kRelease  = { 0x540, 0x564, 0x5B4, 0x5D0, 0x62C, 0, 0, 0, 0, 0, 0, 0 }; // Steam and GOG

} // namespace layout::Flyer
