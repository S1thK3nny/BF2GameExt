#include "pch.h"
#include "target_bar_latch.hpp"
#include "target_bar_geometry.hpp"
#include "target_bar_fade.hpp"
#include "hud_horizon_math.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"
#include "core/pbl_hash.hpp"
#include "weapon/barrel_fire_origin.hpp"   // engine_ray_hit

#include <detours.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// =============================================================================
// See the header for the behaviour and why it is built on the engine's own
// target.* events.  This file documents the layouts and calling conventions it
// relies on. Hook layouts were read per build and independently re-read on
// 2026-09-19; visual geometry was revised on 2026-09-21. See docs/RE/HUDSystem.md.
//
// THE ONE WRITE INTO GAME MEMORY
//
// HUD::GameEvents::UpdateWeaponEvents picks its target as weapon->mTarget, else
//
//     controlled->mReticuleTarget[channel]        PblHandle { object, handleId }
//
// read as `LEA reg,[ctrl + channel*8 + 0x164]` then `MOV [reg]` / `MOV [reg+4]` -
// raw bytes checked on all three builds (modtools 0x006B3630, Steam 0x0056104F,
// GOG 0x00561DCF; Steam and GOG are byte-identical there).  Phantom has the array
// at +0x160: it is a different compile and is NOT a layout reference.
//
// The lend lives strictly inside our detour of HUD::GameEvents::Update: written
// immediately before the original runs, restored immediately after.  Game logic
// is single-threaded and nothing but the HUD update executes in that window, so
// no other system can observe the lent value.  The restore only fires if the slot
// still holds exactly what we wrote.
//
// CONVENTIONS THAT DIFFER BY BUILD - each is part of the contract
//
//   HUD::GameEvents::Update   modtools cdecl(float dt), pushed and never read.
//                             Steam/GOG: LTCG dropped the parameter; void(void).
//   EventClass::FindByHashID  modtools cdecl, hash on the stack.
//                             Steam/GOG: hash in ECX, no stack args.
//
// Same everywhere: EventClass::Create cdecl varargs, GameEvents::Open void(void),
// NetGame::GetLocalPlayer cdecl(uint), GameObject::IsMyEnemy thiscall RET 4,
// Damageable::ApplyDamage thiscall with five stack args RET 0x14, and the four
// virtuals used here (slot offsets read from each binary, not carried over):
//
//   Trackable::GetGameObject       vptr at Controllable+0x18, slot +0x20
//   GameObject::IsRtti             primary vptr, slot +0x00, RET 4
//   GameObject::GetTargetPoint     primary vptr, slot +0x50, RET 0x10
//   GameObject::GetSmoothedMatrix  primary vptr, slot +0x110
//
// EventClass::Create never checks for an existing name: a second Create with the
// same name makes an orphan no element can ever bind to, because FindByHashID
// returns the first match.  Always find first.
//
// WHY ApplyDamage AND NOT RegisterHit
//
// Character::RegisterHit is what drives the stock hit marker, and hooking its one
// call site looked like the natural latch.  It is wrong online: on a multiplayer
// client Damageable::ApplyDamageCommon diverts into the cosmetic
// ApplyNetClientDamage, so RegisterHit never runs there and the latch would
// silently never take.  ApplyDamage has no client early-out.  DamageDesc+0x00 is
// the attacker's Character*, which gives an exact local-player test on every role
// - a host sees every player's hits pass through here.
// =============================================================================

float g_targetBarLatchSeconds = 2.5f;

// ---- Layout.  Identical on modtools, Steam and GOG. ------------------------
static constexpr int kChr_Unit          = 0x148;
static constexpr int kChr_Vehicle       = 0x14C;
static constexpr int kChr_Remote        = 0x150;

static constexpr int kCtrl_Trackable    = 0x18;
static constexpr int kCtrl_EyeDir       = 0xE8;
static constexpr int kCtrl_AimStart     = 0x148;
static constexpr int kCtrl_ReticuleTgt  = 0x164;  // + channel * 8
static constexpr int kVt_GetGameObject  = 0x20;   // on the Trackable vptr

static constexpr int kVt_GetTargetPoint = 0x50;   // on the primary vptr
static constexpr int kVt_GetSmoothedMtx = 0x110;
static constexpr int kGO_Active         = 0xDC;   // byte
static constexpr int kGO_MatrixTrans    = 0x120;  // mMatrix (+0xF0) .trans
static constexpr int kGO_Model          = 0x130;  // GameModel*
static constexpr int kGO_Flags          = 0x1FC;  // bit 3 = alive
static constexpr int kGO_HandleId       = 0x204;
static constexpr int kGO_CollisionCentre = 0x18; // live TreeGrid centre mirror
static constexpr int kGO_CollisionBounds = 0x60; // world AABB: min/max XYZ
// GameModel primary RedModel +0x20. Its bounds offset differs by build.
// Collision bounds suit infantry, but vehicles can use sphere-derived cubes.
static constexpr uint32_t kSoldierRtti = 0x5E8739F4;

static constexpr int kDmg_ToObject      = 0x140;  // GameObject* = Damageable* - 0x140
static constexpr int kDD_Character      = 0x00;   // DamageDesc.mDamageOwner.mCharacter

static constexpr int kPD_WeaponStride   = 0x28;   // sizeof(WeaponData)
static constexpr int kWD_TargetObject   = 0x14;
static constexpr int kWD_TargetHandleId = 0x18;

static constexpr int kCM_Camera0        = 0x24;   // CameraManager::mRedCamera[0]
static constexpr int kCam_Matrix        = 0x30;   // right, up, forward, trans
static constexpr int kCam_TanHalfFovW   = 0x144;
static constexpr int kCam_TanHalfFovH   = 0x148;

static constexpr int kEC_HandlerList    = 0x08;   // self-linked when nobody listens

static constexpr int kTypeVector3       = 9;      // HUD::EventClass::Type
static constexpr int kSafeBodyId        = -1;     // what the engine passes unlocked
static constexpr int kChannels          = 2;

// ---- Tuning ------------------------------------------------------------------
static constexpr int   kRayMask     = 0x9A;   // soldiers, vehicles, terrain, statics
static constexpr float kRayClear    = 0.75f;  // start the ray clear of the shooter
static constexpr int   kLosInterval = 6;      // ticks between line-of-sight rays
static constexpr float kMaxTickSecs = 0.25f;  // a pause must not run the hold out

// ---- Engine entry points -----------------------------------------------------
using fn_event_send_t    = void(__fastcall*)(void* ecx, void* edx);
using fn_create_t        = void*(__cdecl*)(int type, const char* fmt, ...);
using fn_find_cdecl_t    = void*(__cdecl*)(unsigned hash);
using fn_find_fastcall_t = void*(__fastcall*)(unsigned hash);
using fn_local_player_t  = void*(__cdecl*)(unsigned localIndex);
using fn_is_my_enemy_t   = bool(__thiscall*)(void* self, void* other);
using fn_get_object_t    = void*(__thiscall*)(void* self);
using fn_target_point_t  = const float*(__thiscall*)(void* self, float* out,
                                                     const float* aimStart,
                                                     const float* aimDir, int bodyId);
using fn_smoothed_t      = const float*(__thiscall*)(void* self);
using fn_rtti_t          = bool(__thiscall*)(void* self, uint32_t hash);

using fn_open_t          = void(__cdecl*)();
using fn_update_float_t  = void(__cdecl*)(float dt);
using fn_update_void_t   = void(__cdecl*)();
using fn_apply_damage_t  = bool(__thiscall*)(void* self, void* desc, void* hitDir,
                                             void* hitPos, int bodyId, unsigned flags);

static fn_open_t         original_Open        = nullptr;
static fn_update_float_t original_UpdateFloat = nullptr;  // modtools
static fn_update_void_t  original_UpdateVoid  = nullptr;  // Steam, GOG
static fn_apply_damage_t original_ApplyDamage = nullptr;

static fn_event_send_t    s_eventSend    = nullptr;
static fn_create_t        s_create       = nullptr;
static fn_find_cdecl_t    s_findCdecl    = nullptr;
static fn_find_fastcall_t s_findFastcall = nullptr;
static fn_local_player_t  s_localPlayer  = nullptr;
static fn_is_my_enemy_t   s_isMyEnemy    = nullptr;

static uintptr_t* s_eventList  = nullptr;  // EventClass::sList
static uint8_t*   s_playerData = nullptr;  // HUD::GameEvents::gPlayerData[0]
static uintptr_t* s_cameraMgr  = nullptr;  // CameraManager::sInstance
static const uint32_t* s_screenWidth = nullptr;
static const uint32_t* s_screenHeight = nullptr;
// Built-in edge reservations retain the tested placement. They constrain only
// the anchor; artwork sizing, alignment and manual offsets belong to the .hud.
static constexpr target_bar_geometry::Insets kScreenInsets = { 0.10f, 0.10f, 0.107f, 0.0f };

static bool s_installed = false;

// ---- State.  Every object pointer here dies with the mission: see hooked_Open.
struct Handle {
   uint8_t* obj;
   uint32_t id;
};

static void*  s_evtPosition[kChannels];   // EventClass*, recreated every mission
static target_bar_fade::Position s_position[kChannels]; // persists through death fade
static Handle s_lastFocus[kChannels];     // keeps position alive through a fade

// Independent HUD opt-in, sharing Open/Update rather than stacking detours on
// those guarded entry points. A rotation listener does NOT enable the latch.
static constexpr char kHorizonEvent[] = "player1.reticule.horizonRotation";
static void* s_evtHorizon = nullptr;
static hud_horizon::State s_horizon;
static const uint8_t* s_horizonCamera = nullptr;
static float s_horizonRotation[3] = {};

static void*  s_localChr = nullptr;       // non-null only while someone listens
static Handle s_pending  = {};            // written by the ApplyDamage detour
static Handle s_latch    = {};
static double s_now      = 0.0;           // seconds, advanced only by HUD ticks
static double s_expiry   = 0.0;
static bool   s_losOk    = false;
static int    s_losAge   = kLosInterval;
static bool   s_announced = false;

struct Lend {
   uint32_t* slot;       // &mReticuleTarget[ch], two dwords
   uint32_t  saved[2];   // what was there
   uint32_t  written[2]; // what we put there
   bool      active;
};
static Lend s_lend[kChannels];

// ---------------------------------------------------------------------------
// Reading the engine
// ---------------------------------------------------------------------------

// The engine's own PblHandle test: non-null and the generation still matches.
static bool handle_ok(const Handle& h)
{
   return h.obj != nullptr && *(const uint32_t*)(h.obj + kGO_HandleId) == h.id;
}

static bool is_alive(const uint8_t* obj)
{
   return ((*(const uint32_t*)(obj + kGO_Flags)) >> 3 & 1u) != 0;
}

static void* event_find(unsigned hash)
{
   if (s_findCdecl)    return s_findCdecl(hash);
   if (s_findFastcall) return s_findFastcall(hash);
   return nullptr;
}

// Is any .hud element bound to this event?  PblListDouble is self-linked when
// empty, so this is one comparison and needs no walk.
static bool has_listener(const void* cls)
{
   if (!cls) return false;
   const uintptr_t node = (uintptr_t)cls + kEC_HandlerList;
   return *(const uintptr_t*)node != node;
}

static void* vcall_object(void* subobject)
{
   void** vt = *(void***)subobject;
   return ((fn_get_object_t)vt[kVt_GetGameObject / 4])(subobject);
}

// Used only for the native body point in the line-of-sight test. Model-space
// visual bounds below use the FULL rendered matrix, including its orientation.
static void rendered_pose_offset(uint8_t* obj, float out[3])
{
   out[0] = out[1] = out[2] = 0.0f;
   if (*(obj + kGO_Active) == 0) return;

   void** vt = *(void***)obj;
   const float* sm = ((fn_smoothed_t)vt[kVt_GetSmoothedMtx / 4])(obj);
   if (!sm) return;    // may be a shared static: copy immediately

   const float* sim = (const float*)(obj + kGO_MatrixTrans);
   out[0] = sm[12] - sim[0];
   out[1] = sm[13] - sim[1];
   out[2] = sm[14] - sim[2];
}

// The body point the engine would put a lock-on bracket on, moved onto the
// RENDERED pose the way UpdateWeaponEvents does it.  Without that term a marker
// follows the simulated position and jitters on a multiplayer client; offline
// the offset is zero.
static void target_point(uint8_t* obj, const uint8_t* controlled, float out[3])
{
   void** vt = *(void***)obj;
   float  tmp[3] = { 0.0f, 0.0f, 0.0f };
   const float* p = ((fn_target_point_t)vt[kVt_GetTargetPoint / 4])(
      obj, tmp, (const float*)(controlled + kCtrl_AimStart),
      (const float*)(controlled + kCtrl_EyeDir), kSafeBodyId);
   if (!p) p = tmp;   // both implementations return `out`; read through the result anyway
   out[0] = p[0]; out[1] = p[1]; out[2] = p[2];

   float renderOffset[3];
   rendered_pose_offset(obj, renderOffset);
   out[0] += renderOffset[0];
   out[1] += renderOffset[1];
   out[2] += renderOffset[2];
}

static const uint8_t* hud_camera()
{
   const uintptr_t mgr = s_cameraMgr ? *s_cameraMgr : 0;
   return mgr ? *(const uint8_t* const*)(mgr + kCM_Camera0) : nullptr;
}

static void reset_horizon()
{
   s_horizon.reset();
   s_horizonCamera = nullptr;
   s_horizonRotation[0] = s_horizonRotation[1] = s_horizonRotation[2] = 0.0f;
}

// Independent of target-bar/local-player state. Only publishes rotation; native
// reticule position, mesh, visibility and alpha continue to work as before.
static void tick_horizon()
{
   __try {
      if (!s_eventList || *s_eventList == (uintptr_t)s_eventList) {
         s_evtHorizon = nullptr;
         reset_horizon();
         return;
      }
      if (!has_listener(s_evtHorizon)) { reset_horizon(); return; }

      const uint8_t* cam = hud_camera();
      if (cam != s_horizonCamera) reset_horizon();
      s_horizonCamera = cam;
      if (cam && s_screenWidth && s_screenHeight) {
         s_horizonRotation[2] = s_horizon.update(
            (const float*)(cam + kCam_Matrix),
            *(const float*)(cam + kCam_TanHalfFovW),
            *(const float*)(cam + kCam_TanHalfFovH), *s_screenWidth, *s_screenHeight);
      } else {
         reset_horizon();
      }
      struct { void* mClass; const float* mData; } ev = { s_evtHorizon, s_horizonRotation };
      s_eventSend(&ev, nullptr);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A torn-down camera must not escape into the game's HUD update.
      reset_horizon();
   }
}

// Use the top centre of a WORLD bounding box, not the extrema of a projected
// rectangle. This is the healthbar-only SWBFIII approach supplied by the user:
// no head bone, no perspective size and no POI-icon displacement.
static bool project_target_anchor(uint8_t* obj, float out[3])
{
   using namespace target_bar_geometry;
   const uint8_t* cam = hud_camera();
   if (!cam || !s_screenWidth || !s_screenHeight) return false;

   float matrix[16];
   const float* rendered = (const float*)(obj + kGO_MatrixTrans - 0x30);
   void** vt = *(void***)obj;
   if (*(obj + kGO_Active)) {
      const float* smoothed = ((fn_smoothed_t)vt[kVt_GetSmoothedMtx / 4])(obj);
      if (smoothed) rendered = smoothed;
   }
   std::memcpy(matrix, rendered, sizeof(matrix));
   if (!valid_matrix(matrix)) return false;

   Box world;
   bool haveBounds = false;
   if (((fn_rtti_t)vt[0])(obj, kSoldierRtti)) {
      Box collision;
      std::memcpy(&collision, obj + kGO_CollisionBounds, sizeof(collision));
      const float* liveCentre = (const float*)(obj + kGO_CollisionCentre);
      const float* sim = (const float*)(obj + kGO_MatrixTrans);
      const float centre[3] = { liveCentre[0] + matrix[12] - sim[0],
                                liveCentre[1] + matrix[13] - sim[1],
                                liveCentre[2] + matrix[14] - sim[2] };
      // Preserve the non-animated, stance-sized bbox the user preferred. The
      // centre is refreshed even in the native jump/flail path (AABB is not).
      haveBounds = recenter_bounds(collision, centre, world);
   }
   if (!haveBounds) {
      const uint8_t* gameModel = *(const uint8_t* const*)(obj + kGO_Model);
      if (!gameModel) return false;
      const uint8_t* model = *(const uint8_t* const*)(gameModel + 0x20);
      if (!model) return false;
      Box local;
      std::memcpy(&local, model + (g_build == GameBuild::Modtools ? 0x98 : 0x88), sizeof(local));
      // Vehicles/props use visual model bounds, not sphere-derived collision
      // cubes. Form the world AABB with the full rendered rotation/translation.
      if (!world_bounds(local, matrix, world)) return false;
   }

   if (!project_anchor(world, (const float*)(cam + kCam_Matrix),
                        *(const float*)(cam + kCam_TanHalfFovW),
                        *(const float*)(cam + kCam_TanHalfFovH), out)) return false;
   return pin_to_screen(out, *s_screenWidth, *s_screenHeight, kScreenInsets);
}

// Nothing but the two ends of the line in the way.  Both ends are EXCLUDED rather
// than recognised: CollisionManager::RayCallback compares each candidate's
// GetGameObject() against the list, so it takes the same base pointer a handle
// holds.  If that reading were ever wrong the ray would stop on the target itself
// and report "no line of sight" - the bar would simply not linger.  It cannot
// crash: the list is only compared against.
static bool line_of_sight(const uint8_t* controlled, uint8_t* localObj, uint8_t* target,
                          const float point[3])
{
   const float* eye = (const float*)(controlled + kCtrl_AimStart);
   float d[3] = { point[0] - eye[0], point[1] - eye[1], point[2] - eye[2] };
   const float dist = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
   if (!(dist > kRayClear * 2.0f)) return true;   // point blank; also rejects NaN

   d[0] /= dist; d[1] /= dist; d[2] /= dist;
   const float start[3] = { eye[0] + d[0]*kRayClear, eye[1] + d[1]*kRayClear,
                            eye[2] + d[2]*kRayClear };

   void* exclude[2] = { localObj, target };
   void* hit = nullptr;
   engine_ray_hit(start, d, dist - kRayClear, &hit, exclude, 2, kRayMask);
   return hit == nullptr;
}

static void send_position(int ch)
{
   struct { void* mClass; const float* mData; } ev = { s_evtPosition[ch], s_position[ch].value };
   s_eventSend(&ev, nullptr);
}

// ---------------------------------------------------------------------------
// The latch
// ---------------------------------------------------------------------------

static void clear_latch()
{
   s_latch  = Handle{};
   s_losOk  = false;
   s_losAge = kLosInterval;
}

static void clear_all_objects()
{
   clear_latch();
   s_pending  = Handle{};
   s_localChr = nullptr;
   for (int ch = 0; ch < kChannels; ++ch) {
      s_lastFocus[ch] = Handle{};
      s_position[ch].reset();
      s_lend[ch]      = Lend{};
   }
}

static void restore_lends()
{
   for (int ch = 0; ch < kChannels; ++ch) {
      Lend& l = s_lend[ch];
      if (!l.active) continue;
      // Only undo our own write.  If anything changed the slot meanwhile it is
      // theirs now, and putting a stale value back would be worse than leaving it.
      if (l.slot[0] == l.written[0] && l.slot[1] == l.written[1]) {
         l.slot[0] = l.saved[0];
         l.slot[1] = l.saved[1];
      }
      l.active = false;
   }
}

static void advance_clock()
{
   static LARGE_INTEGER freq = {};
   static LARGE_INTEGER last = {};
   LARGE_INTEGER now;
   QueryPerformanceCounter(&now);
   if (freq.QuadPart == 0) { QueryPerformanceFrequency(&freq); last = now; }

   double dt = (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart;
   last = now;
   // Update is not called while paused, so wall time would run the hold out
   // behind a pause menu.  Count ticks, not seconds on the wall.
   if (dt < 0.0) dt = 0.0;
   if (dt > kMaxTickSecs) dt = kMaxTickSecs;
   s_now += dt;
}

static void refresh_hold()
{
   s_expiry = s_now + (double)g_targetBarLatchSeconds;
}

// Runs immediately BEFORE the engine's HUD update.
static void tick_before()
{
   __try {
      if (!s_eventList) return;

      // EventClass::DestroyAll empties the list at mission end.  Our classes went
      // with it, and so did every object we were holding.
      if (*s_eventList == (uintptr_t)s_eventList) {
         s_evtPosition[0] = s_evtPosition[1] = nullptr;
         clear_all_objects();
         return;
      }

      const bool listening = has_listener(s_evtPosition[0]) || has_listener(s_evtPosition[1]);
      if (!listening) {            // opt-in by data: no binding, no behaviour at all
         clear_all_objects();
         return;
      }
      if (!s_announced) {
         s_announced = true;
         install_log("[TargetBarLatch] a .hud element is bound to target.position; "
                     "latch active, hold %.2fs", (double)g_targetBarLatchSeconds);
      }

      advance_clock();

      uint8_t* chr = (uint8_t*)s_localPlayer(0);
      s_localChr = chr;            // what the ApplyDamage detour compares against
      if (!chr) { clear_latch(); s_pending = Handle{}; return; }

      // Same selection HUD::GameEvents::Update makes: remote, else vehicle, else unit.
      uint8_t* controlled = *(uint8_t**)(chr + kChr_Remote);
      if (!controlled) controlled = *(uint8_t**)(chr + kChr_Vehicle);
      if (!controlled) controlled = *(uint8_t**)(chr + kChr_Unit);
      if (!controlled) { clear_latch(); s_pending = Handle{}; return; }

      uint8_t* localObj = (uint8_t*)vcall_object(controlled + kCtrl_Trackable);
      if (!localObj) { clear_latch(); s_pending = Handle{}; return; }

      // A hit recorded since the last tick.  Hitting someone else transfers.
      if (s_pending.obj) {
         const Handle hit = s_pending;
         s_pending = Handle{};
         if (handle_ok(hit) && hit.obj != localObj && is_alive(hit.obj) &&
             s_isMyEnemy(localObj, hit.obj)) {
            if (hit.obj != s_latch.obj) s_losAge = kLosInterval;   // re-test sight now
            s_latch = hit;
            refresh_hold();
         }
      }

      if (s_latch.obj && (!handle_ok(s_latch) || !is_alive(s_latch.obj))) clear_latch();

      // What is genuinely under the reticle, per channel.
      bool slotEmpty[kChannels];
      for (int ch = 0; ch < kChannels; ++ch) {
         const uint32_t* slot = (const uint32_t*)(controlled + kCtrl_ReticuleTgt + ch * 8);
         const Handle aimed = { (uint8_t*)(uintptr_t)slot[0], slot[1] };
         slotEmpty[ch] = !handle_ok(aimed);
         if (slotEmpty[ch] || !s_latch.obj) continue;

         if (aimed.obj == s_latch.obj) refresh_hold();   // still on him: keep it alive
         else                          clear_latch();    // looked at someone else: drop it
      }

      if (!s_latch.obj) return;
      if (g_targetBarLatchSeconds > 0.0f && s_now > s_expiry) { clear_latch(); return; }

      // Out of sight or behind the camera: do not lend, but KEEP the latch.
      float point[3], screen[3];
      target_point(s_latch.obj, controlled, point);
      if (!project_target_anchor(s_latch.obj, screen)) return;

      if (++s_losAge >= kLosInterval) {
         s_losAge = 0;
         s_losOk  = line_of_sight(controlled, localObj, s_latch.obj, point);
      }
      if (!s_losOk) return;

      for (int ch = 0; ch < kChannels; ++ch) {
         if (!slotEmpty[ch]) continue;
         Lend& l = s_lend[ch];
         l.slot     = (uint32_t*)(controlled + kCtrl_ReticuleTgt + ch * 8);
         l.saved[0] = l.slot[0];
         l.saved[1] = l.slot[1];
         l.written[0] = (uint32_t)(uintptr_t)s_latch.obj;
         l.written[1] = s_latch.id;
         l.slot[0]    = l.written[0];
         l.slot[1]    = l.written[1];
         l.active     = true;
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      // Never leave a lend behind, and never let a bad read here be worse than
      // not having the feature.
      restore_lends();
      clear_all_objects();
   }
}

// Runs immediately AFTER the engine's HUD update.
static void tick_after()
{
   bool lent = false;
   for (int ch = 0; ch < kChannels; ++ch) lent = lent || s_lend[ch].active;
   restore_lends();   // first, and outside the guard: nothing below may skip it

   __try {
      if (!s_playerData || !s_localChr) return;

      Handle projected = {};
      float anchor[3];
      bool anchorValid = false;
      for (int ch = 0; ch < kChannels; ++ch) {
         if (!has_listener(s_evtPosition[ch])) {
            s_lastFocus[ch] = Handle{};
            s_position[ch].reset();
            continue;
         }

         // The target the engine settled on, after its own filtering.
         const uint8_t* wd = s_playerData + ch * kPD_WeaponStride;
         const Handle shown = { *(uint8_t* const*)(wd + kWD_TargetObject),
                                *(const uint32_t*)(wd + kWD_TargetHandleId) };

         Handle focus = Handle{};
         bool sameTarget = true;
         if (handle_ok(shown)) {
            focus = shown;
            sameTarget = shown.obj == s_lastFocus[ch].obj && shown.id == s_lastFocus[ch].id;
            s_lastFocus[ch] = shown;
            if (s_latch.obj) {
               // Not ours: the weapon's own lock picked someone.  One tick late,
               // harmless - the engine displayed that unit this tick regardless.
               if (shown.obj != s_latch.obj) clear_latch();
               else if (!lent)               refresh_hold();
            }
         } else if (handle_ok(s_lastFocus[ch])) {
            focus = s_lastFocus[ch];   // keep following only while still alive
         } else {
            // No object left to follow. Keep the cached position for its native
            // fade, but drop the stale handle instead of reading it next tick.
            s_lastFocus[ch] = Handle{};
         }
         const bool alive = focus.obj && is_alive(focus.obj);
         if (alive) {
            if (focus.obj != projected.obj || focus.id != projected.id) {
               projected = focus;
               anchorValid = project_target_anchor(focus.obj, anchor);
            }
         }
         // Never query dead-unit bounds: death changes the collision/model pose
         // and pulled the fading bar down into the corpse. Keep screen position
         // through death/despawn; live projection failures still hide normally.
         s_position[ch].update(sameTarget, alive, alive && anchorValid ? anchor : nullptr);
         send_position(ch); // position only: preserve native enable/fade events
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      clear_all_objects();
   }
}

// ---------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------

static void __cdecl hooked_UpdateFloat(float dt)   // modtools
{
   tick_before();
   original_UpdateFloat(dt);
   tick_after();
   tick_horizon();
}

static void __cdecl hooked_UpdateVoid()            // Steam, GOG: the float was dropped
{
   tick_before();
   original_UpdateVoid();
   tick_after();
   tick_horizon();
}

// HUD::Manager::Open switches to GameMemory::RunTimeHeap before calling
// GameEvents::Open and restores the previous heap only after it returns, so
// creating here lands on the same heap as the stock events.  It also runs before
// any .lvl is read: Item::ReadEvent resolves names with FindByHashID only and
// never creates, so a class made any later could not be bound by a .hud at all.
static void __cdecl hooked_Open()
{
   original_Open();

   // A new mission.  Every GameObject pointer from the last one is dead memory.
   clear_all_objects();
   s_announced = false;
   s_evtPosition[0] = s_evtPosition[1] = nullptr;
   s_evtHorizon = nullptr;
   reset_horizon();
   // Separate guards keep either event family usable if the other fails.
   __try {
      s_evtHorizon = event_find(pbl_hash(kHorizonEvent));
      if (!s_evtHorizon) s_evtHorizon = s_create(kTypeVector3, "%s", kHorizonEvent);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      s_evtHorizon = nullptr;
   }
   __try {
      for (int ch = 0; ch < kChannels; ++ch) {
         char name[64];
         _snprintf_s(name, sizeof(name), _TRUNCATE, "player1.weapon%d.target.position", ch + 1);

         void* cls = event_find(pbl_hash(name));
         if (!cls)   // literal name through "%s": Create runs its fmt through vsnprintf
            cls = s_create(kTypeVector3, "%s", name);
         s_evtPosition[ch] = cls;
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      s_evtPosition[0] = s_evtPosition[1] = nullptr;
   }
}

static bool __fastcall hooked_ApplyDamage(void* self, void* /*edx*/, void* desc, void* hitDir,
                                          void* hitPos, int bodyId, unsigned flags)
{
   // Read BEFORE the original: it zeroes the shooter handle in the desc when that
   // handle is stale.  s_localChr is only non-null while a .hud is listening, so
   // with no binding this is one load and one compare.
   if (s_localChr && desc && self &&
       *(void* const*)((const uint8_t*)desc + kDD_Character) == s_localChr) {
      uint8_t* victim = (uint8_t*)self - kDmg_ToObject;
      s_pending.obj = victim;
      s_pending.id  = *(const uint32_t*)(victim + kGO_HandleId);
   }
   return original_ApplyDamage(self, desc, hitDir, hitPos, bodyId, flags);
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

// Prologue guard with wildcards.  The retail images can be rebased at load, so an
// embedded absolute address is not a usable fingerprint: mask those bytes out.
struct Guard {
   const char*    what;
   uintptr_t      va;
   const uint8_t* bytes;
   const char*    mask;      // 'x' compare, '?' skip
   bool           mayBeDetoured;
};

static bool guard_ok(uintptr_t exe_base, const Guard& g)
{
   const uint8_t* p = (const uint8_t*)resolve(exe_base, g.va);
   const size_t   n = std::strlen(g.mask);

   // A sibling module (aim assist) may already have detoured ApplyDamage.  Detours
   // writes a 5-byte JMP and pads the rest of the last instruction it displaced
   // with 0xCC - one byte on the retail prologue, none on modtools - so skip the
   // first eight bytes and fingerprint the four after them.
   const size_t from = (g.mayBeDetoured && p[0] == 0xE9) ? 8 : 0;

   for (size_t i = from; i < n; ++i) {
      if (g.mask[i] == 'x' && p[i] != g.bytes[i]) {
         install_log("[TargetBarLatch] NOT installed: prologue mismatch at %s 0x%08X: "
                     "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                     g.what, (unsigned)g.va, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                     p[8], p[9], p[10], p[11]);
         return false;
      }
   }
   return true;
}

void target_bar_latch_install(uintptr_t exe_base)
{
   const bool modtools = (g_build == GameBuild::Modtools);
   const bool retail   = (g_build == GameBuild::Steam || g_build == GameBuild::GOG);
   if (!modtools && !retail) return;

   if (g_addr->hud_event_class_create == 0 || g_addr->hud_event_class_find == 0 ||
       g_addr->hud_event_class_list == 0 || g_addr->hud_game_events_open == 0 ||
       g_addr->hud_game_events_update == 0 || g_addr->hud_player_data == 0 ||
       g_addr->net_game_get_local_player == 0 || g_addr->camera_manager_instance == 0 ||
       g_addr->hud_screen_width == 0 || g_addr->hud_screen_height == 0 ||
       g_addr->game_object_is_my_enemy == 0 || g_addr->hud_event_send == 0 ||
       g_addr->apply_damage == 0) {
      install_log("[TargetBarLatch] NOT installed: no address set for this build");
      return;
   }

   // Every function we CALL is guarded as well as every one we detour: a wrong
   // address behind a function pointer is a crash on first use, not a decline.
   static constexpr uint8_t kCreateMt[] = { 0x8B,0x4C,0x24,0x08,0x81,0xEC,0x04,0x04,0x00,0x00,0x56,0x8D };
   static constexpr uint8_t kCreateRt[] = { 0x55,0x8B,0xEC,0x6A,0xFF,0x68,0,0,0,0,0x64,0xA1 };
   static constexpr uint8_t kFindMt[]   = { 0x8B,0x0D,0,0,0,0,0x81,0xF9,0,0,0,0 };
   static constexpr uint8_t kFindRt[]   = { 0xA1,0,0,0,0,0x8B,0xD1,0x3D,0,0,0,0 };
   static constexpr uint8_t kOpenMt[]   = { 0x53,0x55,0x56,0x57,0x68,0,0,0,0,0x33,0xDB,0x6A };
   static constexpr uint8_t kOpenRt[]   = { 0x53,0x56,0x57,0x68,0,0,0,0,0x6A,0x04,0xC6,0x05 };
   static constexpr uint8_t kUpdateMt[] = { 0x55,0x8B,0xEC,0x83,0xE4,0xF8,0x81,0xEC,0x70,0x01,0x00,0x00 };
   static constexpr uint8_t kUpdateRt[] = { 0x55,0x8B,0xEC,0x81,0xEC,0x6C,0x01,0x00,0x00,0x53,0x56,0x57 };
   static constexpr uint8_t kLocalMt[]  = { 0x55,0x8B,0xEC,0x83,0xEC,0x0C,0x8B,0x45,0x08,0x3B,0x05,0 };
   static constexpr uint8_t kLocalRt[]  = { 0x55,0x8B,0xEC,0xE8,0,0,0,0,0x39,0x45,0x08,0x72 };
   static constexpr uint8_t kEnemyMt[]  = { 0x8B,0x44,0x24,0x04,0x50,0xE8,0,0,0,0,0x33,0xC9 };
   static constexpr uint8_t kEnemyRt[]  = { 0x55,0x8B,0xEC,0xFF,0x75,0x08,0xE8,0,0,0,0,0x33 };
   static constexpr uint8_t kDamageMt[] = { 0x83,0xEC,0x28,0x53,0x55,0x56,0x8B,0xF1,0x8B,0x4C,0x24,0x44 };
   static constexpr uint8_t kDamageRt[] = { 0x55,0x8B,0xEC,0x83,0xEC,0x28,0x53,0x56,0x57,0xFF,0x75,0x14 };

   const Guard guards[] = {
      { "EventClass::Create",      g_addr->hud_event_class_create,
        modtools ? kCreateMt : kCreateRt, modtools ? "xxxxxxxxxxxx" : "xxxxxx????xx", false },
      { "EventClass::FindByHashID", g_addr->hud_event_class_find,
        modtools ? kFindMt : kFindRt,     modtools ? "xx????xx????" : "x????xxx????", false },
      { "GameEvents::Open",        g_addr->hud_game_events_open,
        modtools ? kOpenMt : kOpenRt,     modtools ? "xxxxx????xxx" : "xxxx????xxxx", false },
      { "GameEvents::Update",      g_addr->hud_game_events_update,
        modtools ? kUpdateMt : kUpdateRt, "xxxxxxxxxxxx", false },
      { "NetGame::GetLocalPlayer", g_addr->net_game_get_local_player,
        modtools ? kLocalMt : kLocalRt,   modtools ? "xxxxxxxxxxx?" : "xxxx????xxxx", false },
      { "GameObject::IsMyEnemy",   g_addr->game_object_is_my_enemy,
        modtools ? kEnemyMt : kEnemyRt,   modtools ? "xxxxxx????xx" : "xxxxxxx????x", false },
      { "Damageable::ApplyDamage", g_addr->apply_damage,
        modtools ? kDamageMt : kDamageRt, "xxxxxxxxxxxx", true },
   };
   for (const Guard& g : guards)
      if (!guard_ok(exe_base, g)) return;

   s_eventSend   = (fn_event_send_t)resolve(exe_base, g_addr->hud_event_send);
   s_create      = (fn_create_t)resolve(exe_base, g_addr->hud_event_class_create);
   s_localPlayer = (fn_local_player_t)resolve(exe_base, g_addr->net_game_get_local_player);
   s_isMyEnemy   = (fn_is_my_enemy_t)resolve(exe_base, g_addr->game_object_is_my_enemy);
   s_eventList   = (uintptr_t*)resolve(exe_base, g_addr->hud_event_class_list);
   s_playerData  = (uint8_t*)resolve(exe_base, g_addr->hud_player_data);
   s_cameraMgr   = (uintptr_t*)resolve(exe_base, g_addr->camera_manager_instance);
   s_screenWidth = (const uint32_t*)resolve(exe_base, g_addr->hud_screen_width);
   s_screenHeight = (const uint32_t*)resolve(exe_base, g_addr->hud_screen_height);

   // The hash travels on the stack on modtools and in ECX on the retail builds.
   void* find = resolve(exe_base, g_addr->hud_event_class_find);
   if (modtools) s_findCdecl    = (fn_find_cdecl_t)find;
   else          s_findFastcall = (fn_find_fastcall_t)find;

   original_Open        = (fn_open_t)resolve(exe_base, g_addr->hud_game_events_open);
   original_ApplyDamage = (fn_apply_damage_t)resolve(exe_base, g_addr->apply_damage);
   void* update         = resolve(exe_base, g_addr->hud_game_events_update);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   LONG r = DetourAttach(&(PVOID&)original_Open, hooked_Open);
   if (r == NO_ERROR) {
      if (modtools) {
         original_UpdateFloat = (fn_update_float_t)update;
         r = DetourAttach(&(PVOID&)original_UpdateFloat, hooked_UpdateFloat);
      } else {
         original_UpdateVoid = (fn_update_void_t)update;
         r = DetourAttach(&(PVOID&)original_UpdateVoid, hooked_UpdateVoid);
      }
   }
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)original_ApplyDamage, hooked_ApplyDamage);

   if (r != NO_ERROR) {
      DetourTransactionAbort();
      install_log("[TargetBarLatch] NOT installed: DetourAttach failed (%ld)", (long)r);
      return;
   }
   s_installed = (DetourTransactionCommit() == NO_ERROR);

   install_log("[TargetBarLatch] %s (Open 0x%08X, Update 0x%08X %s, ApplyDamage 0x%08X, "
               "hold %.2fs). Inert until a .hud binds player1.weaponN.target.position",
               s_installed ? "installed" : "commit failed",
               (unsigned)g_addr->hud_game_events_open, (unsigned)g_addr->hud_game_events_update,
               modtools ? "cdecl(float)" : "void(void)", (unsigned)g_addr->apply_damage,
               (double)g_targetBarLatchSeconds);
   if (s_installed)
      install_log("[HudHorizon] available: EventRotation(\"%s\"); inert without a binding", kHorizonEvent);
}

void target_bar_latch_uninstall()
{
   if (!s_installed) return;

   restore_lends();
   s_evtHorizon = nullptr;
   reset_horizon();

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_Open, hooked_Open);
   if (original_UpdateFloat) DetourDetach(&(PVOID&)original_UpdateFloat, hooked_UpdateFloat);
   if (original_UpdateVoid)  DetourDetach(&(PVOID&)original_UpdateVoid, hooked_UpdateVoid);
   DetourDetach(&(PVOID&)original_ApplyDamage, hooked_ApplyDamage);
   DetourTransactionCommit();

   s_installed = false;
}
