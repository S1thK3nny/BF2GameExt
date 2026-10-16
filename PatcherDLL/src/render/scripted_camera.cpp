#include "pch.h"
#include "scripted_camera.hpp"
#include "camera_shake.hpp"
#include "camera_shake_core.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "game/Battlefront2/Source/Character.h"
#include "game/Battlefront2/Source/ChaseCamera.h"
#include "game/Battlefront2/Source/RedCamera.h"
#include "util/install_log.hpp"

#include <detours.h>

#include <cmath>
#include <cstring>

// =============================================================================
// What it calls and hooks, read off each build (docs/RE/ScriptedCamera.md):
//
//                                  modtools     Steam        GOG
//   RedCamera::SetMatrix           0x007FEED0   0x006CBEF0   0x006CCF90   called
//     thiscall(const PblMatrix*), RET 4
//   RedCamera::SetFOV              0x007FEF90   0x006CBE60   0x006CCF00   called
//     thiscall(float fov, float aspect), RET 8
//   Tracker::IsFirstPersonView     0x0049FDD0   0x0044E3C0   0x0044E3A0   called
//     thiscall(), bool in AL
//   Trackable::CheckFirstPerson    0x004BAC10   0x00658830   0x006598D0   detoured
//     thiscall(int playerId), bool in AL, RET 4
//   the path list                  0x00AE33F0   0x007DF840   0x007E0840   read
//
// Same on every build: the chase camera's owner (+0x0C), its Trackable's
// tracker (+0x1C) and its shake queue (count +0x9C, amounts +0xA0, decays
// +0xB0, four slots, as CameraManager::ApplyShake fills it); the render
// camera's matrix (+0x30), half-FOV tangents (+0x138, +0x13C) and zoom
// (+0x140); an entity's mId (+0x4), IsRtti (vtable +0x00) and matrix (+0xF0,
// EntityGeometry::mMatrix); a GameObject's mHandleId (+0x204); a Character's
// unit, vehicle and remote (+0x148, +0x14C, +0x150), each a Controllable whose
// Trackable part (+0x18) gives its GameObject (vtable +0x20), as the stock
// GetCharacterUnit reads it.
// =============================================================================

namespace scripted_camera {
namespace {

using SetMatrixFn     = void(__thiscall*)(void* camera, const float* matrix);
using SetFovFn        = void(__thiscall*)(void* camera, float fov, float aspect);
using IsFirstPersonFn = bool(__thiscall*)(void* tracker);
using HashFindFn      = void*(__cdecl*)(void* table, int slots, uint32_t hash);
using IsRttiFn        = bool(__thiscall*)(void* self, uint32_t hash);
using GetObjectFn     = uint8_t*(__thiscall*)(void* self);

SetMatrixFn     s_setMatrix     = nullptr;
SetFovFn        s_setFov        = nullptr;
IsFirstPersonFn s_isFirstPerson = nullptr;
HashFindFn      s_find          = nullptr;
void*           s_idMap         = nullptr;
uint8_t*        s_pathList      = nullptr;
uint8_t**       s_characters    = nullptr;   // &Character::sCharacters
int*            s_characterCap  = nullptr;   // &Character::sCharacterLimit
bool            s_installed     = false;

constexpr uint32_t kObjId            = 0x04;    // EntityEx::mId
constexpr uint32_t kObjMatrix        = 0xF0;    // EntityGeometry::mMatrix
constexpr uint32_t kObjHandle        = 0x204;   // PblHandled::mHandleId of a GameObject
constexpr uint32_t kCtrlTrackable    = 0x18;    // a Controllable's Trackable part
constexpr uint32_t kVtGetGameObject  = 0x20;    // on the Trackable's vtable
constexpr int      kIdMapSlots       = 0x800;
constexpr uint32_t kGeometryRtti     = pbl_hash("EntityGeometry");
constexpr uint32_t kShakeDecay       = 0xB0;    // ChaseCamera::mShakeDecay[4]
constexpr uint32_t kCamTanW          = 0x138;   // RedCamera::_fUnzoomedTanHalfFOVWidth
constexpr uint32_t kCamTanH          = 0x13C;   // ...Height
constexpr uint32_t kListFirst        = 0x4;
constexpr uint32_t kListNext         = 0x4;
constexpr uint32_t kListPath         = 0xC;
constexpr uint32_t kPathHash         = 0x14;
constexpr uint32_t kPathNodes        = 0x18;
constexpr uint32_t kPathCount        = 0x1C;
constexpr uint32_t kNodeSize         = 0x1C;

// While true, Trackable::CheckFirstPerson answers third person, and the game
// switches the player's view to it. Read by the stub below.
volatile bool s_forceThird = false;
void* s_checkFirstPerson = nullptr;   // the original, a trampoline once detoured

// thiscall(int playerId), bool in AL, RET 4: third person while a script's
// shot holds the view, else the original, every register as it found them.
__declspec(naked) void check_first_person_stub()
{
   __asm {
      cmp byte ptr [s_forceThird], 0
      jne third
      jmp dword ptr [s_checkFirstPerson]
   third:
      xor eax, eax
      ret 4
   }
}

// -----------------------------------------------------------------------------
// Units
// -----------------------------------------------------------------------------

uint8_t* named(uint32_t name)
{
   if (!name || !s_find || !s_idMap) return nullptr;
   return static_cast<uint8_t*>(s_find(s_idMap, kIdMapSlots, name));
}

uint8_t* controllable_object(uint8_t* controllable)
{
   if (!controllable) return nullptr;
   void* trackable = controllable + kCtrlTrackable;
   return reinterpret_cast<GetObjectFn>((*static_cast<void***>(trackable))[kVtGetGameObject / 4])(trackable);
}

// What a character is now: its remote, else its vehicle, else its soldier.
uint8_t* character_object(int index)
{
   if (!s_characters || !s_characterCap || index < 0 || index >= *s_characterCap) return nullptr;
   uint8_t* const array = *s_characters;
   if (!array) return nullptr;
   uint8_t* const character = array + (size_t)index * layout::Character::kSize;
   for (uint32_t slot : { layout::Character::kRemote, layout::Character::kVehicle, layout::Character::kUnit })
      if (uint8_t* ctrl = *reinterpret_cast<uint8_t**>(character + slot)) return controllable_object(ctrl);
   return nullptr;
}

uint8_t* object_of(const UnitRef& u)
{
   switch (u.type) {
   case UnitRef::Type::Name:
      return named(u.name);
   case UnitRef::Type::Handle:
      return u.object && *reinterpret_cast<uint32_t*>(u.object + kObjHandle) == u.handle ? u.object : nullptr;
   case UnitRef::Type::Character:
      return character_object(u.character);
   default:
      return nullptr;
   }
}

bool is_rtti(uint8_t* obj, uint32_t hash)
{
   return reinterpret_cast<IsRttiFn>((*reinterpret_cast<void***>(obj))[0])(obj, hash);
}

// A unit's matrix, rows right, up, forward, position. False once it is gone.
bool unit_matrix(const UnitRef& u, float m[16])
{
   __try {
      uint8_t* const obj = object_of(u);
      if (!obj || !is_rtti(obj, kGeometryRtti)) return false;
      std::memcpy(m, obj + kObjMatrix, 16 * sizeof(float));
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
   for (int i = 0; i < 16; ++i)
      if (!std::isfinite(m[i])) return false;
   return true;
}

// -----------------------------------------------------------------------------
// The script's orders
// -----------------------------------------------------------------------------

enum class Kind { None, Shot, Follow, Path };

struct State {
   Kind   kind = Kind::None;
   Shot   shot;
   Follow follow;
   Path   path;
   Curve  curve;
   Spot   aimOverride;          // CameraLookAt on a scripted view
   bool   aimOverridden = false;

   double elapsed = 0.0;        // since the order took the view
   float  blendIn = 0.0f;
   bool   fromShown = false;    // blending in from the view a script left on screen
   View   from;
   View   startView;            // on screen when the order came: a point shot's facing
   bool   releasing = false;
   float  releaseDur = 0.0f;
   float  releaseT = 0.0f;
   int    waitFrames = 0;       // a cut waits for the third-person switch
   bool   followInit = false;
   V3     followPos;
   View   lastTarget;           // the order's own view last frame, kept when its unit is lost
   bool   haveTarget = false;

   bool   looking = false;      // a look over gameplay
   Look   look;
   float  lookT = 0.0f;
   float  lookOutStart = -1.0f; // when its turn back began; negative while it holds
   float  lookOutDur = 0.0f;
   V3     lookPoint;
   bool   lookHave = false;

   bool   fovOn = false;
   bool   fovFromOrder = false;
   bool   fovFromPending = false;
   float  fovFrom = 0.0f;
   float  fovTo = 0.0f;
   float  fovDur = 0.0f;
   float  fovT = 0.0f;

   bool   shakePending = false;
   float  shakeAmount = 0.0f;
   float  shakeSeconds = 1.0f;

   View   shown;                // the view drawn last frame
   bool   haveShown = false;
   bool   playerFirstPerson = false;
};

State s;

const Common& order_common()
{
   switch (s.kind) {
   case Kind::Follow: return s.follow;
   case Kind::Path:   return s.path;
   default:           return s.shot;
   }
}

// A spot's world point now. A unit spot is its local point from the unit; a
// unit that is gone gives none.
bool spot_point(const Spot& spot, V3& out)
{
   if (spot.type == Spot::Type::Point) {
      out = spot.point;
      return true;
   }
   if (spot.type != Spot::Type::Unit) return false;
   float m[16];
   if (!unit_matrix(spot.unit, m)) return false;
   out = from_object(m, spot.local);
   return true;
}

// Where the order's view aims now: CameraLookAt's target over the order's own.
bool order_aim(const Spot& own, V3& out)
{
   if (s.aimOverridden) return spot_point(s.aimOverride, out);
   return spot_point(own, out);
}

void end_order()
{
   s.kind = Kind::None;
   s.releasing = false;
   s.fromShown = false;
   s.haveTarget = false;
   s.aimOverridden = false;
   s.waitFrames = 0;
   if (s.fovFromOrder) {
      s.fovOn = false;
      s.fovFromOrder = false;
   }
}

void start_release(float seconds)
{
   if (s.kind == Kind::None) return;
   if (!(seconds > 0.0f)) {
      end_order();
      return;
   }
   if (s.releasing) return;
   s.releasing = true;
   s.releaseDur = seconds;
   s.releaseT = 0.0f;
   // The game eases its own FOV back from the one on screen.
   if (s.fovFromOrder) {
      s.fovOn = false;
      s.fovFromOrder = false;
   }
}

void set_fov(float degrees, float seconds, bool fromOrder)
{
   s.fovOn = true;
   s.fovFromOrder = fromOrder;
   s.fovFromPending = true;   // from the FOV on screen, read next frame
   s.fovTo = degrees < 1.0f ? 1.0f : degrees > 170.0f ? 170.0f : degrees;
   s.fovDur = seconds > 0.0f ? seconds : 0.0f;
   s.fovT = 0.0f;
}

void begin(Kind kind, const Common& c)
{
   const bool onScreen = s.haveShown && (s.kind != Kind::None || s.looking);
   s.fromShown = onScreen;
   s.from = s.shown;
   s.startView = s.shown;
   s.kind = kind;
   s.elapsed = 0.0;
   s.blendIn = c.blend > 0.0f ? c.blend : 0.0f;
   s.releasing = false;
   s.releaseT = 0.0f;
   s.followInit = false;
   s.haveTarget = false;
   s.aimOverridden = false;
   s.looking = false;
   // In first person the arms and cockpit go with the view: wait for the
   // switch to third person before cutting (a blend starts from the player's
   // view anyway).
   s.waitFrames = s.playerFirstPerson ? 3 : 0;
   s_forceThird = true;
   if (c.fov > 0.0f) set_fov(c.fov, c.blend, true);
   else if (s.fovFromOrder) {
      s.fovOn = false;
      s.fovFromOrder = false;
   }
}

// -----------------------------------------------------------------------------
// Each order's own view
// -----------------------------------------------------------------------------

bool shot_view(View& out)
{
   const Shot& o = s.shot;
   V3 place;
   View facing;
   bool haveFacing = false;
   if (o.fromMatrix) {
      place = from_object(o.matrix, o.offset);
      facing = facing_view(o.matrix);
      haveFacing = true;
   } else if (o.from.type == Spot::Type::Unit) {
      float m[16];
      if (!unit_matrix(o.from.unit, m)) return false;
      place = from_object(m, o.from.local + o.offset);
      facing = facing_view(m);
      haveFacing = true;
   } else if (o.from.type == Spot::Type::Point) {
      place = o.from.point + o.offset;
   } else {
      return false;
   }
   V3 aim;
   if (order_aim(o.lookAt, aim)) {
      out = look_at(place, aim, haveFacing ? facing.right : s.startView.right);
   } else if (haveFacing) {
      out = facing;
   } else {
      out = s.startView;   // a point and nothing to look at: the way the view faced
   }
   out.pos = place;
   return true;
}

bool follow_view(float dt, View& out)
{
   const Follow& o = s.follow;
   float m[16];
   if (!unit_matrix(o.unit, m)) return false;
   const V3 desired = o.world ? V3{ m[12], m[13], m[14] } + o.offset : from_object(m, o.offset);
   if (!s.followInit) {
      s.followPos = desired;
      s.followInit = true;
   } else {
      s.followPos = trail(s.followPos, desired, dt, o.lag);
   }
   V3 aim;
   if (!order_aim(o.lookAt, aim)) aim = from_object(m, o.aim);
   const V3 hint = s.haveTarget ? s.lastTarget.right : V3{ -m[0], -m[1], -m[2] };
   out = look_at(s.followPos, aim, hint);
   return true;
}

bool path_view(View& out)
{
   const Path& o = s.path;
   const float total = s.curve.total();
   float frac;
   if (o.loop) {
      frac = o.seconds > 0.0f ? (float)std::fmod(s.elapsed, (double)o.seconds) / o.seconds : 0.0f;
   } else {
      frac = o.seconds > 0.0f ? ease(o.ease, (float)(s.elapsed / o.seconds)) : 1.0f;
   }
   const float dist = total * frac;
   int seg = 0;
   float through = 0.0f;
   const V3 pos = s.curve.at(dist, &seg, &through);

   V3 aim;
   if (order_aim(o.lookAt, aim)) {
      out = look_at(pos, aim, s.haveTarget ? s.lastTarget.right : V3{ 1.0f, 0.0f, 0.0f });
   } else if (!o.nodeViews.empty()) {
      const int n = (int)o.nodeViews.size();
      const int a = seg % n;
      const int b = s.curve.closed() ? (seg + 1) % n : (seg + 1 < n ? seg + 1 : n - 1);
      out = blend(o.nodeViews[(size_t)a], o.nodeViews[(size_t)b], through);
   } else {
      float lead = total * 0.02f;
      lead = lead < 2.0f ? 2.0f : lead > 15.0f ? 15.0f : lead;
      out = look_at(pos, ahead_on(s.curve, dist, lead), s.haveTarget ? s.lastTarget.right : V3{ 1.0f, 0.0f, 0.0f });
   }
   out.pos = pos;

   if (!o.loop && s.elapsed >= o.seconds && o.endRelease >= 0.0f) start_release(o.endRelease);
   return true;
}

bool order_view(float dt, View& out)
{
   switch (s.kind) {
   case Kind::Shot:   return shot_view(out);
   case Kind::Follow: return follow_view(dt, out);
   case Kind::Path:   return path_view(out);
   default:           return false;
   }
}

// -----------------------------------------------------------------------------
// The frame
// -----------------------------------------------------------------------------

bool first_person(uint8_t* chase)
{
   __try {
      uint8_t* const owner = *reinterpret_cast<uint8_t**>(chase + layout::ChaseCamera::kOwner);
      if (!owner) return false;
      void* const tracker = *reinterpret_cast<void**>(owner + layout::ChaseCamera::kTrackableTracker);
      return tracker && s_isFirstPerson(tracker);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

// CameraShake: appended to the chase camera's shake queue the way
// CameraManager::ApplyShake does it, a fifth one while four run dropped.
void apply_shake(uint8_t* chase)
{
   if (!s.shakePending) return;
   s.shakePending = false;
   int& count = *reinterpret_cast<int*>(chase + layout::ChaseCamera::kShakeCount);
   if (count < 0 || count >= layout::ChaseCamera::kShakeSlots) return;
   reinterpret_cast<float*>(chase + layout::ChaseCamera::kShakeAmount)[count] = s.shakeAmount;
   reinterpret_cast<float*>(chase + kShakeDecay)[count] = s.shakeAmount / s.shakeSeconds;
   ++count;
}

// The camera's aspect, height over width, as SetFOV was last given it.
float camera_aspect(uint8_t* cam)
{
   const float w = *reinterpret_cast<float*>(cam + kCamTanW);
   const float h = *reinterpret_cast<float*>(cam + kCamTanH);
   return w > 0.0f && std::isfinite(w) && std::isfinite(h) && h > 0.0f ? h / w : 0.75f;
}

// The scripted FOV this frame, in the camera's own measure; false with none.
bool fov_now(uint8_t* cam, float aspect, float dt, float& out)
{
   if (!s.fovOn) return false;
   if (s.fovFromPending) {
      const float tanW = *reinterpret_cast<float*>(cam + kCamTanW);
      s.fovFrom = game_fov(2.0f * std::atan(tanW), aspect);
      if (!std::isfinite(s.fovFrom)) s.fovFrom = s.fovTo;
      s.fovFromPending = false;
      s.fovT = 0.0f;
   }
   s.fovT += dt;
   const float degrees = s.fovFrom + (s.fovTo - s.fovFrom) * blend_weight(s.fovT, s.fovDur);
   out = widened_fov(degrees, aspect);
   return std::isfinite(out) && out > 0.01f && out < 3.1f;
}

void frame(uint8_t* chase, uint8_t* cam, float dt)
{
   if (!(dt > 0.0f)) dt = 0.0f;           // paused, or not a number
   else if (dt > 0.1f) dt = 0.1f;         // a hitch must not jump a blend

   apply_shake(chase);
   s.playerFirstPerson = first_person(chase);

   // The player's own view, as SetupCamera and camera shake left it.
   const View player = from_matrix(reinterpret_cast<float*>(cam + layout::RedCamera::kMatrix));
   View view = player;
   bool drawn = false;
   float shotWeight = 0.0f;   // how much of a shot is on screen: scopes give way to it

   if (s.kind != Kind::None) {
      if (s.waitFrames > 0) {
         if (s.playerFirstPerson) --s.waitFrames;
         else s.waitFrames = 0;
      }
      if (s.waitFrames == 0) {
         s.elapsed += dt;
         View target;
         bool have = order_view(dt, target);
         if (have) {
            s.lastTarget = target;
            s.haveTarget = true;
         } else if (s.haveTarget) {
            target = s.lastTarget;   // its unit is gone: where it last was
            have = true;
         }
         if (have) {
            // Explosions and CameraShake shake the script's view too.
            camera_shake::Offset blast;
            if (camera_shake_blast(blast)) {
               float m[16];
               to_matrix(target, m);
               camera_shake::apply(m, blast);
               target = from_matrix(m);
            }
            const float in = blend_weight((float)s.elapsed, s.blendIn);
            const View v = blend(s.fromShown ? s.from : player, target, in);
            float out = 1.0f;
            if (s.releasing) {
               s.releaseT += dt;
               out = 1.0f - blend_weight(s.releaseT, s.releaseDur);
            }
            view = blend(player, v, out);
            shotWeight = out * (s.fromShown ? 1.0f : in);
            drawn = true;
         }
         const Common& c = order_common();
         if (!s.releasing && c.time >= 0.0f && s.elapsed >= c.time) start_release(c.release);
         if (s.releasing && s.releaseT >= s.releaseDur) end_order();
      }
   } else if (s.looking) {
      s.lookT += dt;
      if (s.lookOutStart < 0.0f && s.lookT >= s.look.blend + s.look.time) {
         s.lookOutStart = s.lookT;
         s.lookOutDur = s.look.release;
      }
      V3 point;
      if (spot_point(s.look.target, point)) {
         s.lookPoint = point;
         s.lookHave = true;
      }
      if (s.lookHave) {
         float w = blend_weight(s.lookT, s.look.blend);
         if (s.lookOutStart >= 0.0f) w *= 1.0f - blend_weight(s.lookT - s.lookOutStart, s.lookOutDur);
         view = blend(player, look_at(player.pos, s.lookPoint, player.right), w);
         drawn = true;
      }
      if (s.lookOutStart >= 0.0f && s.lookT - s.lookOutStart >= s.lookOutDur) s.looking = false;
   }

   s_forceThird = s.kind != Kind::None;

   if (drawn) {
      float m[16];
      to_matrix(view, m);
      s_setMatrix(cam, m);
      camera_shake_view_replaced(cam);
   }

   // A shot shows no scope: the zoom eases toward none with it, and the game
   // eases it back after. SetFOV rebuilds the projection, which reads it.
   const float aspect = camera_aspect(cam);
   bool rebuild = false;
   if (shotWeight > 0.0f) {
      float& zoom = *reinterpret_cast<float*>(cam + layout::RedCamera::kZoom);
      if (std::isfinite(zoom) && zoom != 1.0f) {
         zoom = zoom + (1.0f - zoom) * shotWeight;
         rebuild = true;
      }
   }
   float fov = 0.0f;
   if (fov_now(cam, aspect, dt, fov)) {
      s_setFov(cam, fov, aspect);
   } else if (rebuild) {
      s_setFov(cam, 2.0f * std::atan(*reinterpret_cast<float*>(cam + kCamTanW)), aspect);
   }

   s.shown = view;
   s.haveShown = true;
}

void frame_guarded(uint8_t* chase, uint8_t* cam, float dt)
{
   __try {
      frame(chase, cam, dt);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      // An unreadable frame drops the script's view rather than fault again.
      s.kind = Kind::None;
      s.looking = false;
      s.fovOn = false;
      s_forceThird = false;
   }
}

bool guard(uintptr_t base, uintptr_t va, const char* what, const char* bytes, const char* mask)
{
   const auto* code = static_cast<const unsigned char*>(resolve(base, va));
   for (size_t i = 0; mask[i]; ++i) {
      if (mask[i] == 'x' && code[i] != static_cast<unsigned char>(bytes[i])) {
         install_log("[ScriptedCamera] NOT installed: unexpected bytes at %s 0x%08X", what, (unsigned)va);
         return false;
      }
   }
   return true;
}

} // namespace

// -----------------------------------------------------------------------------
// The commands
// -----------------------------------------------------------------------------

UnitRef unit_from_name(const char* name)
{
   UnitRef u;
   if (name && *name) {
      u.type = UnitRef::Type::Name;
      u.name = pbl_hash(name);
   }
   return u;
}

UnitRef unit_from_character(int index)
{
   UnitRef u;
   if (index >= 0) {
      u.type = UnitRef::Type::Character;
      u.character = index;
   }
   return u;
}

// A pointer is taken only when it is something the game knows by name, or what
// a character is now: never called into before it is found.
UnitRef unit_from_object(void* object)
{
   UnitRef u;
   uint8_t* const obj = static_cast<uint8_t*>(object);
   if (!obj) return u;
   __try {
      const uint32_t id = *reinterpret_cast<uint32_t*>(obj + kObjId);
      if (id && named(id) == obj) {
         u.type = UnitRef::Type::Name;
         u.name = id;
         return u;
      }
      const int cap = s_characterCap ? *s_characterCap : 0;
      for (int i = 0; i < cap; ++i) {
         if (character_object(i) == obj) {
            u.type = UnitRef::Type::Handle;
            u.object = obj;
            u.handle = *reinterpret_cast<uint32_t*>(obj + kObjHandle);
            return u;
         }
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
   return UnitRef{};
}

void shot(const Shot& order)
{
   if (!s_installed) return;
   s.shot = order;
   begin(Kind::Shot, order);
}

void follow(const Follow& order)
{
   if (!s_installed) return;
   s.follow = order;
   begin(Kind::Follow, order);
}

void path(const Path& order)
{
   if (!s_installed) return;
   s.path = order;
   s.curve.build(order.nodes, order.loop);
   begin(Kind::Path, order);
}

void look(const Look& order)
{
   if (!s_installed) return;
   if (s.kind != Kind::None) {
      aim(&order.target);
      return;
   }
   s.looking = true;
   s.look = order;
   s.lookT = 0.0f;
   s.lookOutStart = -1.0f;
   s.lookHave = false;
}

void aim(const Spot* target)
{
   if (s.kind == Kind::None) return;
   if (target && target->given()) {
      s.aimOverride = *target;
      s.aimOverridden = true;
   } else {
      s.aimOverridden = false;
   }
}

void fov(float degrees, float seconds)
{
   if (!s_installed) return;
   set_fov(degrees, seconds, false);
}

void fov_off()
{
   s.fovOn = false;
   s.fovFromOrder = false;
}

void shake(float amount, float seconds)
{
   if (!s_installed || !(amount > 0.0f)) return;
   s.shakePending = true;
   s.shakeAmount = amount > 20.0f ? 20.0f : amount;
   s.shakeSeconds = seconds > 0.01f ? seconds : 1.0f;
}

void release(float seconds)
{
   if (s.looking && s.kind == Kind::None) {
      // A look turns back now, over `seconds`, from wherever it has got to.
      if (!(seconds > 0.0f)) {
         s.looking = false;
         return;
      }
      if (s.lookOutStart < 0.0f) {
         s.lookOutStart = s.lookT;
         s.lookOutDur = seconds;
      }
      return;
   }
   start_release(seconds);
}

bool holds()
{
   return s.kind != Kind::None || s.looking;
}

bool shown(View& out)
{
   if (!s.haveShown) return false;
   out = s.shown;
   return true;
}

bool installed()
{
   return s_installed;
}

bool read_path(const char* name, std::vector<V3>& nodes)
{
   nodes.clear();
   if (!s_pathList || !name || !*name) return false;
   const uint32_t hash = pbl_hash(name);
   uint8_t* path = nullptr;
   int count = 0;
   uint8_t* data = nullptr;
   __try {
      uint8_t* node = *reinterpret_cast<uint8_t**>(s_pathList + kListFirst);
      for (int guardCount = 0; node && guardCount < 65536; ++guardCount) {
         uint8_t* const p = *reinterpret_cast<uint8_t**>(node + kListPath);
         if (!p) break;
         if (*reinterpret_cast<uint32_t*>(p + kPathHash) == hash) {
            path = p;
            break;
         }
         node = *reinterpret_cast<uint8_t**>(node + kListNext);
      }
      if (!path) return false;
      count = *reinterpret_cast<int*>(path + kPathCount);
      data = *reinterpret_cast<uint8_t**>(path + kPathNodes);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
   if (count <= 0 || count > 4096 || !data) return true;   // a path with no nodes
   for (int i = 0; i < count; ++i) {
      const float* p = reinterpret_cast<const float*>(data + (size_t)i * kNodeSize);
      V3 v;
      __try {
         v = { p[0], p[1], p[2] };
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         nodes.clear();
         return false;
      }
      if (finite(v)) nodes.push_back(v);
   }
   return true;
}

void reset()
{
   s = State{};
   s_forceThird = false;
}

void after_setup(uint8_t* chase, void* camera, float dt)
{
   if (!s_installed || !chase || !camera) return;
   if (s.kind == Kind::None && !s.looking && !s.fovOn && !s.shakePending) {
      // Nothing scripted: only keep what is on screen and the view the player has.
      if (s_forceThird) s_forceThird = false;
      __try {
         s.shown = from_matrix(reinterpret_cast<float*>(static_cast<uint8_t*>(camera) + layout::RedCamera::kMatrix));
         s.haveShown = true;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
      s.playerFirstPerson = first_person(chase);
      return;
   }
   frame_guarded(chase, static_cast<uint8_t*>(camera), dt);
}

} // namespace scripted_camera

// -----------------------------------------------------------------------------
// Install
// -----------------------------------------------------------------------------

void scripted_camera_install(uintptr_t base)
{
   using namespace scripted_camera;
   const bool modtools = g_build == GameBuild::Modtools;
   if (!modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;

   if (!g_addr->red_camera_set_matrix || !g_addr->red_camera_set_fov || !g_addr->tracker_is_first_person_view ||
       !g_addr->trackable_check_first_person || !g_addr->entityex_id_map || !g_addr->pbl_hash_table_find ||
       !g_addr->char_array_base || !g_addr->max_chars || !g_addr->path_list) {
      install_log("[ScriptedCamera] NOT installed: no address set for this build");
      return;
   }
   if (!camera_shake_hooked()) {
      install_log("[ScriptedCamera] NOT installed: it draws in camera shake's SetupCamera detour, "
                  "which is not installed");
      return;
   }

   // The functions it calls and the one it detours, prologues up to the
   // first absolute operand (masked: retail relocates them).
   if (!guard(base, g_addr->red_camera_set_fov, "RedCamera::SetFOV",
              modtools ? "\xD9\x44\x24\x04\xD8\x0D\x00\x00\x00\x00\xD9\xF2\xDD\xD8\xD9\x91\x38\x01\x00\x00"
                       : "\x55\x8B\xEC\xF3\x0F\x10\x45\x08\xF3\x0F\x59\x05\x00\x00\x00\x00\x56\x8B\xF1",
              modtools ? "xxxxxx????xxxxxxxxxx" : "xxxxxxxxxxxx????xxx") ||
       !guard(base, g_addr->trackable_check_first_person, "Trackable::CheckFirstPerson",
              modtools ? "\x56\x57\x8B\xF9\x8B\x07\xFF\x50\x24\x8B\xF0\x85\xF6"
                       : "\x55\x8B\xEC\x56\x8B\xF1\x57\x8B\x06\xFF\x50\x24\x8B\xF8\x85\xFF",
              modtools ? "xxxxxxxxxxxxx" : "xxxxxxxxxxxxxxxx") ||
       !guard(base, g_addr->tracker_is_first_person_view, "Tracker::IsFirstPersonView",
              "\x56\x57\x8B\xF9\x8B\x77\x10\x85\xF6", "xxxxxxxxx"))
      return;

   s_setMatrix     = reinterpret_cast<SetMatrixFn>(resolve(base, g_addr->red_camera_set_matrix));
   s_setFov        = reinterpret_cast<SetFovFn>(resolve(base, g_addr->red_camera_set_fov));
   s_isFirstPerson = reinterpret_cast<IsFirstPersonFn>(resolve(base, g_addr->tracker_is_first_person_view));
   s_find          = reinterpret_cast<HashFindFn>(resolve(base, g_addr->pbl_hash_table_find));
   s_idMap         = resolve(base, g_addr->entityex_id_map);
   s_pathList      = static_cast<uint8_t*>(resolve(base, g_addr->path_list));
   s_characters    = static_cast<uint8_t**>(resolve(base, g_addr->char_array_base));
   s_characterCap  = static_cast<int*>(resolve(base, g_addr->max_chars));
   s_checkFirstPerson = resolve(base, g_addr->trackable_check_first_person);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG r = DetourAttach(&(PVOID&)s_checkFirstPerson, check_first_person_stub);
   if (r != NO_ERROR) {
      DetourTransactionAbort();
      install_log("[ScriptedCamera] NOT installed: DetourAttach failed (%ld)", (long)r);
      return;
   }
   if (DetourTransactionCommit() != NO_ERROR) {
      install_log("[ScriptedCamera] NOT installed: the CheckFirstPerson detour did not commit");
      return;
   }
   s_installed = true;
   reset();
   install_log("[ScriptedCamera] installed: CameraShot, CameraFollow, CameraPath, CameraLookAt, CameraFOV, "
               "CameraShake and CameraRelease for mission scripts (CheckFirstPerson 0x%08X)",
               (unsigned)g_addr->trackable_check_first_person);
}

void scripted_camera_uninstall()
{
   using namespace scripted_camera;
   if (!s_installed) return;
   s_installed = false;
   s_forceThird = false;
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)s_checkFirstPerson, check_first_person_stub);
   DetourTransactionCommit();
}
