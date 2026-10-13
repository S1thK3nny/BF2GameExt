#pragma once

#include <stdint.h>

// =============================================================================
// Real flight on paths, a PROTOTYPE: AI flyers that fly the map's flyer paths
// chase a point along them by physics instead of being placed on the curve.
// Modtools only. BF2GameExt.log says how well they keep up.
//
// Stock, a flyer on its path is not flown: EntityFlyer::UpdatePathFollower hands
// its path follower the flyer's own matrix, and the follower puts the flyer on
// the path's curve each update (docs/RE/FlyerAI.md, "Fighter splines" and
// "Flying a path"). Here:
//
//   - Each such flyer gets a carrot, a matrix of our own. Every update
//     EntityFlyer::UpdateIndirect's detour (flyer_wingmen.cpp) steps the
//     flyer's path follower on the carrot instead, so everything the follower
//     does (branching, Variance, Frequency, EnableObject, the weave, turning on
//     an attacker) moves the carrot. The carrot runs about a second ahead of
//     the flyer, waiting when the flyer falls behind (flyer_real_flight_core.hpp).
//   - The flyer is held in free flight (mFreeFlyTime above 0), so the stock
//     code flies it by physics, and the call to CalculateDest steers it at the
//     carrot, along the carrot's heading, at the path's speed, as the wingmen
//     are steered at their slots. Danger regions and barriers then apply.
//   - Free flight would reset the follower every update: EntityFlyer::Update
//     calls EntityPathFollower::Reset whenever a flyer is not on its path. That
//     CALL goes to free_reset_mt, which keeps the follower of a flyer on real
//     flight.
//   - EntityFlyer::Update's CALL to UpdatePathFollower goes to
//     update_path_follower_mt: a flyer on real flight (or a wingman in
//     formation) whose free flight something cut short (TargetReact zeroes
//     it) is held again instead of placed, and the rest of the update flies it.
//   - A follower that finds no path leaves the flyer to the AI for 2 s, as the
//     stock code does; a node's LandOnArrival lands it, as the stock code does;
//     a flyer that falls far behind its carrot starts the carrot over from
//     where it is.
//
// READ ON MODTOOLS (Steam and GOG: not yet):
//
//   EntityPathFollower::Update      0x005EC8F0, thiscall(BranchParams*, float
//                                   dt, float* velocityOut, PblMatrix*
//                                   matrixOut, bool forceBranch, float
//                                   boostRatio, float energyRatio) on the
//                                   follower, RET 0x1C, bool in AL: false when
//                                   it has no path; moves BranchParams+4's matrix
//   GetBranchParams                 the follower's vptr +0x1C, thiscall(out*),
//                                   RET 4: 0x24 bytes (mPathClass +0, mMatrix +4,
//                                   ..., mFallbackToClosest +0x21)
//   EntityFlyer::UpdatePathFollower 0x004F30C0, thiscall(float dt, PblMatrix*)
//                                   on the entity, RET 8; how it calls the above
//                                   (boost ratio: max / mid speed when the class's
//                                   mBoostFlyerSpeed +0x898 is above 0; energy
//                                   ratio: +0x61C over the bar class's max at
//                                   +0x618, unless +0x620 bit 0); on a false it
//                                   gives 2 s of free flight; on mbLandNow it
//                                   lands the flyer
//   EntityFlyer::Update's CALLs     to UpdatePathFollower 0x004FDD93 (thunk
//                                   0x00411CA2), then, re-reading the same test
//                                   (mFreeFlyTime included), to
//                                   EntityPathFollower::Reset 0x004FDDED (thunk
//                                   0x00404133) when not on the path
//   EntityPathFollower::Reset       0x005E7290, thiscall(pathClass), RET 4
//   EntityFlyer::Land               0x004F1380, thiscall(), RET (the
//                                   LandOnArrival fix detours it)
//   EntityFlyer (from the entity)   the path follower +0x3CC (mPath +0xA8,
//                                   mbLandNow +0xD5 within it), network proxy
//                                   +0x78 bit 0, class +0x66C (its
//                                   EntityPathFollowerClass +0x6AC); the rest as
//                                   flyer_wingmen.hpp
//
// AI flies on the host and in single player only.
//
// ODF key on a flyer class, over the INI: PathFollowerRealFlight = 1 flies its
// paths for real, 0 places it as the stock game does.
//
// INI (unregistered while a prototype): [AI] FlyerRealFlight=1 for every AI
// flyer that flies paths.
// =============================================================================

extern bool g_flyerRealFlight;

namespace flyer_real_flight {

// What it borrows from flyer_wingmen.cpp, which owns the hooks: the original
// speed getters (thiscall() on the entity, the speed in ST0) and whether the
// wingmen hold a flyer.
using SpeedGetter = float(__fastcall*)(uint8_t* flyer, void* edx);
using HeldBy      = bool (*)(const uint8_t* flyer);

struct Borrowed {
   SpeedGetter maxSpeed = nullptr;
   SpeedGetter minSpeed = nullptr;
   SpeedGetter midSpeed = nullptr;
   HeldBy      wingman = nullptr;
};

// After the wingmen's hooks are in, during the installers' writable window.
bool install(uintptr_t exe_base, const Borrowed& borrowed);
void uninstall();

// The class key, from the wingmen's ODF listener: true when consumed.
bool on_property(void* cls, uint32_t hash, const char* value);
void on_derive(const void* parent, void* child);

// From the UpdateIndirect detour, before the original, for every flyer the AI
// flies that is not a wingman in formation.
void on_update(uint8_t* flyer, float dt);

// A flyer that has become a wingman in formation: its carrot goes.
void release(const uint8_t* flyer);

// From the CalculateDest stand-in, when the wingmen do not steer the flyer.
// steer_before swaps the AI's destination for the carrot and keeps what it
// replaced; steer_after puts it back and, unless CalculateDest lifted its
// `point` over terrain or an obstacle, holds the path's speed until the point
// is 60 degrees off the nose.
struct Steer {
   uint8_t* dest = nullptr;   // AILowLevel::mDest, while swapped
   uint8_t  saved[0x34] = {};
   float    point[3] = {};
   float    target = 0.0f;    // the speed asked for, a share of its top speed
   int      carrot = -1;
};
bool steer_before(uint8_t* flyer, Steer& st);
void steer_after(uint8_t* flyer, Steer& st, bool steered, const float* point, float* speed);
void restore(Steer& st);   // after a fault between the two

} // namespace flyer_real_flight
