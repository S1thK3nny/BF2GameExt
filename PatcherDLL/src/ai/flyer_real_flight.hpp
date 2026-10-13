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
//   - No barrier or danger region slides: CalculateDest checks for them once
//     the flyer's next check (+0x650) is due and slides the flyer off to one
//     side while the line ahead is blocked; a path through a danger region (a
//     capital ship's hangar sits in one on Cato Neimoidia) had flyers circling
//     their carrots for ever. Placed flyers never met them; this check is held
//     off for a flyer on real flight (FLT_MAX, given back when it stops).
//   - RaiseHeight's obstacle ray counts only what vehicles hit: it tested every
//     collision body, so a hangar shield that lets vehicles through kept flyers
//     in the hangar. Its CALL to CollisionManager::RayHit goes to raise_ray_mt,
//     which, for a flyer on real flight or a wingman in formation, sends the
//     ray out held to "ordnance" bodies while CollisionModel::RayHit (detoured)
//     reads each model's vehicle mask (VehicleCollision) as its ordnance mask.
//   - The chase (FlyerRealFlightPursuit): TargetReact's turn and attack sets the
//     follower's mAttacker, which its next step would bend the path toward.
//     Before each step the attacker is taken instead (cleared) and, when it
//     flies a flyer on another side, chased: the carrot waits and the flyer is
//     steered at where the target will be, at its speed once close behind it
//     (flyer_real_flight_core.hpp), for 15 s from the last provocation (40 s
//     at most), until the target dies, lands or gets 1000 m away. Then the
//     carrot starts over from the flyer, so it branches back onto its paths,
//     and it turns on no one for 4 s.
//   - RaiseHeight's ray passes over flyers for a chasing flyer, and for every
//     flyer with avoidance on (flyer_avoidance.hpp): a flyer it hits (a
//     CollisionObject whose GameObject, its vptr +0x38, is an EntityFlyer) is
//     added to the ray's ignore list and the ray cast again, at most 4 times;
//     else a flyer ahead makes it climb and slow to 20 m/s.
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
//   CalculateDest's barrier check   the flyer's +0x650 (read only there), against
//                                   the mission time (0x004F4687, 0x004F47DE)
//   RaiseHeight's CALL to RayHit    0x004F429E (thunk 0x00407581 to
//                                   CollisionManager::RayHit 0x0042E230,
//                                   cdecl, 9 arguments, ST0), types 0x18, the
//                                   flyer its one ignored object
//   CollisionModel::RayHit          0x004359C0, thiscall, 9 arguments, RET 0x24,
//                                   ST0; ordnance mask +0x12C, vehicle +0x114
//   EntityFlyer (from the entity)   the path follower +0x3CC (mPath +0xA8,
//                                   mAttacker +0xB4, mbLandNow +0xD5 within it),
//                                   network proxy +0x78 bit 0, class +0x66C (its
//                                   EntityPathFollowerClass +0x6AC); the rest as
//                                   flyer_wingmen.hpp
//   TargetReact                     0x004F0C30, thiscall(attacker Controllable)
//                                   on the entity, RET 4, from the attacker's
//                                   EntityFlyer::Update (0x004FDD10); turn and
//                                   attack writes the attacker's Character (its
//                                   Controllable +0xCC) to mAttacker (0x004F0D4C)
//   ShotAtByEnemy                   0x005EBDA0, from the follower's Update
//                                   (0x005ECA9E): mAttacker's flyer is its
//                                   Character's remote +0x150, else vehicle
//                                   +0x14C, else unit +0x148
//   the ray callback                0x00429E00: a CollisionObject's GameObject
//                                   from its vptr +0x38, against the ignore list
//
// AI flies on the host and in single player only.
//
// ODF key on a flyer class, over the INI: PathFollowerRealFlight = 1 flies its
// paths for real, 0 places it as the stock game does.
//
// INI: [AI] FlyerRealFlight=1 for every AI flyer that flies paths (off by
// default); FlyerRealFlightPursuit=0 leaves the turn on an attacker to the
// game (on by default).
// =============================================================================

extern bool g_flyerRealFlight;
extern bool g_flyerRealFlightPursuit;

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
void release(uint8_t* flyer);

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
   float    keepCos = 0.5f;   // that speed held until the point is this far off its nose (a cosine)
   int      carrot = -1;
};
bool steer_before(uint8_t* flyer, Steer& st);
void steer_after(uint8_t* flyer, Steer& st, bool steered, const float* point, float* speed);
void restore(Steer& st);   // after a fault between the two

// For the AI squadrons (flyer_wingmen.cpp):
bool pursuing(const uint8_t* flyer);        // on real flight, chasing its attacker
bool on_real_flight(const uint8_t* flyer);  // its carrot kept by its last update
// The path a flyer on real flight is flying (its follower's), or null while it
// waits for one or chases.
const uint8_t* squadron_path(const uint8_t* flyer);
// A wingman's attacker (its follower's mAttacker, which TargetReact sets) handed
// to its leader to chase, unless the leader has one or chases already; with no
// leader, dropped. The wingman's follower is not stepped while it flies in
// formation, so left there it would start a chase when it is let go.
void pass_attacker(uint8_t* wingman, uint8_t* leader);

} // namespace flyer_real_flight
