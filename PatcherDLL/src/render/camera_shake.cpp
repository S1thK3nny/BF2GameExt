#include "pch.h"
#include "camera_shake.hpp"
#include "camera_shake_core.hpp"
#include "core/entity_layout.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/layout/chase_camera.hpp"
#include "core/layout/flyer.hpp"
#include "core/layout/red_camera.hpp"
#include "core/layout/weapon.hpp"
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
//
// Same on every build: the chase camera fields and its shake queue
// (layout::ChaseCamera); the render camera's zoom (layout::RedCamera); the
// Trackable's GetGameObject (+0x1C) and GetControllable (+0x28) slots, the two
// Tracker::IsFirstPersonView itself calls; a Controllable's Trackable part at
// +0x18; GameObject::IsRtti (+0x00, thiscall(hash), RET 4) and GetEntityClass
// (+0x28), as in hud_class_icons.cpp; a GameObject's Damageable at +0x140, as
// controller_rumble.cpp and aim_assist.cpp read it; the soldier's mState at
// Controllable + g_soldier->mState. The flyer fields are per build
// (layout::Flyer).
//
// How the shake gets drawn: SetupCamera builds mMatrix, turns it by the stock
// shake unless mission time is past mShakeSuppressUntil, and hands it to
// RedCamera::SetMatrix. With Smooth on, the hook holds mShakeSuppressUntil at
// FLT_MAX for the call, so the stock turn is skipped; either way it then moves
// and turns mMatrix by its own offset and sets the camera again. The aim never
// reads mMatrix (docs/RE/CameraShake.md), so shots are unaffected.
//
// The reticule is the one thing that would follow the shake: ReticuleDisplay::
// Update places it by projecting the aim point through the render camera's
// _MatrixInverse, so it chased the aim point across the shaken view. Its detour
// puts the unshaken inverse back for that call only, so the reticule holds still.
// =============================================================================

bool  g_cameraShakeEnabled  = true;
bool  g_cameraShakeSmooth   = true;
float g_cameraShakeStrength = 1.0f;
float g_cameraShakeChannel[kCameraShakeChannels] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

namespace {
using namespace camera_shake;
namespace cam = layout::ChaseCamera;

constexpr uint32_t kVt_GetGameObject   = 0x1C;  // Trackable vtable
constexpr uint32_t kVt_GetControllable = 0x28;  // Trackable vtable
constexpr uint32_t kCtrl_Trackable     = 0x18;  // a Controllable's Trackable part
constexpr uint32_t kVt_IsRtti          = 0x00;  // GameObject primary vtable
constexpr uint32_t kVt_GetEntityClass  = 0x28;  // GameObject primary vtable

// A GameObject's Damageable part is at +0x140 (Phantom PDB: the vptr at +0x140,
// Damageable_data from +0x144), on every build.
constexpr uint32_t kObj_Health    = 0x144;  // Damageable::mCurHealth
constexpr uint32_t kObj_MaxHealth = 0x148;  // Damageable::mMaxHealth

// RTTI hashes are PblHash of the class name, hashed by each class's static
// initialiser (EntityFlyer: modtools 0x00A168C0, Steam 0x00402AD0).
constexpr uint32_t kSoldierRtti = pbl_hash("EntitySoldier");
constexpr uint32_t kFlyerRtti   = pbl_hash("EntityFlyer");
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

// Rate at which the sprint judder eases in and out, per second (the spec's 3/s).
constexpr float kSprintEase = 3.0f;

using SetupCameraFn   = void(__fastcall*)(uint8_t* self, void* edx, void* camera, float dt);
using SetMatrixFn     = void(__thiscall*)(void* camera, const void* matrix);
using IsFirstPersonFn = bool(__thiscall*)(void* tracker);
using DoTrickFn       = void(__fastcall*)(uint8_t* self, void* edx, int trick);
using SignalFireFn    = void(__fastcall*)(uint8_t* weapon, void* edx);
using ReticleUpdateFn = bool(__fastcall*)(void* self, void* edx, float dt);
using ObjectFn        = uint8_t*(__thiscall*)(void* self);
using IsRttiFn        = bool(__thiscall*)(void* self, uint32_t hash);

SetupCameraFn   s_setupCamera   = nullptr;
SetMatrixFn     s_setMatrix     = nullptr;
IsFirstPersonFn s_isFirstPerson = nullptr;
DoTrickFn       s_doTrick       = nullptr;
SignalFireFn    s_signalFire    = nullptr;
ReticleUpdateFn s_reticleUpdate = nullptr;
layout::Flyer::Offsets s_flyer  = layout::Flyer::kModtools;

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

// -----------------------------------------------------------------------------
// ODF properties: seven shakes, each a scale and eight details, kept per class
// and inherited through the reader's Derive
// -----------------------------------------------------------------------------

enum Field : uint8_t { kScale, kPitch, kYaw, kRollAxis, kPush, kLength, kRise, kRate, kLimit, kFieldCount };

constexpr const char* kShakeNames[kCameraShakeChannels] = {
   "Fire", "Hit", "Land", "Roll", "Sprint", "Brake", "Blast",
};
constexpr const char* kFieldSuffixes[kFieldCount] = {
   "Shake", "ShakePitch", "ShakeYaw", "ShakeRoll", "ShakePush", "ShakeLength", "ShakeRise", "ShakeRate",
   "ShakeLimit",
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
   return scale > 0.0f;
}

// -----------------------------------------------------------------------------
// The view: what the chase camera follows, and the shakes running on it
// -----------------------------------------------------------------------------

struct View {
   uint8_t* obj          = nullptr;   // GameObject the chase camera follows
   int      soldierState = -1;
   int      flyerState   = -1;
   float    prevSpeed    = -1.0f;
   AirTime  air;
   HitSense hit;
};

View        s_view;
KickChannel s_fireKick, s_hitKick, s_landKick, s_rollKick;
float       s_sprintLevel = 0.0f;
Shape       s_sprintShape = defaults::kSprintSoldier;
Hold        s_boost, s_brake, s_decel;
Shape       s_boostShape = defaults::kSprintFlyer;
Shape       s_brakeShape = defaults::kBrake;
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
void update_soldier(uint8_t* owner, const ClassShake* cs, float dt, float& sprintTarget)
{
   uint8_t* ctrl = vcall_object(owner, kVt_GetControllable);
   const int state = ctrl ? at<int>(ctrl, g_soldier->mState) : -1;
   const int prev = s_view.soldierState;
   s_view.soldierState = state;

   if (s_view.air.update(soldier_airborne(state), soldier_grounded(state), dt))
      play(s_landKick, cs, kShakeLand, defaults::kLandSoldier);

   if (first_person(owner)) return;
   if (state == kSoldierRoll && prev != kSoldierRoll && prev != -1)
      play(s_rollKick, cs, kShakeRoll, defaults::kRollSoldier);
   Shape shape;
   float scale;
   if (state == kSoldierSprint && shape_for(cs, kShakeSprint, defaults::kSprintSoldier, false, shape, scale)) {
      s_sprintShape = shape;
      sprintTarget = scale;
   }
}

// On a flyer, sprinting is boosting, a landing is touching down and a roll is a
// trick (from the DoTrick detour below).
void update_flyer(uint8_t* obj, const ClassShake* cs, float dt, float& boostTarget, float& brakeTarget)
{
   const float* v = &at<float>(obj, s_flyer.velocity);
   const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
   if (dt > 0.0f) {
      const float decel = s_view.prevSpeed >= 0.0f ? (s_view.prevSpeed - speed) / dt : 0.0f;
      s_decel.advance(decel > 0.0f ? decel : 0.0f, dt, 0.05f, 0.15f);
      s_view.prevSpeed = speed;
   }

   const int state = at<int>(obj, s_flyer.state);
   const int prev = s_view.flyerState;
   s_view.flyerState = state;
   if (state == layout::Flyer::kStateLanded &&
       (prev == layout::Flyer::kStateLanding || prev == layout::Flyer::kStateFlying))
      play(s_landKick, cs, kShakeLand, defaults::kLandFlyer);
   if (state != layout::Flyer::kStateFlying) return;

   Shape shape;
   float scale;
   if ((at<uint8_t>(obj, s_flyer.flags) & layout::Flyer::kFlagBoost) &&
       shape_for(cs, kShakeSprint, defaults::kSprintFlyer, false, shape, scale)) {
      s_boostShape = shape;
      boostTarget = scale;
   }
   if (shape_for(cs, kShakeBrake, defaults::kBrake, false, shape, scale)) {
      s_brakeShape = shape;
      brakeTarget = scale * brake_intensity(s_decel.level);
   }
}

// Any unit: a drop in its health is a hit, sized by how much it lost.
void update_hit(uint8_t* obj, const ClassShake* cs, float dt)
{
   const float maxHealth = at<float>(obj, kObj_MaxHealth);
   const float hit = s_view.hit.update(at<float>(obj, kObj_Health), hit_threshold(maxHealth), dt);
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

void shake_view(uint8_t* self, void* camera, float dt, bool smooth)
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
   }

   const bool flyer = obj && is_rtti(obj, kFlyerRtti);
   const ClassShake* cs = nullptr;
   if (obj && !s_classes.empty())
      cs = find_class(flyer ? at<void*>(obj, s_flyer.cls) : vcall_object(obj, kVt_GetEntityClass));

   float sprintTarget = 0.0f, boostTarget = 0.0f, brakeTarget = 0.0f;
   if (g_cameraShakeEnabled && obj) {
      if (flyer)                          update_flyer(obj, cs, dt, boostTarget, brakeTarget);
      else if (is_rtti(obj, kSoldierRtti)) update_soldier(owner, cs, dt, sprintTarget);
      update_hit(obj, cs, dt);
   }

   s_fireKick.advance(dt);
   s_hitKick.advance(dt);
   s_landKick.advance(dt);
   s_rollKick.advance(dt);
   s_sprintLevel = approach(s_sprintLevel, sprintTarget, kSprintEase, dt);
   s_boost.advance(boostTarget, dt, 0.1f, 0.3f);
   s_brake.advance(brakeTarget, dt, 0.08f, 0.25f);

   const float* k = g_cameraShakeChannel;
   Offset total;
   total += s_fireKick.value(s_time).scaled(k[kShakeFire]);
   total += s_hitKick.value(s_time).scaled(k[kShakeHit]);
   total += s_landKick.value(s_time).scaled(k[kShakeLand]);
   total += s_rollKick.value(s_time).scaled(k[kShakeRoll]);
   total += judder(s_sprintShape, s_time, s_sprintLevel).scaled(k[kShakeSprint]);
   total += sway(s_boostShape, s_time, s_boost.level).scaled(k[kShakeSprint]);
   total += sway(s_brakeShape, s_time, s_brake.level).scaled(k[kShakeBrake]);
   if (smooth) total += blast_offset(self, camera, g_cameraShakeEnabled ? cs : nullptr).scaled(k[kShakeBlast]);
   total = clamp_offset(total.scaled(g_cameraShakeStrength));
   if (total.negligible()) return;

   float* m = &at<float>(self, cam::kMatrix);   // rows right, up, back, position
   if (smooth) std::memcpy(self + cam::kPreShakeMatrix, m, 16 * sizeof(float));
   apply(m, total);
   s_setMatrix(camera, m);

   s_reticle.camera = static_cast<uint8_t*>(camera);
   std::memcpy(s_reticle.shaken, s_reticle.camera + layout::RedCamera::kMatrixInverse,
               sizeof s_reticle.shaken);
   s_reticle.valid = true;
}

// -----------------------------------------------------------------------------
// Hooks
// -----------------------------------------------------------------------------

void shake_view_guarded(uint8_t* self, void* camera, float dt, bool smooth)
{
   __try {
      shake_view(self, camera, dt, smooth);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      // An unreadable frame keeps the view SetupCamera made.
   }
}

void __fastcall hooked_SetupCamera(uint8_t* self, void* edx, void* camera, float dt)
{
   const bool smooth = g_cameraShakeSmooth;
   float& suppress = at<float>(self, cam::kShakeSuppressUntil);
   const float saved = suppress;
   if (smooth) suppress = FLT_MAX;   // mission time is never past it: no stock turn
   s_setupCamera(self, edx, camera, dt);
   if (smooth) suppress = saved;
   shake_view_guarded(self, camera, dt, smooth);
}

void shake_for_fire(uint8_t* weapon)
{
   __try {
      uint8_t* owner = at<uint8_t*>(weapon, layout::Weapon::kOwner);
      if (!owner || vcall_object(owner + kCtrl_Trackable, kVt_GetGameObject) != s_view.obj) return;
      const ClassShake* cs = find_class(at<void*>(weapon, layout::Weapon::kClass));
      if (!cs) cs = find_class(at<void*>(weapon, layout::Weapon::kStart));
      play(s_fireKick, cs, kShakeFire, defaults::kFire);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
}

void __fastcall hooked_SignalFire(uint8_t* weapon, void* edx)
{
   s_signalFire(weapon, edx);
   if (g_cameraShakeEnabled && s_view.obj && weapon && !s_classes.empty()) shake_for_fire(weapon);
}

void shake_for_trick(uint8_t* self)
{
   __try {
      play(s_rollKick, find_class(at<void*>(self, s_flyer.cls)), kShakeRoll, defaults::kRollFlyer);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
}

// DoTrick zeroes mTrick when it starts a trick and sets it to -1 when there is
// not enough energy; a class with tricks turned off returns before touching it.
// mTrick is set to a marker for the call so the three cases can be told apart,
// and put back when DoTrick left it alone.
void __fastcall hooked_DoTrick(uint8_t* self, void* edx, int trick)
{
   const bool watch = g_cameraShakeEnabled && self && self == s_view.obj;
   float* mTrick = watch ? &at<float>(self, s_flyer.trick) : nullptr;
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

float clamp_strength(float v) { return std::isfinite(v) ? clampf(v, 0.0f, 5.0f) : 1.0f; }

} // namespace

void camera_shake_install(uintptr_t base)
{
   const bool modtools = g_build == GameBuild::Modtools;
   if (!modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;

   g_cameraShakeStrength = clamp_strength(g_cameraShakeStrength);
   for (float& s : g_cameraShakeChannel) s = clamp_strength(s);
   if (!g_cameraShakeEnabled && !g_cameraShakeSmooth) {
      install_log("[CameraShake] off in the INI (Enabled=0, Smooth=0)");
      return;
   }
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

   s_flyer = modtools ? layout::Flyer::kModtools : layout::Flyer::kRelease;
   s_setupCamera   = reinterpret_cast<SetupCameraFn>(resolve(base, g_addr->chase_camera_setup_camera));
   s_setMatrix     = reinterpret_cast<SetMatrixFn>(resolve(base, g_addr->red_camera_set_matrix));
   s_isFirstPerson = reinterpret_cast<IsFirstPersonFn>(resolve(base, g_addr->tracker_is_first_person_view));
   s_doTrick       = reinterpret_cast<DoTrickFn>(resolve(base, g_addr->flyer_do_trick));
   s_signalFire    = reinterpret_cast<SignalFireFn>(resolve(base, g_addr->weapon_signal_fire));
   s_reticleUpdate = reinterpret_cast<ReticleUpdateFn>(resolve(base, g_addr->reticle_display_update));
   s_rng.seed(GetTickCount() ^ 0x5EED5EEDu);

   if (g_cameraShakeEnabled &&
       (!odf_add_property_handler(on_property) || !odf_add_derive_handler(on_derive))) {
      install_log("[CameraShake] ODF properties unavailable: no listener slot left");
      g_cameraShakeEnabled = false;
   }

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   LONG r = DetourAttach(&(PVOID&)s_setupCamera, hooked_SetupCamera);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_reticleUpdate, hooked_ReticuleUpdate);
   if (r == NO_ERROR && g_cameraShakeEnabled) r = DetourAttach(&(PVOID&)s_doTrick, hooked_DoTrick);
   if (r == NO_ERROR && g_cameraShakeEnabled) r = DetourAttach(&(PVOID&)s_signalFire, hooked_SignalFire);
   if (r != NO_ERROR) {
      DetourTransactionAbort();
      install_log("[CameraShake] NOT installed: DetourAttach failed (%ld)", (long)r);
      return;
   }
   const bool ok = DetourTransactionCommit() == NO_ERROR;
   const float* k = g_cameraShakeChannel;
   install_log("[CameraShake] %s (SetupCamera 0x%08X): stock shake %s, ODF shake %s, strength %.2f "
               "(fire %.2f hit %.2f land %.2f roll %.2f sprint %.2f brake %.2f blast %.2f)",
               ok ? "installed" : "commit failed", (unsigned)g_addr->chase_camera_setup_camera,
               g_cameraShakeSmooth ? "as a blast" : "as stock", g_cameraShakeEnabled ? "on" : "off",
               g_cameraShakeStrength, k[kShakeFire], k[kShakeHit], k[kShakeLand], k[kShakeRoll],
               k[kShakeSprint], k[kShakeBrake], k[kShakeBlast]);
}
