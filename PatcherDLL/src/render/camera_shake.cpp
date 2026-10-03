#include "pch.h"
#include "camera_shake.hpp"
#include "camera_shake_core.hpp"
#include "core/entity_layout.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "game/Battlefront2/Source/ChaseCamera.h"
#include "game/Battlefront2/Source/CollisionObject.h"
#include "game/Battlefront2/Source/EntityFlyer.h"
#include "game/Battlefront2/Source/EntityHover.h"
#include "game/Battlefront2/Source/EntityWalker.h"
#include "game/Battlefront2/Source/RedCamera.h"
#include "game/Battlefront2/Source/Weapon.h"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "entity/odf_gameext_props.hpp"
#include "util/install_log.hpp"

#include <detours.h>

#include <cfloat>
#include <cmath>
#include <cstring>
#include <unordered_map>

// =============================================================================
// What it hooks and calls, read off each build (names from the Phantom PDB):
//
//                                 modtools     Steam        GOG
//   ChaseCamera::SetupCamera      0x004A2440   0x00453D00   0x00453CE0   detoured
//     thiscall(RedCamera*, float dt), RET 8
//   RedCamera::SetMatrix          0x007FEED0   0x006CBEF0   0x006CCF90   called
//     thiscall(const PblMatrix*), RET 4
//   Tracker::IsFirstPersonView    0x0049FDD0   0x0044E3C0   0x0044E3A0   called
//     thiscall(), bool in AL, RET
//   EntityFlyer::DoTrick          0x004F3D10   0x004B18F0   0x004B18F0   detoured
//     thiscall(Trick), RET 4
//   Weapon::SignalFire            game_addrs weapon_signal_fire         detoured
//     thiscall(), RET; controller rumble detours it too
//   ReticuleDisplay::Update       0x00683270   0x00630650   0x006316F0   detoured
//     thiscall(float dt), bool in AL, RET 4; the widescreen fix detours it too
//   CameraManager::ApplyShake     0x004A0690   -            -            called
//     thiscall(float amount, float duration), RET 8
//   its CALL in EntityFlyer::     0x004F7F3A   -            -            retargeted
//     PostCollisionUpdate, and in
//     CollisionCallback           0x00503230   -            -            retargeted
//     both through the thunk at 0x004162D4
//   WeaponMelee::UpdateFire       0x00639020   0x0068C230   0x0068D2C0   detoured
//     thiscall(float dt), bool in AL, RET 4
//   WeaponMelee::Deflect          0x00637670   0x0068A550   0x0068B5E0   detoured
//     thiscall(Ordnance*, const PblVector3*, const PblVector3*), bool in AL, RET 0xC
//   EntityHover::CollisionCallback 0x005155B0  0x004C66A0   0x004C66A0   detoured
//     thiscall(CollisionResult*, CollisionObject*, Restrictor*), bool in AL,
//     RET 0xC; `this` is the hover + 0xC; CommandHover shares it
//
// Same on every build: the chase camera fields and its shake queue
// (layout::ChaseCamera); the render camera's zoom (layout::RedCamera); the
// Trackable's GetGameObject (+0x1C) and GetControllable (+0x28) slots, the two
// Tracker::IsFirstPersonView itself calls; a Controllable's Trackable part at
// +0x18; GameObject::IsRtti (+0x00, thiscall(hash), RET 4) and GetEntityClass
// (+0x28), as in hud_class_icons.cpp; a GameObject's Damageable at +0x140, as
// controller_rumble.cpp and aim_assist.cpp read it; a GameObject's mTeam bits
// at +0x234; the Weapon vtable's IsMelee (+0x54). Per build: the soldier's
// mState, at Controllable + g_soldier->mState; WeaponMelee's list of what a
// swing struck (layout::WeaponMelee). The flyer fields are per build
// (layout::EntityFlyer); those not yet read on Steam and GOG are 0 there, and so
// are the collision sites, so what needs them stays off on those builds. The
// walker and hover fields are per build too (layout::EntityWalker and
// layout::EntityHover), read on all three, and a hover's collision callback
// reads the other object and the contact as BF2's own callback does
// (layout::CollisionObject, layout::CollisionResult), the same on every build.
//
// How the shake gets drawn: SetupCamera builds mMatrix, turns it by the stock
// shake once mission time is past mShakeSuppressUntil, and hands it to
// RedCamera::SetMatrix. The hook holds mShakeSuppressUntil at FLT_MAX for the
// call, so the stock turn is skipped, then moves and turns mMatrix by its own
// offset, the stock queue drawn as a blast included, and sets the camera again. The aim never
// reads mMatrix (docs/RE/CameraShake.md), so shots are unaffected.
//
// SetupCamera builds mMatrix from the one it left last frame: the owner's
// SetupCameraTrackMatrix eases the camera from there toward where it should be
// (CameraTrackSetting::SetupCameraTrackMatrix), and ChaseCamera blends from
// there too while it settles. So before calling it, the hook puts back the
// matrix SetupCamera made last frame in place of the shaken one (LeftShake),
// and a shake never feeds BF2's easing (docs/RE/CameraShake.md).
//
// The reticule is the one thing that would follow the shake: ReticuleDisplay::
// Update places it by projecting the aim point through the render camera's
// _MatrixInverse, so it chased the aim point across the shaken view. Its detour
// puts the unshaken inverse back for that call only, so the reticule holds still.
// =============================================================================

namespace {
using namespace camera_shake;
namespace cam = layout::ChaseCamera;
namespace fly = layout::EntityFlyer;
namespace fly_class = layout::EntityFlyerClass;
namespace walk = layout::EntityWalker;
namespace walk_class = layout::EntityWalkerClass;
namespace hov = layout::EntityHover;
namespace hov_class = layout::EntityHoverClass;
namespace coll = layout::CollisionObject;

// The ODF-driven shakes. Always on; off only if the ODF reader had no
// listener slot left for their properties, when just the blast is drawn.
bool s_odfShakes = true;

constexpr uint32_t kVt_GetGameObject   = 0x1C;  // Trackable vtable
constexpr uint32_t kVt_GetControllable = 0x28;  // Trackable vtable
constexpr uint32_t kCtrl_Trackable     = 0x18;  // a Controllable's Trackable part
constexpr uint32_t kVt_IsRtti          = 0x00;  // GameObject primary vtable
constexpr uint32_t kVt_GetEntityClass  = 0x28;  // GameObject primary vtable
constexpr uint32_t kVt_GetVelocity     = 0x44;  // GameObject primary vtable: const PblVector3*

// A GameObject's Damageable part is at +0x140 (Phantom PDB: the vptr at +0x140,
// Damageable_data from +0x144), on every build.
constexpr uint32_t kObj_Health    = 0x144;  // Damageable::mCurHealth
constexpr uint32_t kObj_MaxHealth = 0x148;  // Damageable::mMaxHealth
// GameObject::mTeam, a 4-bit signed field in the low bits, on every build (as
// spawn_vehicle_list.cpp reads it; WeaponMelee::UpdateFire takes the owner's
// team the same way, SHL 0x1C / SAR 0x1C).
constexpr uint32_t kObj_Team      = 0x234;

// RTTI hashes are PblHash of the class name, hashed by each class's static
// initialiser (EntityFlyer: modtools 0x00A168C0, Steam 0x00402AD0).
constexpr uint32_t kSoldierRtti = pbl_hash("EntitySoldier");
constexpr uint32_t kFlyerRtti   = pbl_hash("EntityFlyer");
constexpr uint32_t kWalkerRtti  = pbl_hash("EntityWalker");   // CommandWalker answers it too
constexpr uint32_t kHoverRtti   = pbl_hash("EntityHover");    // CommandHover answers it too
static_assert(kSoldierRtti == 0x5E8739F4u, "the target bar's soldier RTTI hash");

// SoldierState (PDB enum), the values used here.
constexpr int kSoldierSprint = 3;
constexpr int kSoldierRoll   = 5;

bool soldier_airborne(int s) { return s == 4 || s == 6 || s == 7 || s == 8; }   // JUMP, JET_JUMP, JET_HOVER, FALL
bool soldier_grounded(int s)                                                     // STAND, CROUCH, PRONE, SPRINT, ROLL, SLIDE
{
   return s == 0 || s == 1 || s == 2 || s == 3 || s == 5 || s == 19;
}

// mTrick is set to this around DoTrick, to tell "no trick attempted" apart.
constexpr float kTrickUnset = -2.0f;
constexpr float kTrickRefused = -1.0f;

// For this long after a bump's shake, a drop in health is the bump's own
// damage and does not shake again as a hit.
constexpr float kBumpHitMute = 0.25f;

using SetupCameraFn   = void(__fastcall*)(uint8_t* self, void* edx, void* camera, float dt);
using SetMatrixFn     = void(__thiscall*)(void* camera, const void* matrix);
using IsFirstPersonFn = bool(__thiscall*)(void* tracker);
using DoTrickFn       = void(__fastcall*)(uint8_t* self, void* edx, int trick);
using SignalFireFn    = void(__fastcall*)(uint8_t* weapon, void* edx);
using ReticleUpdateFn = bool(__fastcall*)(void* self, void* edx, float dt);
using ApplyShakeFn    = void(__fastcall*)(void* manager, void* edx, float amount, float duration);
using MeleeUpdateFn   = bool(__fastcall*)(uint8_t* self, void* edx, float dt);
using MeleeDeflectFn  = bool(__fastcall*)(uint8_t* self, void* edx, void* ordnance, const void* pos,
                                          const void* dir);
using HoverCollisionFn = bool(__fastcall*)(uint8_t* self, void* edx, void* result, uint8_t* other,
                                           void* restrictor);
using ObjectFn        = uint8_t*(__thiscall*)(void* self);
using IsRttiFn        = bool(__thiscall*)(void* self, uint32_t hash);
using IsMeleeFn       = bool(__thiscall*)(void* weapon);

SetupCameraFn   s_setupCamera   = nullptr;
SetMatrixFn     s_setMatrix     = nullptr;
IsFirstPersonFn s_isFirstPerson = nullptr;
DoTrickFn       s_doTrick       = nullptr;
SignalFireFn    s_signalFire    = nullptr;
ReticleUpdateFn s_reticleUpdate = nullptr;
ApplyShakeFn    s_applyShake    = nullptr;
MeleeUpdateFn   s_meleeUpdate   = nullptr;
MeleeDeflectFn  s_meleeDeflect  = nullptr;
uint32_t        s_meleeHits     = layout::WeaponMelee::kDamageDataModtools;
HoverCollisionFn s_hoverCollision = nullptr;
bool            s_hoverHooked   = false;   // the hover's collision callback is detoured

template<class T> T& at(uint8_t* p, uint32_t offset) { return *reinterpret_cast<T*>(p + offset); }
template<class T> T& at(void* p, uint32_t offset) { return at<T>(static_cast<uint8_t*>(p), offset); }

uint8_t* vcall_object(void* self, uint32_t slot)
{
   return reinterpret_cast<ObjectFn>((*static_cast<void***>(self))[slot / 4])(self);
}

bool is_rtti(uint8_t* obj, uint32_t hash)
{
   return reinterpret_cast<IsRttiFn>((*reinterpret_cast<void***>(obj))[kVt_IsRtti / 4])(obj, hash);
}

bool is_melee(uint8_t* weapon)
{
   return reinterpret_cast<IsMeleeFn>((*reinterpret_cast<void***>(weapon))[layout::Weapon::kVt_IsMelee / 4])(weapon);
}

// -----------------------------------------------------------------------------
// ODF properties: twenty-two shakes, each a scale and twelve details, kept per class
// and inherited through the reader's Derive
// -----------------------------------------------------------------------------

enum Field : uint8_t {
   kScale, kPitch, kYaw, kRollAxis, kPush, kLength, kRise, kRate, kLimit, kThreshold, kSteady, kPushOnce,
   kTeammates,
   kFieldCount
};

constexpr const char* kShakeNames[kCameraShakeChannels] = {
   "Fire", "Hit", "Land", "Roll", "Sprint", "Brake", "Blast",
   "Boost", "Turn", "Collision", "TrickRoll", "TrickFlip", "Takeoff", "Landing",
   "Swing", "Strike", "Block", "Deflect", "SwingBlocked", "Step", "Jump", "Move",
};
constexpr const char* kFieldSuffixes[kFieldCount] = {
   "Shake", "ShakePitch", "ShakeYaw", "ShakeRoll", "ShakePush", "ShakeLength", "ShakeRise", "ShakeRate",
   "ShakeLimit", "ShakeThreshold", "ShakeSteady", "ShakePushOnce", "ShakeTeammates",
};

struct PropName {
   uint32_t hash;
   uint8_t  shake;
   uint8_t  field;
};

struct PropTable {
   PropName e[kCameraShakeChannels * kFieldCount];
};

constexpr PropTable make_prop_table()
{
   PropTable t{};
   int n = 0;
   for (int s = 0; s < kCameraShakeChannels; ++s)
      for (int f = 0; f < kFieldCount; ++f)
         t.e[n++] = { pbl_hash_append(pbl_hash(kShakeNames[s]), kFieldSuffixes[f]),
                      static_cast<uint8_t>(s), static_cast<uint8_t>(f) };
   return t;
}

constexpr PropTable kProps = make_prop_table();
static_assert(kProps.e[0].hash == pbl_hash("FireShake"), "name table order");
static_assert(kProps.e[kShakeRoll * kFieldCount + kRollAxis].hash == pbl_hash("RollShakeRoll"), "name table order");
static_assert(kProps.e[kShakeFire * kFieldCount + kLimit].hash == pbl_hash("FireShakeLimit"), "name table order");
static_assert(kProps.e[kShakeBoost * kFieldCount + kThreshold].hash == pbl_hash("BoostShakeThreshold"),
              "name table order");
static_assert(kProps.e[kShakeLanding * kFieldCount + kSteady].hash == pbl_hash("LandingShakeSteady"),
              "name table order");
static_assert(kProps.e[kShakeBlast * kFieldCount + kPushOnce].hash == pbl_hash("BlastShakePushOnce"),
              "name table order");
static_assert(kProps.e[kShakeStrike * kFieldCount + kTeammates].hash == pbl_hash("StrikeShakeTeammates"),
              "name table order");
static_assert(kProps.e[kShakeSwingBlocked * kFieldCount + kScale].hash == pbl_hash("SwingBlockedShake"),
              "name table order");
static_assert(kProps.e[kShakeStep * kFieldCount + kThreshold].hash == pbl_hash("StepShakeThreshold"),
              "name table order");
static_assert(kProps.e[kShakeJump * kFieldCount + kScale].hash == pbl_hash("JumpShake"), "name table order");
static_assert(kProps.e[kShakeMove * kFieldCount + kThreshold].hash == pbl_hash("MoveShakeThreshold"),
              "name table order");

// Every shake's name in its enum's place, so one added out of order fails here.
struct ShakeName {
   int         id;
   const char* name;
};
constexpr ShakeName kNameOrder[] = {
   { kShakeFire, "FireShake" },           { kShakeHit, "HitShake" },
   { kShakeLand, "LandShake" },           { kShakeRoll, "RollShake" },
   { kShakeSprint, "SprintShake" },       { kShakeBrake, "BrakeShake" },
   { kShakeBlast, "BlastShake" },         { kShakeBoost, "BoostShake" },
   { kShakeTurn, "TurnShake" },           { kShakeCollision, "CollisionShake" },
   { kShakeTrickRoll, "TrickRollShake" }, { kShakeTrickFlip, "TrickFlipShake" },
   { kShakeTakeoff, "TakeoffShake" },     { kShakeLanding, "LandingShake" },
   { kShakeSwing, "SwingShake" },         { kShakeStrike, "StrikeShake" },
   { kShakeBlock, "BlockShake" },         { kShakeDeflect, "DeflectShake" },
   { kShakeSwingBlocked, "SwingBlockedShake" }, { kShakeStep, "StepShake" },
   { kShakeJump, "JumpShake" },           { kShakeMove, "MoveShake" },
};
static_assert(sizeof kNameOrder / sizeof kNameOrder[0] == kCameraShakeChannels, "a shake missing from kNameOrder");

constexpr bool names_in_order()
{
   for (const ShakeName& n : kNameOrder)
      if (kProps.e[n.id * kFieldCount + kScale].hash != pbl_hash(n.name)) return false;
   return true;
}
static_assert(names_in_order(), "kShakeNames must follow CameraShakeChannel");

constexpr bool all_distinct(const PropTable& t)
{
   constexpr int n = kCameraShakeChannels * kFieldCount;
   for (int i = 0; i < n; ++i)
      for (int j = i + 1; j < n; ++j)
         if (t.e[i].hash == t.e[j].hash) return false;
   return true;
}
static_assert(all_distinct(kProps), "two shake property names share a hash");

struct ShakeProps {
   uint16_t set   = 0;      // bit per Field the class gave
   float    scale = 1.0f;
   Shape    shape;
};

struct ClassShake {
   ShakeProps shake[kCameraShakeChannels];
};

std::unordered_map<const void*, ClassShake> s_classes;

bool on_property(void* cls, uint32_t hash, const char* value)
{
   for (const PropName& p : kProps.e) {
      if (p.hash != hash) continue;
      ShakeProps& s = s_classes[cls].shake[p.shake];
      bool ok = false;
      switch (p.field) {
      case kScale:    ok = parse_amount(value, s.scale);        break;
      case kPitch:    ok = parse_range(value, s.shape.pitch);   break;
      case kYaw:      ok = parse_range(value, s.shape.yaw);     break;
      case kRollAxis: ok = parse_range(value, s.shape.roll);    break;
      case kPush:     ok = parse_range(value, s.shape.push);    break;
      case kLength:   ok = parse_amount(value, s.shape.length); break;
      case kRise:
         ok = parse_amount(value, s.shape.rise);
         s.shape.rise = clamp01(s.shape.rise);
         break;
      case kRate:     ok = parse_amount(value, s.shape.rate);   break;
      case kLimit:    ok = parse_amount(value, s.shape.limit);  break;
      case kThreshold: ok = parse_threshold(value, s.shape.threshold); break;
      case kSteady:
         ok = parse_amount(value, s.shape.steady);
         s.shape.steady = clamp01(s.shape.steady);
         break;
      case kPushOnce: {
         float on = 0.0f;
         ok = parse_amount(value, on);
         if (ok) s.shape.pushOnce = on > 0.0f;
         break;
      }
      case kTeammates: {
         float on = 0.0f;
         ok = parse_amount(value, on);
         if (ok) s.shape.teammates = on > 0.0f;
         break;
      }
      default: break;
      }
      if (ok) s.set = static_cast<uint16_t>(s.set | (1u << p.field));
      return true;   // no stock SetProperty knows these names
   }
   return false;
}

// A child class starts as a copy of its parent. This also replaces whatever an
// earlier mission left at a reused class address.
void on_derive(const void* parent, void* child)
{
   if (s_classes.empty()) return;
   const auto it = s_classes.find(parent);
   if (it == s_classes.end()) {
      s_classes.erase(child);
      return;
   }
   const ClassShake inherited = it->second;   // copy before the insert can rehash
   s_classes[child] = inherited;
}

const ClassShake* find_class(const void* cls)
{
   if (!cls || s_classes.empty()) return nullptr;
   const auto it = s_classes.find(cls);
   return it == s_classes.end() ? nullptr : &it->second;
}

// A class's settings for one shake, over the defaults. False when the shake is
// off: the class sets none of it and it is opt-in, or its scale is 0.
bool shape_for(const ClassShake* cs, int id, const Shape& base, bool onByDefault, Shape& out, float& scale)
{
   out = base;
   scale = 1.0f;
   const ShakeProps* p = cs ? &cs->shake[id] : nullptr;
   if (!p || !p->set) return onByDefault;
   const auto has = [p](Field f) { return (p->set & (1u << f)) != 0; };
   if (has(kScale))    scale = p->scale;
   if (has(kPitch))    out.pitch = p->shape.pitch;
   if (has(kYaw))      out.yaw = p->shape.yaw;
   if (has(kRollAxis)) out.roll = p->shape.roll;
   if (has(kPush))     out.push = p->shape.push;
   if (has(kLength))   out.length = p->shape.length;
   if (has(kRise))     out.rise = p->shape.rise;
   if (has(kRate))     out.rate = p->shape.rate;
   if (has(kLimit))    out.limit = p->shape.limit;
   if (has(kThreshold)) out.threshold = p->shape.threshold;
   if (has(kSteady))   out.steady = p->shape.steady;
   if (has(kPushOnce)) out.pushOnce = p->shape.pushOnce;
   if (has(kTeammates)) out.teammates = p->shape.teammates;
   return scale > 0.0f;
}

// -----------------------------------------------------------------------------
// The view: what the chase camera follows, and the shakes running on it
// -----------------------------------------------------------------------------

struct View {
   uint8_t* obj          = nullptr;   // GameObject the chase camera follows
   int      soldierState = -1;
   int      flyerState   = -1;
   float    prevSpeed    = -1.0f;     // a flyer's speed last frame, m/s
   float    prevForward  = 0.0f;      // and its speed along its nose
   float    prevVelocity[3] = {};
   bool     haveVelocity = false;
   float    prevNose[3]  = {};        // a flyer's forward axis last frame
   bool     haveNose     = false;
   Hold     noseRate;                 // how fast its nose swings round, smoothed
   TurnCount turn;                    // seconds of hard turning
   SpeedUp  speedUp;                  // a speed-up the throttle or a boost asked for
   float    hitMute      = 0.0f;      // seconds left of a bump's damage (kBumpHitMute)
   bool     takesBumps   = false;     // its class sets CollisionShake
   uint32_t walkerFeet   = 0;         // a walker's feet that had landed, last look
   bool     walkerJumping = false;    // and whether it was jumping
   bool     haveWalker   = false;     // those two hold a reading
   WalkerAir walkerAir;               // its time off the ground
   bool     takesHoverHits = false;   // a hover whose class sets CollisionShake
   bool     hoverJumping = false;     // a hover jumping, last look
   bool     haveHover    = false;     // that holds a reading
   HoverAir hoverAir;                 // its time off the ground
   BumpGate hoverBumps;               // its bumps that have just played
   AirTime  air;
   HitSense hit;
};

// A bump BF2's collision code reported for the viewed flyer, waiting for the
// next camera frame, with the largest stock shake amount it came with.
struct PendingBump {
   bool  pending = false;
   float amount  = 0.0f;
};

// A hit the viewed hover's collision callback reported, waiting for the next
// camera frame: the hardest since the last, with where it came from.
struct PendingHoverHit {
   bool     pending = false;
   HoverHit hit;
};

View        s_view;
PendingBump s_bump;
PendingHoverHit s_hoverHit;
KickChannel s_fireKick, s_hitKick, s_landKick, s_rollKick;
KickChannel s_trickRollKick, s_trickFlipKick, s_takeoffKick, s_landingKick, s_bumpKick;
KickChannel s_swingKick, s_strikeKick, s_blockKick, s_deflectKick, s_swingBlockedKick;
KickChannel s_stepKick, s_jumpKick;
float       s_sprintLevel = 0.0f;
Shape       s_sprintShape = defaults::kSprintSoldier;
Hold        s_boost, s_turn, s_brake, s_move, s_decel, s_accel;
Shape       s_boostShape = defaults::kBoost;
Shape       s_turnShape  = defaults::kTurn;
Shape       s_brakeShape = defaults::kBrake;
Shape       s_moveShape  = defaults::kMoveHover;
Rng         s_rng;
double      s_time = 0.0;   // seconds of shake time, double: see noise()

// The render camera's world-to-camera matrix without and with this frame's
// shake, for the reticule (hooked_ReticuleUpdate).
struct ReticleView {
   uint8_t* camera = nullptr;
   float    unshaken[16] = {};
   float    shaken[16]   = {};
   bool     valid        = false;
};
ReticleView s_reticle;

// The shake left in the chase camera's mMatrix, to hand BF2 its own matrix
// back before the next SetupCamera (LeftShake).
LeftShake s_left;

void play(KickChannel& channel, const ClassShake* cs, int id, const Shape& base, float extra = 1.0f)
{
   Shape shape;
   float scale;
   if (shape_for(cs, id, base, false, shape, scale))
      channel.trigger(make_kick(shape, scale * extra, s_rng), s_time, shape.limit);
}

bool first_person(uint8_t* owner)
{
   void* tracker = at<void*>(owner, cam::kTrackableTracker);
   return tracker && s_isFirstPerson(tracker);
}

// Landings in any view; rolls and sprinting in third person only, since first
// person has camera motion of its own for both.
void update_soldier(uint8_t* owner, const ClassShake* cs, float dt, bool firstPerson, float& sprintTarget)
{
   uint8_t* ctrl = vcall_object(owner, kVt_GetControllable);
   const int state = ctrl ? at<int>(ctrl, g_soldier->mState) : -1;
   const int prev = s_view.soldierState;
   s_view.soldierState = state;

   if (s_view.air.update(soldier_airborne(state), soldier_grounded(state), dt))
      play(s_landKick, cs, kShakeLand, defaults::kLandSoldier);

   if (firstPerson) return;
   if (state == kSoldierRoll && prev != kSoldierRoll && prev != -1)
      play(s_rollKick, cs, kShakeRoll, defaults::kRollSoldier);
   Shape shape;
   float scale;
   if (state == kSoldierSprint && shape_for(cs, kShakeSprint, defaults::kSprintSoldier, false, shape, scale)) {
      s_sprintShape = shape;
      sprintTarget = scale;
   }
}

// A flyer class's MinSpeed, MidSpeed, MaxSpeed and BoostSpeed, where this
// build's layout has them.
FlyerSpeeds class_speeds(void* cls)
{
   FlyerSpeeds s;
   if (!cls || !fly_class::mMinSpeed.off() || !fly_class::mMidSpeed.off() || !fly_class::mMaxSpeed.off() ||
       !fly_class::mBoostSpeed.off())
      return s;
   s.min = fly_class::mMinSpeed(cls);
   s.mid = fly_class::mMidSpeed(cls);
   s.max = fly_class::mMaxSpeed(cls);
   s.boost = fly_class::mBoostSpeed(cls);
   s.known = std::isfinite(s.min) && std::isfinite(s.mid) && std::isfinite(s.max) && std::isfinite(s.boost);
   return s;
}

// A bump: CollisionShake sized by how hard it was, as a kick on the flyer's
// turbulence.
void play_bump(const ClassShake* cs, const FlyerSpeeds& speeds, float impact)
{
   Shape shape;
   float scale;
   if (!shape_for(cs, kShakeCollision, defaults::kCollision, false, shape, scale)) return;
   const float size = bump_scale(shape.threshold, speeds, impact);
   if (!(size > 0.0f)) return;
   s_bumpKick.trigger(make_kick(shape, scale * size, s_rng, true), s_time, shape.limit);
   s_view.hitMute = kBumpHitMute;
}

// What the shakes that last are heading for this frame: a flyer's boost, turn
// and brake, a walker's boost and turn, or a hover's movement and boost.
struct HeldTargets {
   float boost = 0.0f;
   float turn  = 0.0f;
   float brake = 0.0f;
   float move  = 0.0f;
};

// On a flyer: the boost and turn turbulence, the brake sway, bumps, take-off
// and landing. Tricks come from the DoTrick detour below.
void update_flyer(uint8_t* obj, const ClassShake* cs, float dt, HeldTargets& out)
{
   const float* v = fly::mVelocity(obj);
   const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
   // Its speed along its nose, which is what BF2 and the class's speeds mean by
   // speed; plain speed where this build's field is not read yet.
   const float forward = fly::mGetSpeedSpeed.off() ? fly::mGetSpeedSpeed(obj) : speed;
   const FlyerSpeeds speeds = class_speeds(fly::mClass(obj));

   // A bump is sized by how much it changed the velocity since the last frame,
   // or failing that by BF2's own figure. Its frame is left out of speeding up
   // and braking, which would read it as a sudden stop.
   const bool bumped = s_bump.pending;
   if (bumped) {
      float impact = stock_bump_speed(s_bump.amount);
      if (s_view.haveVelocity) {
         const float dx = v[0] - s_view.prevVelocity[0];
         const float dy = v[1] - s_view.prevVelocity[1];
         const float dz = v[2] - s_view.prevVelocity[2];
         impact = std::sqrt(dx * dx + dy * dy + dz * dz);
      }
      s_bump = PendingBump{};
      play_bump(cs, speeds, impact);
   }
   std::memcpy(s_view.prevVelocity, v, sizeof s_view.prevVelocity);
   s_view.haveVelocity = true;

   // How fast the nose swings round, every frame so a take-off starts from
   // where it points.
   if (fly::mMatrix_forward.off()) {
      const float* nose = fly::mMatrix_forward(obj);
      const float rate = s_view.haveNose ? nose_turn_rate(s_view.prevNose, nose, dt) : 0.0f;
      std::memcpy(s_view.prevNose, nose, sizeof s_view.prevNose);
      s_view.haveNose = true;
      s_view.noseRate.advance(rate, dt, 0.05f, 0.1f);
   }

   if (dt > 0.0f) {
      const bool measured = s_view.prevSpeed >= 0.0f && !bumped;
      const float decel = measured ? (s_view.prevSpeed - speed) / dt : 0.0f;
      const float accel = measured ? (forward - s_view.prevForward) / dt : 0.0f;
      s_decel.advance(decel > 0.0f ? decel : 0.0f, dt, 0.05f, 0.15f);
      s_accel.advance(accel > 0.0f ? accel : 0.0f, dt, 0.05f, 0.15f);
      s_view.prevSpeed = speed;
      s_view.prevForward = forward;
   }

   const int state = fly::mState(obj);
   const int prev = s_view.flyerState;
   s_view.flyerState = state;
   if (state == fly::kTakeoff && prev == fly::kLanded)
      play(s_takeoffKick, cs, kShakeTakeoff, defaults::kTakeoff);
   if (state == fly::kLanded &&
       (prev == fly::kLanding || prev == fly::kFlying))
      play(s_landingKick, cs, kShakeLanding, defaults::kLanding);
   if (state != fly::kFlying) {
      s_view.turn.reset();
      s_view.speedUp.reset();
      return;
   }

   // Speeding up or slowing down: toward the speed the throttle (or a boost)
   // is taking it to, from well short of it, so steering is not mistaken for
   // either. Speeding up counts only once the throttle as held, or a boost,
   // has raised that speed: BF2 lowers its own while the flyer rolls
   // (throttle_intent), and getting back what a roll or a turn cost is not a
   // boost (SpeedUp). Where the throttle is not read yet, how fast the speed
   // changes.
   // Braking is slowing down with the brake held: easing back to cruise after
   // the throttle is let go does not count.
   bool speedingUp, slowingDown, braking;
   if (fly::mControlMove.off() && fly::mInLandingRegionFactor.off() && speeds.known) {
      const float move = fly::mControlMove(obj);
      const float roll = fly::mControlStrafe.off() ? fly::mControlStrafe(obj) : 0.0f;
      const bool boosting = (fly::mFlags(obj) & fly::kFlagBoost) != 0;
      const bool landing = fly::mInLandingRegionFactor(obj) != 0.0f;
      const float target = throttle_target(speeds, move, boosting, landing);
      const float asked = throttle_target(speeds, throttle_intent(move, roll), boosting, landing);
      const float margin = heading_margin(speeds);
      speedingUp = s_view.speedUp.update(asked, target, forward, margin);
      slowingDown = forward - target > margin;
      braking = slowingDown && move <= -kBrakeInput;
   } else {
      speedingUp = s_accel.level > kHoldingSpeed;
      slowingDown = s_decel.level > kHoldingSpeed;
      braking = slowingDown;
   }

   Shape shape;
   float scale;
   if (shape_for(cs, kShakeBoost, defaults::kBoost, false, shape, scale)) {
      // Heading for the threshold's far end: speeding up, or for a pair
      // written high to low, slowing down.
      const bool heading = threshold_rising(shape.threshold, speeds) ? speedingUp : slowingDown;
      s_boostShape = shape;
      out.boost = scale * boost_level(threshold_level(shape.threshold, speeds, forward), heading, shape.steady);
   }
   if (fly::mMatrix_forward.off() && fly_class::mPitchRate.off() && fly_class::mTurnRate.off()) {
      // Turning hard: the nose swinging round near what the class can turn,
      // whether by stick or mouse. A trick's flip is not a turn.
      const EntityFlyerClass* cls = fly::mClass(obj);
      const float hard = cls ? hard_turn_rate(fly_class::mPitchRate(cls), fly_class::mTurnRate(cls))
                             : hard_turn_rate(0.0f, 0.0f);
      const bool tricking = (fly::mFlags(obj) & (fly::kFlagRoll | fly::kFlagFlip)) != 0;
      s_view.turn.update(!tricking && s_view.noseRate.level >= hard, dt);
      if (shape_for(cs, kShakeTurn, defaults::kTurn, false, shape, scale)) {
         // Harder the faster it goes, up to the class's MaxSpeed.
         const float pace = speeds.known && speeds.max > 0.0f ? clamp01(forward / speeds.max) : 1.0f;
         s_turnShape = shape;
         out.turn = scale * pace * threshold_level(shape.threshold, speeds, s_view.turn.time);
      }
   }
   if (shape_for(cs, kShakeBrake, defaults::kBrake, false, shape, scale)) {
      s_brakeShape = shape;
      out.brake = scale * boost_level(brake_position(shape.threshold, speeds, forward), braking, shape.steady);
   }
}

// A step: StepShake for the feet that landed this update, its roll turned
// toward their side and its size graded by the walker's speed where the class
// gives a Threshold, the way a bump is.
void play_step(const ClassShake* cs, const FlyerSpeeds& speeds, float speed, uint32_t landed)
{
   Shape shape;
   float scale;
   if (!shape_for(cs, kShakeStep, defaults::kStep, false, shape, scale)) return;
   const float size = bump_scale(shape.threshold, speeds, speed);
   if (!(size > 0.0f)) return;
   Kick k = make_kick(shape, scale * size, s_rng);
   int side = step_side(landed);
   if (side == 0) side = s_rng.unit() < 0.5f ? -1 : 1;
   k.peak.roll = std::fabs(k.peak.roll) * static_cast<float>(side);
   s_stepKick.trigger(k, s_time, shape.limit);
}

// On a walker: a step for each foot that lands, a jump, a landing, and the
// sways while it turns on the spot or boosts. Steps, jumps and landings are
// read off BF2's own records, so they come at the moment its stomp effect,
// footstep sound and jump play.
void update_walker(uint8_t* obj, const ClassShake* cs, HeldTargets& out)
{
   const EntityWalkerClass* cls = walk::mClass(obj);
   const FlyerSpeeds speeds = cls ? walker_speeds(walk_class::mMaxSpeed(cls), walk_class::mBoostSpeed(cls))
                                  : FlyerSpeeds{};
   const float* v = walk::mVelocity(obj);
   const float speed = std::sqrt(v[0] * v[0] + v[2] * v[2]);   // along the ground
   const int state = walk::mState(obj);
   const bool jumping = (walk::mFlags(obj) & walk::kFlagJumping) != 0;
   const int feet = cls ? walk_class::mNumFeet(cls) : 0;
   const uint32_t down = walk::mFootState(obj) & feet_mask(feet);
   const float came = s_view.walkerAir.update(walk::m_fGroundedTimer(obj), v[1]);

   const bool first = !s_view.haveWalker;
   const uint32_t landed = first ? 0 : feet_landed(s_view.walkerFeet, down);
   const bool jumped = !first && jumping && !s_view.walkerJumping;
   s_view.walkerFeet = down;
   s_view.walkerJumping = jumping;
   s_view.haveWalker = true;
   if (state == walk::kStateDying || state == walk::kStateDead) return;

   if (landed) play_step(cs, speeds, speed, landed);
   if (jumped) play(s_jumpKick, cs, kShakeJump, defaults::kJump);
   Shape shape;
   float scale;
   if (came >= 0.0f && shape_for(cs, kShakeLand, defaults::kLandWalker, false, shape, scale)) {
      const float size = bump_scale(shape.threshold, speeds, came);
      if (size > 0.0f) s_landKick.trigger(make_kick(shape, scale * size, s_rng), s_time, shape.limit);
   }

   if ((state == walk::kStateTurnLeft || state == walk::kStateTurnRight) &&
       shape_for(cs, kShakeTurn, defaults::kTurnWalker, false, shape, scale)) {
      s_turnShape = shape;
      out.turn = scale;
   }
   if ((walk::mBoost(obj) & walk::kBoosting) &&
       shape_for(cs, kShakeBoost, defaults::kBoostWalker, false, shape, scale)) {
      // Full while it is still speeding up to BoostSpeed, then Steady of that.
      const bool heading = speeds.boost - speed > heading_margin(speeds);
      s_boostShape = shape;
      out.boost = scale * boost_level(threshold_level(shape.threshold, speeds, speed), heading, shape.steady);
   }
}

// A hover's bump: CollisionShake sized by how hard it hit, tilted toward the
// hit. A contact that keeps reporting (a spring sinking into a wall) plays
// once (BumpGate).
void play_hover_bump(const ClassShake* cs, const FlyerSpeeds& speeds, const HoverHit& hit)
{
   Shape shape;
   float scale;
   if (!shape_for(cs, kShakeCollision, defaults::kCollisionHover, false, shape, scale)) return;
   const float size = bump_scale(shape.threshold, speeds, hit.speed);
   if (!(size > 0.0f) || !s_view.hoverBumps.admit(hit.speed)) return;
   Kick k = make_kick(shape, scale * size, s_rng);
   aim_bump(k, hit.right, hit.forward);
   s_bumpKick.trigger(k, s_time, shape.limit);
   s_view.hitMute = kBumpHitMute;
}

// On a hover: a sway while it moves and a stronger one while it boosts, both
// fading out in the air; a jump; a landing; and a tilt toward whatever it
// hits. The jump, the landing and the hits come from BF2's own records: the
// jump flag, the ground ratio and the collision callback.
void update_hover(uint8_t* obj, const ClassShake* cs, float dt, HeldTargets& out)
{
   const EntityHoverClass* cls = hov::mClass(obj);
   const FlyerSpeeds speeds = cls ? hover_speeds(hov_class::mForwardSpeed(cls), hov_class::mBoostSpeed(cls))
                                  : FlyerSpeeds{};
   const float* v = hov::mVelocity(obj);
   const float speed = plane_speed(v, hov::mMatrix_up(obj));   // along its deck: its bob is not speed
   const float ratio = hov::mGroundRatio(obj);
   const float ground = std::isfinite(ratio) ? clamp01(ratio) : 0.0f;
   const bool jumping = (hov::mFlags(obj) & hov::kFlagJumping) != 0;
   const float came = s_view.hoverAir.update(ratio, v[1], dt);

   const bool jumped = s_view.haveHover && jumping && !s_view.hoverJumping;
   s_view.hoverJumping = jumping;
   s_view.haveHover = true;
   s_view.hoverBumps.advance(dt);

   if (s_hoverHit.pending) {
      const HoverHit hit = s_hoverHit.hit;
      s_hoverHit = PendingHoverHit{};
      play_hover_bump(cs, speeds, hit);
   }
   if (jumped) play(s_jumpKick, cs, kShakeJump, defaults::kJump);
   Shape shape;
   float scale;
   if (came >= 0.0f && shape_for(cs, kShakeLand, defaults::kLandHover, false, shape, scale)) {
      const float size = bump_scale(shape.threshold, speeds, came);
      if (size > 0.0f) s_landKick.trigger(make_kick(shape, scale * size, s_rng), s_time, shape.limit);
   }

   if (shape_for(cs, kShakeMove, defaults::kMoveHover, false, shape, scale)) {
      s_moveShape = shape;
      out.move = scale * threshold_level(shape.threshold, speeds, speed) * ground;
   }
   if (hov::mBoost(obj) && shape_for(cs, kShakeBoost, defaults::kBoostHover, false, shape, scale)) {
      // Full while it is still speeding up to BoostSpeed, then Steady of that.
      const bool heading = speeds.boost - speed > heading_margin(speeds);
      s_boostShape = shape;
      out.boost = scale * boost_level(threshold_level(shape.threshold, speeds, speed), heading, shape.steady) *
                  ground;
   }
}

// Any unit: a drop in its health is a hit, sized by how much it lost. Right
// after a bump's shake the drop is the bump's own damage, already shaken for.
void update_hit(uint8_t* obj, const ClassShake* cs, float dt)
{
   const float maxHealth = at<float>(obj, kObj_MaxHealth);
   const float hit = s_view.hit.update(at<float>(obj, kObj_Health), hit_threshold(maxHealth), dt);
   if (s_view.hitMute > 0.0f) {
      s_view.hitMute -= dt;
      return;
   }
   if (hit > 0.0f) play(s_hitKick, cs, kShakeHit, defaults::kHit, hit_scale(hit, maxHealth));
}

// The stock queue drawn as a blast: the strongest running shake, not their sum,
// shrunk while the view is zoomed in. On by default; a class can reshape it or
// turn it off with BlastShake.
Offset blast_offset(uint8_t* self, void* camera, const ClassShake* cs)
{
   int n = at<int>(self, cam::kShakeCount);
   if (n < 0) n = 0;
   if (n > cam::kShakeSlots) n = cam::kShakeSlots;
   float strongest = 0.0f;
   for (int i = 0; i < n; ++i) strongest = std::fmax(strongest, at<float>(self, cam::kShakeAmount + 4 * i));
   if (!(strongest > 0.0f)) return {};

   Shape shape;
   float scale;
   if (!shape_for(cs, kShakeBlast, defaults::kBlast, true, shape, scale)) return {};
   const float amount = blast_amount(strongest, shape.limit);
   const float zoom = at<float>(camera, layout::RedCamera::kZoom);
   const float zoomScale = std::isfinite(zoom) && zoom > 1.0f ? 1.0f / zoom : 1.0f;
   return blast(shape, s_time, amount * scale * zoomScale);
}

void shake_view(uint8_t* self, void* camera, float dt)
{
   // SetupCamera has just set the camera from the unshaken matrix: keep the
   // inverse it made, for the reticule.
   s_reticle.valid = false;
   std::memcpy(s_reticle.unshaken, static_cast<uint8_t*>(camera) + layout::RedCamera::kMatrixInverse,
               sizeof s_reticle.unshaken);

   uint8_t* owner = at<uint8_t*>(self, cam::kOwner);
   if (!owner) {
      s_view.obj = nullptr;
      return;
   }

   if (!(dt > 0.0f)) dt = 0.0f;         // paused, or not a number
   else if (dt > 0.1f) dt = 0.1f;       // a hitch must not swallow a whole kick
   s_time += dt;

   uint8_t* obj = vcall_object(owner, kVt_GetGameObject);
   if (obj != s_view.obj) {
      s_view = View{};
      s_view.obj = obj;
      s_bump = PendingBump{};
      s_hoverHit = PendingHoverHit{};
   }

   const bool flyer = obj && is_rtti(obj, kFlyerRtti);
   const bool walker = obj && !flyer && is_rtti(obj, kWalkerRtti);
   const bool hover = obj && !flyer && !walker && is_rtti(obj, kHoverRtti);
   const ClassShake* cs = nullptr;
   if (obj && !s_classes.empty())
      cs = find_class(flyer    ? static_cast<const void*>(fly::mClass(obj))
                      : walker ? static_cast<const void*>(walk::mClass(obj))
                      : hover  ? static_cast<const void*>(hov::mClass(obj))
                               : static_cast<const void*>(vcall_object(obj, kVt_GetEntityClass)));
   const bool firstPerson = first_person(owner);

   float sprintTarget = 0.0f;
   HeldTargets held;
   s_view.takesBumps = false;
   s_view.takesHoverHits = false;
   if (s_odfShakes && obj) {
      if (flyer) {
         Shape bump;
         float scale;
         s_view.takesBumps = shape_for(cs, kShakeCollision, defaults::kCollision, false, bump, scale);
         update_flyer(obj, cs, dt, held);
      } else if (walker) {
         update_walker(obj, cs, held);
      } else if (hover) {
         Shape bump;
         float scale;
         s_view.takesHoverHits =
            s_hoverHooked && shape_for(cs, kShakeCollision, defaults::kCollisionHover, false, bump, scale);
         update_hover(obj, cs, dt, held);
      } else if (is_rtti(obj, kSoldierRtti)) {
         update_soldier(owner, cs, dt, firstPerson, sprintTarget);
      }
      update_hit(obj, cs, dt);
   }

   s_fireKick.advance(dt);
   s_hitKick.advance(dt);
   s_landKick.advance(dt);
   s_rollKick.advance(dt);
   s_trickRollKick.advance(dt);
   s_trickFlipKick.advance(dt);
   s_takeoffKick.advance(dt);
   s_landingKick.advance(dt);
   s_bumpKick.advance(dt);
   s_swingKick.advance(dt);
   s_strikeKick.advance(dt);
   s_blockKick.advance(dt);
   s_deflectKick.advance(dt);
   s_swingBlockedKick.advance(dt);
   s_stepKick.advance(dt);
   s_jumpKick.advance(dt);
   // The shakes that last follow their target at the fade their Length and
   // Rise give them.
   s_sprintLevel = ramp_toward(s_sprintLevel, sprintTarget, dt, held_fade(s_sprintShape));
   fade_toward(s_boost, held.boost, dt, held_fade(s_boostShape));
   fade_toward(s_turn, held.turn, dt, held_fade(s_turnShape));
   fade_toward(s_brake, held.brake, dt, held_fade(s_brakeShape));
   fade_toward(s_move, held.move, dt, held_fade(s_moveShape));

   Offset total;
   total += s_fireKick.value(s_time);
   total += s_hitKick.value(s_time);
   total += s_landKick.value(s_time);
   total += s_rollKick.value(s_time);
   total += s_trickRollKick.value(s_time);
   total += s_trickFlipKick.value(s_time);
   total += s_takeoffKick.value(s_time);
   total += s_landingKick.value(s_time);
   total += s_bumpKick.value(s_time);
   total += s_swingKick.value(s_time);
   total += s_strikeKick.value(s_time);
   total += s_blockKick.value(s_time);
   total += s_deflectKick.value(s_time);
   total += s_swingBlockedKick.value(s_time);
   total += s_stepKick.value(s_time);
   total += s_jumpKick.value(s_time);
   total += judder(s_sprintShape, s_time, s_sprintLevel);
   total += turbulence(s_moveShape, s_time, s_move.level);
   total += turbulence(s_boostShape, s_time, s_boost.level);
   total += turbulence(s_turnShape, s_time, s_turn.level);
   total += sway(s_brakeShape, s_time, s_brake.level);
   total += blast_offset(self, camera, s_odfShakes ? cs : nullptr);
   // In first person, cockpits included, a shake turns the view but never
   // moves it: the camera is at the eye, and the arms and cockpit drawn from
   // mMatrix would move with it.
   if (firstPerson) total = turn_only(total);
   total = finite_offset(total);
   if (total.negligible()) return;

   float* m = &at<float>(self, cam::kMatrix);   // rows right, up, back, position
   std::memcpy(self + cam::kPreShakeMatrix, m, 16 * sizeof(float));
   apply(m, total);
   s_left.leave(self, &at<float>(self, cam::kPreShakeMatrix), m);
   s_setMatrix(camera, m);

   s_reticle.camera = static_cast<uint8_t*>(camera);
   std::memcpy(s_reticle.shaken, s_reticle.camera + layout::RedCamera::kMatrixInverse,
               sizeof s_reticle.shaken);
   s_reticle.valid = true;
}

// -----------------------------------------------------------------------------
// Hooks
// -----------------------------------------------------------------------------

void shake_view_guarded(uint8_t* self, void* camera, float dt)
{
   __try {
      shake_view(self, camera, dt);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      // An unreadable frame keeps the view SetupCamera made.
   }
}

// SetupCamera eases this frame's camera from last frame's mMatrix: let it start
// from the matrix it made, not the one we shook.
void put_back_unshaken(uint8_t* self)
{
   __try {
      s_left.put_back(self, &at<float>(self, cam::kMatrix));
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
}

void __fastcall hooked_SetupCamera(uint8_t* self, void* edx, void* camera, float dt)
{
   put_back_unshaken(self);
   float& suppress = at<float>(self, cam::kShakeSuppressUntil);
   const float saved = suppress;
   suppress = FLT_MAX;   // mission time is never past it: no stock turn
   s_setupCamera(self, edx, camera, dt);
   suppress = saved;
   shake_view_guarded(self, camera, dt);
}

// The GameObject a weapon's owner is, or null.
uint8_t* weapon_object(uint8_t* weapon)
{
   uint8_t* owner = at<uint8_t*>(weapon, layout::Weapon::kOwner);
   return owner ? vcall_object(owner + kCtrl_Trackable, kVt_GetGameObject) : nullptr;
}

// A weapon's shake settings: its class's, or failing that its start class's.
const ClassShake* weapon_shake(uint8_t* weapon)
{
   const ClassShake* cs = find_class(at<void*>(weapon, layout::Weapon::kClass));
   return cs ? cs : find_class(at<void*>(weapon, layout::Weapon::kStart));
}

// The viewed unit's weapons that have signalled fire this frame (ShotGate).
ShotGate s_shots;

// A shot, or on a melee weapon a swing: each attack of a combo signals fire.
// A shotgun's pellets signal once each, all at once, and kick once.
void shake_for_fire(uint8_t* weapon)
{
   __try {
      if (weapon_object(weapon) != s_view.obj) return;
      if (!s_shots.first(weapon, s_time)) return;
      const ClassShake* cs = weapon_shake(weapon);
      if (is_melee(weapon))
         play(s_swingKick, cs, kShakeSwing, defaults::kSwing);
      else
         play(s_fireKick, cs, kShakeFire, defaults::kFire);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
}

void __fastcall hooked_SignalFire(uint8_t* weapon, void* edx)
{
   s_signalFire(weapon, edx);
   if (s_view.obj && weapon && !s_classes.empty()) shake_for_fire(weapon);
}

// -----------------------------------------------------------------------------
// Melee: strikes, blocks and deflections
// -----------------------------------------------------------------------------

// The viewed unit's swing while its WeaponMelee::UpdateFire runs: what the
// swing had struck before, and what blocked it during the update.
constexpr int kMeleeBlockers = 8;

struct MeleeSwing {
   uint8_t*    weapon = nullptr;
   MeleeTally  before;
   const void* blocker[kMeleeBlockers] = {};
   int         blocked = 0;

   bool blocked_by(const void* obj) const
   {
      for (int i = 0; i < blocked; ++i)
         if (blocker[i] == obj) return true;
      return false;
   }
};

MeleeSwing s_swing;

int hit_count(uint8_t* attack)
{
   const int n = at<int>(attack, layout::WeaponMelee::kHitCount);
   return n < 0 ? 0 : (n > layout::WeaponMelee::kHitMax ? layout::WeaponMelee::kHitMax : n);
}

// Whether this update is the viewed unit's swing; if so, what it has struck.
bool melee_watch_begin(uint8_t* weapon)
{
   __try {
      s_swing.weapon = nullptr;
      if (!s_view.obj || weapon_object(weapon) != s_view.obj) return false;
      s_swing = MeleeSwing{};
      uint8_t* attack = at<uint8_t*>(weapon, s_meleeHits);
      for (int i = 0; attack && i < kMeleeAttacks; ++i) {
         s_swing.before.add(attack, hit_count(attack));
         attack = at<uint8_t*>(attack, layout::WeaponMelee::kHitNext);
      }
      s_swing.weapon = weapon;
      return true;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      s_swing.weapon = nullptr;
      return false;
   }
}

// After it: the swing landed if it struck anything during the update that did
// not block (StrikeShake; teammates count unless its Teammates is 0), and was
// blocked if anything stopped it (SwingBlockedShake).
void melee_watch_end(uint8_t* weapon)
{
   __try {
      if (s_swing.weapon != weapon) return;
      s_swing.weapon = nullptr;
      const ClassShake* cs = weapon_shake(weapon);
      if (s_swing.blocked > 0) play(s_swingBlockedKick, cs, kShakeSwingBlocked, defaults::kSwingBlocked);

      Shape shape;
      float scale;
      if (!shape_for(cs, kShakeStrike, defaults::kStrike, false, shape, scale)) return;
      const int own = team_from_bits(at<uint32_t>(s_view.obj, kObj_Team));
      bool landed = false;
      uint8_t* attack = at<uint8_t*>(weapon, s_meleeHits);
      for (int i = 0; attack && i < kMeleeAttacks && !landed; ++i) {
         const int n = hit_count(attack);
         for (int k = s_swing.before.before(attack); k < n && !landed; ++k) {
            uint8_t* obj = at<uint8_t*>(attack, layout::WeaponMelee::kHitObjects + 4 * k);
            if (!obj || s_swing.blocked_by(obj)) continue;
            if (!shape.teammates && same_team(own, team_from_bits(at<uint32_t>(obj, kObj_Team)))) continue;
            landed = true;
         }
         attack = at<uint8_t*>(attack, layout::WeaponMelee::kHitNext);
      }
      if (landed) s_strikeKick.trigger(make_kick(shape, scale, s_rng), s_time, shape.limit);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
}

bool __fastcall hooked_MeleeUpdate(uint8_t* self, void* edx, float dt)
{
   const bool watched = self && !s_classes.empty() && melee_watch_begin(self);
   const bool result = s_meleeUpdate(self, edx, dt);
   if (watched) melee_watch_end(self);
   return result;
}

// A melee weapon stopped something: a strike (no ordnance) or a bolt or beam.
// A strike by the viewed unit's swing goes on that swing's list; the viewed
// unit's own block or deflection shakes.
void melee_blocked(uint8_t* weapon, void* ordnance)
{
   __try {
      uint8_t* blocker = weapon_object(weapon);
      if (!blocker) return;
      if (s_swing.weapon && !ordnance && s_swing.blocked < kMeleeBlockers)
         s_swing.blocker[s_swing.blocked++] = blocker;
      if (blocker != s_view.obj) return;
      const ClassShake* cs = weapon_shake(weapon);
      if (ordnance)
         play(s_deflectKick, cs, kShakeDeflect, defaults::kDeflect);
      else
         play(s_blockKick, cs, kShakeBlock, defaults::kBlock);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
}

bool __fastcall hooked_MeleeDeflect(uint8_t* self, void* edx, void* ordnance, const void* pos,
                                    const void* dir)
{
   const bool blocked = s_meleeDeflect(self, edx, ordnance, pos, dir);
   if (blocked && self && s_view.obj && !s_classes.empty()) melee_blocked(self, ordnance);
   return blocked;
}

// A trick that has just started: a flip sets the flag byte's flip bit
// (FlipAdd), a side roll only rolls (RollAdd).
void shake_for_trick(uint8_t* self)
{
   __try {
      const ClassShake* cs = find_class(fly::mClass(self));
      if (fly::mFlags(self) & fly::kFlagFlip)
         play(s_trickFlipKick, cs, kShakeTrickFlip, defaults::kTrickFlip);
      else
         play(s_trickRollKick, cs, kShakeTrickRoll, defaults::kTrickRoll);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
}

// DoTrick zeroes mTrick when it starts a trick and sets it to -1 when there is
// not enough energy or a trick is already running; a class with tricks turned
// off returns before touching it. mTrick is set to a marker for the call so
// the three cases can be told apart, and put back when DoTrick left it alone.
void __fastcall hooked_DoTrick(uint8_t* self, void* edx, int trick)
{
   const bool watch = self && self == s_view.obj;
   float* mTrick = watch ? &fly::mTrick(self) : nullptr;
   const float before = mTrick ? *mTrick : 0.0f;
   if (mTrick) *mTrick = kTrickUnset;

   s_doTrick(self, edx, trick);

   if (!mTrick) return;
   if (*mTrick == kTrickUnset) {
      *mTrick = before;
      return;
   }
   if (*mTrick != kTrickRefused) shake_for_trick(self);
}

// Stands in for CameraManager::ApplyShake at the flyer's two collision calls.
// BF2 makes them only for the flyer the chase camera follows, with the amount
// impact x 0.8 and the duration impact x 0.7. When that flyer's class sets
// CollisionShake the bump is kept for its next camera frame (update_flyer)
// and never reaches the stock queue; otherwise it goes on to the queue as
// before, where it plays as a blast.
void __fastcall bump_apply_shake(void* manager, void* edx, float amount, float duration)
{
   if (s_view.takesBumps) {
      s_bump.pending = true;
      if (amount > s_bump.amount) s_bump.amount = amount;
      return;
   }
   s_applyShake(manager, edx, amount, duration);
}

// The collision type BF2 gives an object, where its own callback reads it: in
// its tree-grid stack's row when it has a stack, else its own.
int collision_type(uint8_t* object)
{
   uint8_t* grid = object + coll::kTreeGrid;
   uint8_t* stack = at<uint8_t*>(grid, layout::TreeGridObject::kStackPtr);
   if (!stack) return at<int>(grid, layout::TreeGridObject::kData);
   return at<int>(stack, layout::TreeGridStack::kData + 4u * at<uint32_t>(grid, layout::TreeGridObject::kStackIdx));
}

// A contact on the viewed hover, before BF2 handles it: how fast the hover
// closes on what it hit, less that thing's own velocity, as BF2 works it out,
// and from where. `part` is the hover's CollisionObject part, the callback's
// `this`. Like BF2, only a contact with something whose collision type is at
// least the hover's own counts, and never a soldier. The hardest since the
// last camera frame waits for it (update_hover).
void note_hover_hit(uint8_t* part, void* result, uint8_t* other)
{
   __try {
      const int type = collision_type(other);
      if (type < collision_type(part) || type == coll::kTypeSoft) return;
      uint8_t* hover = part - hov::kCollisionPart;
      const float* v = hov::mVelocity(hover);
      float rel[3] = { v[0], v[1], v[2] };
      if (uint8_t* go = vcall_object(other, coll::kVt_GetGameObject)) {
         if (const auto* ov = reinterpret_cast<const float*>(vcall_object(go, kVt_GetVelocity)))
            for (int i = 0; i < 3; ++i) rel[i] -= ov[i];
      }
      const auto* normal = reinterpret_cast<const float*>(static_cast<uint8_t*>(result) +
                                                          layout::CollisionResult::kSeparationNormal);
      const HoverHit hit = hover_hit(rel, normal, hov::mMatrix_right(hover), hov::mMatrix_up(hover),
                                     hov::mMatrix_forward(hover));
      if (hit.speed > s_hoverHit.hit.speed) {
         s_hoverHit.pending = true;
         s_hoverHit.hit = hit;
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
}

// EntityHover::CollisionCallback, for every hover and CommandHover; only the
// viewed one, whose class sets CollisionShake, is looked at.
bool __fastcall hooked_HoverCollision(uint8_t* self, void* edx, void* result, uint8_t* other, void* restrictor)
{
   if (s_view.takesHoverHits && self && result && other && self - hov::kCollisionPart == s_view.obj)
      note_hover_hit(self, result, other);
   return s_hoverCollision(self, edx, result, other, restrictor);
}

// The reticule sees the unshaken camera: for this call only, the camera's
// world-to-camera matrix goes back to the one SetupCamera made before the shake,
// and the shaken one returns straight after. Only when the camera still holds
// this frame's shake, so anything else that set it since is left alone.
bool __fastcall hooked_ReticuleUpdate(void* self, void* edx, float dt)
{
   uint8_t* inverse = nullptr;
   __try {
      if (s_reticle.valid && s_reticle.camera) {
         uint8_t* live = s_reticle.camera + layout::RedCamera::kMatrixInverse;
         if (std::memcmp(live, s_reticle.shaken, sizeof s_reticle.shaken) == 0) {
            std::memcpy(live, s_reticle.unshaken, sizeof s_reticle.unshaken);
            inverse = live;
         }
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      inverse = nullptr;
   }
   const bool result = s_reticleUpdate(self, edx, dt);
   if (inverse) std::memcpy(inverse, s_reticle.shaken, sizeof s_reticle.shaken);
   return result;
}

// -----------------------------------------------------------------------------
// Install
// -----------------------------------------------------------------------------

// `allowHooked` accepts a JMP where the prologue should be: another GameExt
// module (controller rumble) detours SignalFire too.
bool guard(uintptr_t base, uintptr_t va, const char* what, const char* bytes, const char* mask,
           bool allowHooked = false)
{
   const auto* code = static_cast<const unsigned char*>(resolve(base, va));
   if (allowHooked && code[0] == 0xE9) return true;
   for (size_t i = 0; mask[i]; ++i) {
      if (mask[i] == 'x' && code[i] != static_cast<unsigned char>(bytes[i])) {
         install_log("[CameraShake] NOT installed: prologue mismatch at %s 0x%08X", what, (unsigned)va);
         return false;
      }
   }
   return true;
}

// Whether the CALL at `site` goes to `fn`, directly or through one JMP thunk.
bool calls(uintptr_t base, uintptr_t site, uintptr_t fn)
{
   __try {
      const auto* call = static_cast<const uint8_t*>(resolve(base, site));
      if (call[0] != 0xE8) return false;
      int32_t rel;
      std::memcpy(&rel, call + 1, sizeof rel);
      const uint8_t* target = call + 5 + rel;
      const auto* want = static_cast<const uint8_t*>(resolve(base, fn));
      if (target == want) return true;
      if (target[0] != 0xE9) return false;
      std::memcpy(&rel, target + 1, sizeof rel);
      return target + 5 + rel == want;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

void retarget(uintptr_t base, uintptr_t site, const void* to)
{
   uint8_t* call = static_cast<uint8_t*>(resolve(base, site));
   const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - reinterpret_cast<uintptr_t>(call + 5));
   std::memcpy(call + 1, &rel, sizeof rel);
}

// CollisionShake: the flyer's two collision calls to ApplyShake go through
// bump_apply_shake. Both or neither, so a class's bumps are all its own.
void install_bumps(uintptr_t base)
{
   const uintptr_t post = g_addr->flyer_post_collision_shake_call;
   const uintptr_t callback = g_addr->flyer_collision_shake_call;
   if (!g_addr->camera_manager_apply_shake || !post || !callback) {
      install_log("[CameraShake] Flyer CollisionShake: not on this build yet");
      return;
   }
   if (!guard(base, g_addr->camera_manager_apply_shake, "CameraManager::ApplyShake",
              "\x8B\x41\x28\x8B\x88\x9C\x00\x00\x00\x83\xF9\x04", "xxxxxxxxxxxx"))
      return;
   if (!calls(base, post, g_addr->camera_manager_apply_shake) ||
       !calls(base, callback, g_addr->camera_manager_apply_shake)) {
      install_log("[CameraShake] CollisionShake: the flyer collision calls (0x%08X, 0x%08X) are not "
                  "as expected", (unsigned)post, (unsigned)callback);
      return;
   }
   s_applyShake = reinterpret_cast<ApplyShakeFn>(resolve(base, g_addr->camera_manager_apply_shake));
   retarget(base, post, reinterpret_cast<const void*>(&bump_apply_shake));
   retarget(base, callback, reinterpret_cast<const void*>(&bump_apply_shake));
   install_log("[CameraShake] CollisionShake: flyer collision calls 0x%08X, 0x%08X", (unsigned)post,
               (unsigned)callback);
}

// StrikeShake, BlockShake, DeflectShake and SwingBlockedShake: detours on
// WeaponMelee::UpdateFire and WeaponMelee::Deflect, both or neither.
void install_melee(uintptr_t base, bool modtools)
{
   if (!g_addr->weapon_melee_update_fire || !g_addr->weapon_melee_deflect) {
      install_log("[CameraShake] Strike, block and deflect shakes: not on this build yet");
      return;
   }
   if (!guard(base, g_addr->weapon_melee_update_fire, "WeaponMelee::UpdateFire",
              modtools ? "\x55\x8B\xEC\x83\xE4\xF0\x81\xEC\xA4\x03\x00\x00"
                       : "\x55\x8B\xEC\x83\xE4\xF0\x81\xEC\x28\x02\x00\x00",
              modtools ? "xxxxxxxxxxxx" : "xxxxxxxxxxxx") ||
       !guard(base, g_addr->weapon_melee_deflect, "WeaponMelee::Deflect",
              modtools ? "\x55\x8B\xEC\x83\xE4\xF0\x81\xEC\x54\x01\x00\x00"
                       : "\x55\x8B\xEC\x83\xE4\xF0\x81\xEC\xE8\x00\x00\x00",
              modtools ? "xxxxxxxxxxxx" : "xxxxxxxxxxxx"))
      return;
   s_meleeHits    = modtools ? layout::WeaponMelee::kDamageDataModtools : layout::WeaponMelee::kDamageDataRelease;
   s_meleeUpdate  = reinterpret_cast<MeleeUpdateFn>(resolve(base, g_addr->weapon_melee_update_fire));
   s_meleeDeflect = reinterpret_cast<MeleeDeflectFn>(resolve(base, g_addr->weapon_melee_deflect));

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   LONG r = DetourAttach(&(PVOID&)s_meleeUpdate, hooked_MeleeUpdate);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_meleeDeflect, hooked_MeleeDeflect);
   if (r != NO_ERROR) {
      DetourTransactionAbort();
      install_log("[CameraShake] Strike, block and deflect shakes: DetourAttach failed (%ld)", (long)r);
      return;
   }
   const bool ok = DetourTransactionCommit() == NO_ERROR;
   install_log("[CameraShake] Strike, block and deflect shakes %s (UpdateFire 0x%08X, Deflect 0x%08X)",
               ok ? "installed" : "commit failed", (unsigned)g_addr->weapon_melee_update_fire,
               (unsigned)g_addr->weapon_melee_deflect);
}

// A hover's CollisionShake: a detour on EntityHover::CollisionCallback, which
// CommandHover shares. Its other shakes need no hook.
void install_hover(uintptr_t base, bool modtools)
{
   if (!g_addr->hover_collision_callback) {
      install_log("[CameraShake] Hover CollisionShake: not on this build yet");
      return;
   }
   if (!guard(base, g_addr->hover_collision_callback, "EntityHover::CollisionCallback",
              modtools ? "\x83\xEC\x2C\x53\x55\x56\x57\x8B\x7C\x24\x44"
                       : "\x55\x8B\xEC\x83\xEC\x2C\x56\x57\x8B\x7D\x0C\x8B\xF1",
              modtools ? "xxxxxxxxxxx" : "xxxxxxxxxxxxx"))
      return;
   s_hoverCollision = reinterpret_cast<HoverCollisionFn>(resolve(base, g_addr->hover_collision_callback));

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG r = DetourAttach(&(PVOID&)s_hoverCollision, hooked_HoverCollision);
   if (r != NO_ERROR) {
      DetourTransactionAbort();
      install_log("[CameraShake] Hover CollisionShake: DetourAttach failed (%ld)", (long)r);
      return;
   }
   s_hoverHooked = DetourTransactionCommit() == NO_ERROR;
   install_log("[CameraShake] Hover CollisionShake %s (CollisionCallback 0x%08X)",
               s_hoverHooked ? "installed" : "commit failed", (unsigned)g_addr->hover_collision_callback);
}

} // namespace

void camera_shake_install(uintptr_t base)
{
   const bool modtools = g_build == GameBuild::Modtools;
   if (!modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;

   if (!g_soldier || !g_addr->chase_camera_setup_camera || !g_addr->red_camera_set_matrix ||
       !g_addr->tracker_is_first_person_view || !g_addr->flyer_do_trick ||
       !g_addr->weapon_signal_fire || !g_addr->reticle_display_update) {
      install_log("[CameraShake] NOT installed: no address set for this build");
      return;
   }

   // Everything hooked or called is guarded: a wrong address behind a function
   // pointer is a crash on first use, not a decline.
   if (!guard(base, g_addr->chase_camera_setup_camera, "ChaseCamera::SetupCamera",
              modtools ? "\x55\x8B\xEC\x83\xE4\xF0\x81\xEC\x04\x01\x00\x00\x53\x8B\xD9\x8B\x43\x0C\x85\xC0"
                       : "\x55\x8B\xEC\x83\xE4\xF0\x81\xEC\xE8\x00\x00\x00\x56\x8B\xF1\x57\x89\x74\x24\x0C\x83\x7E\x0C\x00",
              modtools ? "xxxxxxxxxxxxxxxxxxxx" : "xxxxxxxxxxxxxxxxxxxxxxxx") ||
       !guard(base, g_addr->red_camera_set_matrix, "RedCamera::SetMatrix",
              modtools ? "\x56\x8B\x74\x24\x08\x8B\xC1\x57\x8D\x78\x30\xB9\x10\x00\x00\x00\xF3\xA5"
                       : "\x55\x8B\xEC\x8B\x45\x08\xF3\x0F\x6F\x00\xF3\x0F\x7F\x41\x30",
              modtools ? "xxxxxxxxxxxxxxxxxx" : "xxxxxxxxxxxxxxx") ||
       !guard(base, g_addr->tracker_is_first_person_view, "Tracker::IsFirstPersonView",
              "\x56\x57\x8B\xF9\x8B\x77\x10\x85\xF6\x74\x3D\x83\xFE\xFF\x74\x38\x81\x7E\x1C\xDD\xDD\xDD\xDD",
              "xxxxxxxxxxxxxxxxxxxxxxx") ||
       !guard(base, g_addr->flyer_do_trick, "EntityFlyer::DoTrick",
              modtools ? "\xD9\x05\x00\x00\x00\x00\x56\x8B\xF1\x8B\x8E\x6C\x06\x00\x00"
                       : "\x55\x8B\xEC\x53\x56\x57\x8B\xF9\x0F\x57\xC9\x8B\x8F\x2C\x06\x00\x00",
              modtools ? "xx????xxxxxxxxx" : "xxxxxxxxxxxxxxxxx") ||
       !guard(base, g_addr->weapon_signal_fire, "Weapon::SignalFire",
              modtools ? "\x51\x56\x8B\xF1\xE8" : "\x55\x8B\xEC\x83\xE4\xF8\x51\x56\x8B\xF1",
              modtools ? "xxxxx" : "xxxxxxxxxx", true) ||
       !guard(base, g_addr->reticle_display_update, "ReticuleDisplay::Update",
              modtools ? "\x55\x8B\xEC\x83\xE4\xF8\x83\xEC\x30\x53\x55\x56"
                       : "\x55\x8B\xEC\x83\xE4\xF8\x83\xEC\x24\x53\x8B\xD9",
              modtools ? "xxxxxxxxxxxx" : "xxxxxxxxxxxx", true))
      return;

   s_setupCamera   = reinterpret_cast<SetupCameraFn>(resolve(base, g_addr->chase_camera_setup_camera));
   s_setMatrix     = reinterpret_cast<SetMatrixFn>(resolve(base, g_addr->red_camera_set_matrix));
   s_isFirstPerson = reinterpret_cast<IsFirstPersonFn>(resolve(base, g_addr->tracker_is_first_person_view));
   s_doTrick       = reinterpret_cast<DoTrickFn>(resolve(base, g_addr->flyer_do_trick));
   s_signalFire    = reinterpret_cast<SignalFireFn>(resolve(base, g_addr->weapon_signal_fire));
   s_reticleUpdate = reinterpret_cast<ReticleUpdateFn>(resolve(base, g_addr->reticle_display_update));
   s_rng.seed(GetTickCount() ^ 0x5EED5EEDu);

   if (!odf_add_property_handler(on_property) || !odf_add_derive_handler(on_derive)) {
      install_log("[CameraShake] ODF properties unavailable: no listener slot left");
      s_odfShakes = false;
   }

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   LONG r = DetourAttach(&(PVOID&)s_setupCamera, hooked_SetupCamera);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_reticleUpdate, hooked_ReticuleUpdate);
   if (r == NO_ERROR && s_odfShakes) r = DetourAttach(&(PVOID&)s_doTrick, hooked_DoTrick);
   if (r == NO_ERROR && s_odfShakes) r = DetourAttach(&(PVOID&)s_signalFire, hooked_SignalFire);
   if (r != NO_ERROR) {
      DetourTransactionAbort();
      install_log("[CameraShake] NOT installed: DetourAttach failed (%ld)", (long)r);
      return;
   }
   const bool ok = DetourTransactionCommit() == NO_ERROR;
   install_log("[CameraShake] %s (SetupCamera 0x%08X): the stock shake drawn as a blast, ODF shakes %s",
               ok ? "installed" : "commit failed", (unsigned)g_addr->chase_camera_setup_camera,
               s_odfShakes ? "on" : "unavailable");
   if (ok && s_odfShakes) install_bumps(base);
   if (ok && s_odfShakes) install_melee(base, modtools);
   if (ok && s_odfShakes) install_hover(base, modtools);
   if (ok && s_odfShakes &&
       (!fly::mGetSpeedSpeed.off() || !fly::mMatrix_forward.off() || !fly_class::mMinSpeed.off() ||
        !fly_class::mPitchRate.off() || !fly::mControlMove.off() || !fly::mInLandingRegionFactor.off() ||
        !fly::mControlStrafe.off()))
      install_log("[CameraShake] A flyer's TurnShake, speed names in a flyer's Threshold and its "
                  "throttle-led boost and brake shakes are not on this build yet");
}
