#include "pch.h"
#include "target_bar_latch.hpp"
#include "target_bar_selection.hpp"
#include "hud_number_math.hpp"
#include "hud_class_icons.hpp"
#include "hud_command_posts.hpp"
#include "hud_bar_fill_from.hpp"
#include "hud_editor_properties.hpp"
#include "hud_sub_pixel.hpp"
#include "target_bar_geometry.hpp"
#include "target_bar_fade.hpp"
#include "hud_horizon_math.hpp"
#include "hud_world_markers_core.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"
#include "core/pbl_hash.hpp"
#include "game/Battlefront2/Source/Character.h"

#include <detours.h>

#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// =============================================================================
// See the header for the behaviour and why it is built on the engine's own
// target.* events.  This file documents the layouts and calling conventions it
// relies on. Hook layouts were read per build and independently re-read on
// 2026-09-19; geometry revised 2026-09-21, selection inputs re-read 2026-09-22.
// See docs/RE/HUDSystem.md.
//
// THE WRITES INTO GAME MEMORY
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
// Two lends write those handles. The hold fills an EMPTY reticule slot with the
// retained target. The rider pair writes the member it is showing over the
// engine's pick, in whichever of the two handles the pick was read from. Nothing
// else in the HUD update reads Weapon::mTarget: a charging launcher's lock-on
// marker comes from WeaponLauncher::GetLocked and GetLockedTargetBodyId, which
// read the launcher's own mCurTarget and mCurTargetBodyID (Phantom 0x007C05A0,
// 0x007C05E0), and no other HUD::GameEvents function touches the field.
//
// Each lend lives strictly inside our detour of HUD::GameEvents::Update: written
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
// NetGame::GetLocalPlayer cdecl(uint), and the virtuals used here (slot offsets
// read per build, not carried over from Phantom):
//
//   Trackable::GetGameObject       vptr at Controllable+0x18, slot +0x20
//   GameObject::IsRtti             primary vptr, slot +0x00, RET 4
//   Controllable weapon index     primary vptr, slot +0x3C, channel, RET 4
//   Controllable weapon pointer   primary vptr, slot +0x40, index, RET 4
//   GameObject::GetSmoothedMatrix  primary vptr, slot +0x110
//   GameObject::GetControllable    primary vptr, slot +0x6C, no arguments
//
// The rider test is the engine's own: PlayerController::Update skips a candidate
// riding the player's vehicle with GetControllable -> Controllable::mCharacter
// (+0xCC; Phantom +0xC8) -> Character::mVehicle (+0x14C) -> Trackable (+0x18)
// GetGameObject (+0x20), Steam 0x0061B863, GOG 0x0061C8D3. Modtools reads the
// same chain at 0x004CE784 to swap a rider for its vehicle.
//
// EventClass::Create never checks for an existing name: a second Create with the
// same name makes an orphan no element can ever bind to, because FindByHashID
// returns the first match.  Always find first.
//
// Selection retention: capture the natural weapon/reticle input before lending,
// then require the engine's filtered HUD result to agree. Retained results never
// refresh themselves.
// =============================================================================

float g_targetBarLatchSeconds = 0.5f;

// ---- Layout.  Identical on modtools, Steam and GOG. ------------------------

static constexpr int kCtrl_Trackable    = 0x18;
static constexpr int kCtrl_ReticuleTgt  = 0x164;  // + channel * 8
static constexpr int kVt_GetGameObject  = 0x20;   // on the Trackable vptr
static constexpr int kVt_GetWeaponIndex = 0x3C;  // Controllable primary vptr
static constexpr int kVt_GetWeapon      = 0x40;
static constexpr int kVt_GetSmoothedMtx = 0x110;
static constexpr int kVt_GetControllable = 0x6C;  // GameObject primary vptr
static constexpr int kCtrl_Character    = 0xCC;   // Controllable::mCharacter
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

static constexpr int kGO_CurHealth      = 0x144;

static constexpr int kPD_WeaponStride   = 0x28;   // sizeof(WeaponData)
static constexpr int kWD_TargetObject   = 0x14;
static constexpr int kWD_TargetHandleId = 0x18;

static constexpr int kCM_Camera0        = 0x24;   // CameraManager::mRedCamera[0]
static constexpr int kCam_Matrix        = 0x30;   // right, up, forward, trans
static constexpr int kCam_TanHalfFovW   = 0x144;
static constexpr int kCam_TanHalfFovH   = 0x148;

static constexpr int kEC_HandlerList    = 0x08;   // self-linked when nobody listens

static constexpr int kTypeFloat         = 4;      // HUD::EventClass::Type
static constexpr int kTypeVector3       = 9;
static constexpr int kChannels          = 2;

// ---- Tuning ------------------------------------------------------------------
static constexpr float kMaxTickSecs = 0.25f;  // a pause must not run the hold out
static constexpr double kPairDwellSecs = 0.3; // rider/vehicle: picked this long to switch

// ---- Engine entry points -----------------------------------------------------
using fn_event_send_t    = void(__fastcall*)(void* ecx, void* edx);
using fn_create_t        = void*(__cdecl*)(int type, const char* fmt, ...);
using fn_find_cdecl_t    = void*(__cdecl*)(unsigned hash);
using fn_find_fastcall_t = void*(__fastcall*)(unsigned hash);
using fn_local_player_t  = void*(__cdecl*)(unsigned localIndex);
using fn_get_object_t    = void*(__thiscall*)(void* self);
using fn_weapon_index_t  = int(__thiscall*)(void* self, int channel);
using fn_get_weapon_t    = uint8_t*(__thiscall*)(void* self, int index);
using fn_smoothed_t      = const float*(__thiscall*)(void* self);
using fn_rtti_t          = bool(__thiscall*)(void* self, uint32_t hash);
using fn_controllable_t  = uint8_t*(__thiscall*)(void* self);

using fn_open_t          = void(__cdecl*)();
using fn_update_float_t  = void(__cdecl*)(float dt);
using fn_update_void_t   = void(__cdecl*)();

static fn_open_t         original_Open        = nullptr;
static fn_update_float_t original_UpdateFloat = nullptr;  // modtools
static fn_update_void_t  original_UpdateVoid  = nullptr;  // Steam, GOG

static fn_event_send_t    s_eventSend    = nullptr;
static fn_create_t        s_create       = nullptr;
static fn_find_cdecl_t    s_findCdecl    = nullptr;
static fn_find_fastcall_t s_findFastcall = nullptr;
static fn_local_player_t  s_localPlayer  = nullptr;

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
using target_bar_selection::Handle;
using target_bar_selection::same;
static_assert(sizeof(Handle) == 8, "PblHandle layout requires a 32-bit build");

static void*  s_evtPosition[kChannels];   // EventClass*, recreated every mission
static void*  s_evtDistance[kChannels];   // likewise; independent of the latch
static uint32_t s_distanceBits[kChannels];
static bool   s_distanceSent[kChannels];
static target_bar_fade::Position s_position[kChannels]; // persists through death fade
static Handle s_lastFocus[kChannels];     // keeps position alive through a fade

// Independent HUD opt-in, sharing Open/Update rather than stacking detours on
// those guarded entry points. A rotation listener does NOT enable the latch.
static constexpr char kHorizonEvent[] = "player1.reticule.horizonRotation";
static void* s_evtHorizon = nullptr;
static hud_horizon::State s_horizon;
static const uint8_t* s_horizonCamera = nullptr;
static float s_horizonRotation[3] = {};

static void* s_localChr = nullptr;
static uint8_t* s_controlled = nullptr;
static Handle s_owner = {};
static target_bar_selection::Retention s_retained[kChannels];
static target_bar_selection::Pair s_pair[kChannels];
// The engine's pick, sampled BEFORE any lending (weapon then reticle), after the
// rider pair has chosen which member of a vehicle/rider pair to show.
static Handle s_natural[kChannels];
static bool   s_pairLent[kChannels];      // this tick shows a pair member over the pick
static double s_now      = 0.0;           // seconds, advanced only by HUD ticks
static bool   s_announced = false;

struct Lend {
   uint32_t* slot;       // a PblHandle the HUD reads: mReticuleTarget[ch] or Weapon::mTarget
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

// Same source precedence as native UpdateWeaponEvents, sampled before lending.
// Verified on modtools/Steam/GOG: primary virtual +3C(channel) -> index,
// +40(index) -> Weapon*, then mTarget +128 (modtools) / +104 (retail).
// `source` is the handle the pick was read from, for the rider pair's lend.
static Handle natural_target(uint8_t* controlled, int channel, uint8_t*& weapon,
                             uint32_t*& source)
{
   source = nullptr;
   void** vt = *(void***)controlled;
   const int index = ((fn_weapon_index_t)vt[kVt_GetWeaponIndex / 4])(controlled, channel);
   weapon = index >= 0 ? ((fn_get_weapon_t)vt[kVt_GetWeapon / 4])(controlled, index) : nullptr;
   if (!weapon) return {};
   const int offset = g_build == GameBuild::Modtools ? 0x128 : 0x104;
   Handle target;
   std::memcpy(&target, weapon + offset, sizeof(target));
   if (handle_ok(target)) {
      source = (uint32_t*)(weapon + offset);
      return target;
   }
   uint32_t* reticule = (uint32_t*)(controlled + kCtrl_ReticuleTgt + channel * 8);
   std::memcpy(&target, reticule, sizeof(target));
   if (!handle_ok(target)) return {};
   source = reticule;
   return target;
}

// The object a target shows as part of: a mounted rider's vehicle, anything
// else itself. The engine's own chain; see the header notes for the addresses.
static const void* display_group(const Handle& h)
{
   if (!handle_ok(h)) return nullptr;
   void** vt = *(void***)h.obj;
   const uint8_t* ctrl = ((fn_controllable_t)vt[kVt_GetControllable / 4])(h.obj);
   if (!ctrl) return h.obj;
   const uint8_t* chr = *(const uint8_t* const*)(ctrl + kCtrl_Character);
   if (!chr) return h.obj;
   uint8_t* vehicle = *(uint8_t* const*)(chr + layout::Character::kVehicle);
   if (!vehicle) return h.obj;
   const void* obj = vcall_object(vehicle + kCtrl_Trackable);
   return obj ? obj : h.obj;
}

// Shows `h` for this HUD update only; restore_lends puts the slot back.
static void lend(int ch, uint32_t* slot, const Handle& h)
{
   Lend& l = s_lend[ch];
   l.slot = slot;
   l.saved[0] = slot[0]; l.saved[1] = slot[1];
   l.written[0] = (uint32_t)(uintptr_t)h.obj;
   l.written[1] = h.id;
   slot[0] = l.written[0]; slot[1] = l.written[1];
   l.active = true;
}

static bool retainable(const Handle& target)
{
   if (!handle_ok(target) || !is_alive(target.obj)) return false;
   const float health = *(const float*)(target.obj + kGO_CurHealth);
   return std::isfinite(health) && health > 0;
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
static bool read_target_world_bounds(uint8_t* obj, target_bar_geometry::Box& world)
{
   using namespace target_bar_geometry;
   float matrix[16];
   const float* rendered = (const float*)(obj + kGO_MatrixTrans - 0x30);
   void** vt = *(void***)obj;
   if (*(obj + kGO_Active)) {
      const float* smoothed = ((fn_smoothed_t)vt[kVt_GetSmoothedMtx / 4])(obj);
      if (smoothed) rendered = smoothed;
   }
   std::memcpy(matrix, rendered, sizeof(matrix));
   if (!valid_matrix(matrix)) return false;

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
   return true;
}

static bool project_world_anchor(const target_bar_geometry::Box& world, float out[3], bool pin)
{
   using namespace target_bar_geometry;
   const uint8_t* cam = hud_camera();
   if (!cam || !s_screenWidth || !s_screenHeight) return false;

   if (!project_anchor(world, (const float*)(cam + kCam_Matrix),
                        *(const float*)(cam + kCam_TanHalfFovW),
                        *(const float*)(cam + kCam_TanHalfFovH), out)) return false;
   return pin ? pin_to_screen(out, *s_screenWidth, *s_screenHeight, kScreenInsets, !g_hudSubPixel)
              : snap_to_screen(out, *s_screenWidth, *s_screenHeight, !g_hudSubPixel);
}

// Pinned while the engine picks the target, so a big vehicle up close whose top
// is above the screen keeps its bar. Otherwise the bar leaves with the target.
static bool project_aimed(const target_bar_geometry::Box& world, float out[3])
{
   return project_world_anchor(world, out, true);
}

static bool project_free(const target_bar_geometry::Box& world, float out[3])
{
   return project_world_anchor(world, out, false);
}

// The point distances run from: what the player controls, as the stock lock-on
// distance measures it (GameEvents::UpdateLockOn, modtools 0x006B2720), else the
// camera while dead or spectating.
static bool local_origin(float out[3])
{
   uint8_t* chr = (uint8_t*)s_localPlayer(0);
   uint8_t* controlled = nullptr;
   if (chr) {
      controlled = *(uint8_t**)(chr + layout::Character::kRemote);
      if (!controlled) controlled = *(uint8_t**)(chr + layout::Character::kVehicle);
      if (!controlled) controlled = *(uint8_t**)(chr + layout::Character::kUnit);
   }
   const uint8_t* obj = controlled ? (const uint8_t*)vcall_object(controlled + kCtrl_Trackable) : nullptr;
   const float* p = nullptr;
   if (obj && is_alive(obj)) p = (const float*)(obj + kGO_MatrixTrans);
   else if (const uint8_t* cam = hud_camera()) p = (const float*)(cam + kCam_Matrix) + 12;
   if (!p) return false;
   std::memcpy(out, p, 3 * sizeof(float));
   return hud_world_markers::finite3(out);
}

// player1.weaponN.target.distance: from what the player controls to the target
// the HUD shows, in metres, after the engine's update and so after any lend.
// FLT_MAX with no target, which a Text with InfiniteDashes prints as "--".
static void publish_distances()
{
   if (!s_playerData || !s_eventList || *s_eventList == (uintptr_t)s_eventList) {
      s_evtDistance[0] = s_evtDistance[1] = nullptr;
      s_distanceSent[0] = s_distanceSent[1] = false;
      return;
   }
   if (!has_listener(s_evtDistance[0]) && !has_listener(s_evtDistance[1])) {
      s_distanceSent[0] = s_distanceSent[1] = false;
      return;
   }
   float origin[3];
   const bool haveOrigin = local_origin(origin);
   for (int ch = 0; ch < kChannels; ++ch) {
      if (!has_listener(s_evtDistance[ch])) { s_distanceSent[ch] = false; continue; }
      const uint8_t* wd = s_playerData + ch * kPD_WeaponStride;
      const Handle shown = { *(uint8_t* const*)(wd + kWD_TargetObject),
                             *(const uint32_t*)(wd + kWD_TargetHandleId) };
      float d = FLT_MAX;
      if (haveOrigin && handle_ok(shown))
         d = (float)hud_world_markers::distance(origin, (const float*)(shown.obj + kGO_MatrixTrans));
      uint32_t bits;
      std::memcpy(&bits, &d, sizeof(bits));
      if (s_distanceSent[ch] && s_distanceBits[ch] == bits) continue;
      s_distanceSent[ch] = true;
      s_distanceBits[ch] = bits;
      struct { void* mClass; uint32_t mData; } ev = { s_evtDistance[ch], bits };
      s_eventSend(&ev, nullptr);
   }
}

static void send_position(int ch)
{
   struct { void* mClass; const float* mData; } ev = { s_evtPosition[ch], s_position[ch].value };
   s_eventSend(&ev, nullptr);
}

// ---------------------------------------------------------------------------
// The latch
// ---------------------------------------------------------------------------

static uint8_t* s_weapon[kChannels] = {};

static void clear_all_objects()
{
   s_localChr = nullptr;
   s_controlled = nullptr;
   s_owner = {};
   for (int ch = 0; ch < kChannels; ++ch) {
      s_retained[ch].reset();
      s_pair[ch].reset();
      s_natural[ch] = {};
      s_pairLent[ch] = false;
      s_weapon[ch] = nullptr;
      s_lastFocus[ch] = {};
      s_position[ch].reset();
      s_lend[ch] = {};
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

// Runs immediately BEFORE the engine's HUD update.
static void tick_before()
{
   __try {
      for (int ch = 0; ch < kChannels; ++ch) { s_natural[ch] = {}; s_pairLent[ch] = false; }
      if (!s_eventList) { clear_all_objects(); return; }

      if (*s_eventList == (uintptr_t)s_eventList) {
         s_evtPosition[0] = s_evtPosition[1] = nullptr;
         clear_all_objects();
         return;
      }
      if (!has_listener(s_evtPosition[0]) && !has_listener(s_evtPosition[1])) {
         clear_all_objects();
         return;
      }
      if (!s_announced) {
         s_announced = true;
         install_log("[TargetBarLatch] selection retention active, hold %.2fs; fade is HUD-authored",
                     (double)g_targetBarLatchSeconds);
      }
      advance_clock();

      uint8_t* chr = (uint8_t*)s_localPlayer(0);
      if (!chr) { clear_all_objects(); return; }
      uint8_t* controlled = *(uint8_t**)(chr + layout::Character::kRemote);
      if (!controlled) controlled = *(uint8_t**)(chr + layout::Character::kVehicle);
      if (!controlled) controlled = *(uint8_t**)(chr + layout::Character::kUnit);
      if (!controlled) { clear_all_objects(); return; }
      uint8_t* localObj = (uint8_t*)vcall_object(controlled + kCtrl_Trackable);
      if (!localObj || !is_alive(localObj)) { clear_all_objects(); return; }

      const Handle owner = { localObj, *(const uint32_t*)(localObj + kGO_HandleId) };
      if (chr != s_localChr || controlled != s_controlled || !same(owner, s_owner))
         clear_all_objects(); // respawn, vehicle/remote switch, reused handles
      s_localChr = chr;
      s_controlled = controlled;
      s_owner = owner;

      for (int ch = 0; ch < kChannels; ++ch) {
         if (!has_listener(s_evtPosition[ch])) {
            s_retained[ch].reset();
            s_pair[ch].reset();
            s_lastFocus[ch] = {};
            s_position[ch].reset();
            s_weapon[ch] = nullptr;
            continue;
         }

         uint8_t* weapon = nullptr;
         uint32_t* source = nullptr;
         const Handle pick = natural_target(controlled, ch, weapon, source);
         if (weapon != s_weapon[ch]) { s_retained[ch].reset(); s_pair[ch].reset(); }
         s_weapon[ch] = weapon;
         if (!weapon) { s_retained[ch].reset(); s_pair[ch].reset(); continue; }

         // A vehicle and its exposed rider trade the engine's pick back and
         // forth: keep showing the member picked first until the other holds.
         target_bar_selection::Pair& pair = s_pair[ch];
         const bool shownValid = retainable(pair.shown);
         s_natural[ch] = pair.resolve(pick, pick.obj ? display_group(pick) : nullptr, shownValid,
                                      shownValid ? display_group(pair.shown) : nullptr,
                                      s_now, kPairDwellSecs);

         const Handle held = s_retained[ch].loan(
            s_natural[ch], retainable(s_retained[ch].target), s_now);
         if (pick.obj && !same(s_natural[ch], pick)) {
            // Over the engine's pick, in the handle it was read from.
            lend(ch, source, s_natural[ch]);
            s_pairLent[ch] = true;
            continue;
         }
         if (!held.obj) {
            if (!pick.obj) pair.reset();   // nothing picked or held: the pair is over
            continue;
         }
         // Retention does not re-run acquisition LOS/frustum/range gates.
         // Native selection wins; only temporarily fill an empty reticle slot.
         uint32_t* reticule = (uint32_t*)(controlled + kCtrl_ReticuleTgt + ch * 8);
         Handle slot;
         std::memcpy(&slot, reticule, sizeof(slot));
         if (!handle_ok(slot)) lend(ch, reticule, held);
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      restore_lends();
      clear_all_objects();
   }
}

// Runs immediately AFTER the engine's HUD update.
static void tick_after()
{
   restore_lends();   // first, and outside the guard: nothing below may skip it

   __try {
      publish_distances();
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      s_distanceSent[0] = s_distanceSent[1] = false;
   }

   __try {
      if (!s_playerData || !s_localChr) return;

      Handle measured = {};
      target_bar_geometry::Box world;
      bool boundsValid = false;
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

         // Require native acceptance of the natural input. The lent HUD result
         // cannot refresh retention, and rejected/dead/stale targets clear it.
         const Handle accepted = retainable(shown) ? shown : Handle{};
         s_retained[ch].observe(s_natural[ch], accepted, s_now, g_targetBarLatchSeconds);
         // The HUD refused the pair member shown over the pick: show the
         // engine's own pick from the next tick instead of a blank bar.
         if (s_pairLent[ch] && !same(accepted, s_natural[ch])) s_pair[ch].reset();

         Handle focus = Handle{};
         bool sameTarget = true;
         if (handle_ok(shown)) {
            focus = shown;
            sameTarget = shown.obj == s_lastFocus[ch].obj && shown.id == s_lastFocus[ch].id;
            s_lastFocus[ch] = shown;
         } else if (handle_ok(s_lastFocus[ch])) {
            focus = s_lastFocus[ch];   // keep following only while still alive
         } else {
            // No object left to follow. Keep the cached world anchor for its native
            // fade, but drop the stale handle instead of reading it next tick.
            s_lastFocus[ch] = Handle{};
         }
         const bool alive = focus.obj && is_alive(focus.obj);
         if (alive) {
            if (focus.obj != measured.obj || focus.id != measured.id) {
               measured = focus;
               boundsValid = read_target_world_bounds(focus.obj, world);
            }
         }
         // Never query dead-unit bounds: death changes the collision/model pose
         // and pulled the fading bar down into the corpse. Freeze the last live
         // world anchor, but reproject it with the current camera through the fade.
         // Pinned on screen only while the engine picks this target; held or
         // fading, the bar leaves the screen with it.
         const bool aimed = s_natural[ch].obj && same(s_natural[ch], focus);
         s_position[ch].update(sameTarget, alive, alive && boundsValid ? &world : nullptr,
                               aimed ? project_aimed : project_free);
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
   hud_number_math_update();
   hud_class_icons_update();
   hud_command_posts_update();
}

static void __cdecl hooked_UpdateVoid()            // Steam, GOG: the float was dropped
{
   tick_before();
   original_UpdateVoid();
   tick_after();
   tick_horizon();
   hud_number_math_update();
   hud_class_icons_update();
   hud_command_posts_update();
}

// HUD::Manager::Open switches to GameMemory::RunTimeHeap before calling
// GameEvents::Open and restores the previous heap only after it returns, so
// creating here lands on the same heap as the stock events.  It also runs before
// any .lvl is read: Item::ReadEvent resolves names with FindByHashID only and
// never creates, so a class made any later could not be bound by a .hud at all.
static void __cdecl hooked_Open()
{
   original_Open();
   hud_number_math_open();
   hud_class_icons_open();
   hud_command_posts_open();
   hud_bar_fill_from_open();
   hud_editor_properties_open();   // after every item factory is made

   // A new mission.  Every GameObject pointer from the last one is dead memory.
   clear_all_objects();
   s_announced = false;
   s_evtPosition[0] = s_evtPosition[1] = nullptr;
   s_evtDistance[0] = s_evtDistance[1] = nullptr;
   s_distanceSent[0] = s_distanceSent[1] = false;
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
   __try {
      for (int ch = 0; ch < kChannels; ++ch) {
         char name[64];
         _snprintf_s(name, sizeof(name), _TRUNCATE, "player1.weapon%d.target.distance", ch + 1);
         void* cls = event_find(pbl_hash(name));
         if (!cls) cls = s_create(kTypeFloat, "%s", name);
         else if (*(const uint32_t*)((const uint8_t*)cls + 4) != (uint32_t)kTypeFloat) cls = nullptr;
         s_evtDistance[ch] = cls;
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      s_evtDistance[0] = s_evtDistance[1] = nullptr;
   }
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
};

static bool guard_ok(uintptr_t exe_base, const Guard& g)
{
   const uint8_t* p = (const uint8_t*)resolve(exe_base, g.va);
   const size_t   n = std::strlen(g.mask);

   for (size_t i = 0; i < n; ++i) {
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
       g_addr->hud_event_send == 0) {
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

   const Guard guards[] = {
      { "EventClass::Create",      g_addr->hud_event_class_create,
        modtools ? kCreateMt : kCreateRt, modtools ? "xxxxxxxxxxxx" : "xxxxxx????xx" },
      { "EventClass::FindByHashID", g_addr->hud_event_class_find,
        modtools ? kFindMt : kFindRt,     modtools ? "xx????xx????" : "x????xxx????" },
      { "GameEvents::Open",        g_addr->hud_game_events_open,
        modtools ? kOpenMt : kOpenRt,     modtools ? "xxxxx????xxx" : "xxxx????xxxx" },
      { "GameEvents::Update",      g_addr->hud_game_events_update,
        modtools ? kUpdateMt : kUpdateRt, "xxxxxxxxxxxx" },
      { "NetGame::GetLocalPlayer", g_addr->net_game_get_local_player,
        modtools ? kLocalMt : kLocalRt,   modtools ? "xxxxxxxxxxx?" : "xxxx????xxxx" },
   };
   for (const Guard& g : guards)
      if (!guard_ok(exe_base, g)) return;

   s_eventSend   = (fn_event_send_t)resolve(exe_base, g_addr->hud_event_send);
   s_create      = (fn_create_t)resolve(exe_base, g_addr->hud_event_class_create);
   s_localPlayer = (fn_local_player_t)resolve(exe_base, g_addr->net_game_get_local_player);
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

   if (r != NO_ERROR) {
      DetourTransactionAbort();
      install_log("[TargetBarLatch] NOT installed: DetourAttach failed (%ld)", (long)r);
      return;
   }
   s_installed = (DetourTransactionCommit() == NO_ERROR);

   install_log("[TargetBarLatch] %s (Open 0x%08X, Update 0x%08X %s, selection retention, "
               "hold %.2fs). Inert until a .hud binds player1.weaponN.target.position or .distance",
               s_installed ? "installed" : "commit failed",
               (unsigned)g_addr->hud_game_events_open, (unsigned)g_addr->hud_game_events_update,
               modtools ? "cdecl(float)" : "void(void)",
               (double)g_targetBarLatchSeconds);
   if (s_installed)
      install_log("[HudHorizon] available: EventRotation(\"%s\"); inert without a binding", kHorizonEvent);
   if (s_installed) hud_number_math_resolve(exe_base);
   if (s_installed) hud_class_icons_resolve(exe_base);
   if (s_installed) hud_command_posts_resolve(exe_base);
}

void target_bar_latch_uninstall()
{
   if (!s_installed) return;

   restore_lends();
   clear_all_objects();
   s_evtHorizon = nullptr;
   reset_horizon();

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_Open, hooked_Open);
   if (original_UpdateFloat) DetourDetach(&(PVOID&)original_UpdateFloat, hooked_UpdateFloat);
   if (original_UpdateVoid)  DetourDetach(&(PVOID&)original_UpdateVoid, hooked_UpdateVoid);
   DetourTransactionCommit();

   s_installed = false;
}
