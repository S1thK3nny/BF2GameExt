#include "pch.h"
#include "hud_wingmen.hpp"
#include "hud_command_posts_core.hpp"   // Sent
#include "hud_sub_pixel.hpp"
#include "hud_world_markers_core.hpp"
#include "ai/flyer_wingmen.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "game/Battlefront2/Source/CameraManager.h"
#include "game/Battlefront2/Source/Character.h"
#include "game/Battlefront2/Source/RedCamera.h"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <cstdio>
#include <cstring>

// =============================================================================
// What it reads (modtools; the events and the camera as hud_command_posts.cpp
// reads them, the anchor as the stock Target element's,
// LockOnManager::UpdateTargetVisibility 0x00454BB1, and health as camera shake
// reads it, Damageable::mCurHealth and mMaxHealth at GameObject +0x144 and
// +0x148). The wingmen and their target come from flyer_wingmen.hpp.
// =============================================================================

namespace {
using hud_command_posts::Sent;

constexpr int kTypeBool    = 1;   // HUD::EventClass::Type
constexpr int kTypeUint    = 3;
constexpr int kTypeFloat   = 4;
constexpr int kTypeVector3 = 9;

constexpr uint32_t kEC_Type          = 0x04;
constexpr uint32_t kEC_HandlerList   = 0x08;   // self-linked when nobody listens
constexpr uint32_t kGO_SphereStack   = 0x10;   // when set, the centre is at stack + 0x40 + index * 0x10
constexpr uint32_t kGO_SphereIndex   = 0x14;
constexpr uint32_t kGO_SphereCentre  = 0x18;
constexpr uint32_t kGO_MatrixUp      = 0x100;
constexpr uint32_t kGO_MatrixTrans   = 0x120;
constexpr uint32_t kGO_Health        = 0x144;
constexpr uint32_t kGO_MaxHealth     = 0x148;
constexpr uint32_t kGO_Flags         = 0x1FC;  // bit 3: alive
constexpr float    kMarkerLift       = 1.3f;
constexpr uint32_t kCtrl_Trackable   = 0x18;
constexpr uint32_t kVt_GetGameObject = 0x20;   // on the Trackable vptr
constexpr uint32_t kFlyerRtti        = pbl_hash("EntityFlyer");
constexpr int      kSlots            = 8;      // flyer_wingmen::kMaxWingmen

enum Field { kPosition, kOnScreen, kOffScreen, kDirection, kDistance, kDisable, kHealth, kFieldCount };
const char* const kFieldNames[kFieldCount] = {"position", "onScreen", "offScreen", "direction",
                                              "distance", "disable",  "health"};
constexpr int kFieldTypes[kFieldCount] = {kTypeVector3, kTypeBool, kTypeBool, kTypeVector3,
                                          kTypeFloat,   kTypeBool, kTypeFloat};
constexpr int kTarget = kSlots;   // the attack target's row, without health
constexpr char kCountName[] = "player1.wingmen.count";

using Find        = void*(__cdecl*)(uint32_t hash);
using Create      = void*(__cdecl*)(int type, const char* fmt, ...);
using Send        = void(__fastcall*)(void* ev, void* edx);
using LocalPlayer = uint8_t*(__cdecl*)(unsigned localIndex);
using GameObjectOf = uint8_t*(__thiscall*)(void* self);
using IsRtti      = bool(__thiscall*)(void* self, uint32_t hash);

bool        s_active = false;
Find        s_find = nullptr;
Create      s_create = nullptr;
Send        s_send = nullptr;
LocalPlayer s_localPlayer = nullptr;
const uintptr_t* s_eventList = nullptr;   // EventClass::sList
const uintptr_t* s_cameraMgr = nullptr;   // CameraManager::sInstance
const uint32_t*  s_screenWidth = nullptr;
const uint32_t*  s_screenHeight = nullptr;

struct Row {
   Sent x, y, onScreen, direction, distance, health, disabled;

   void invalidate_values()
   {
      x.invalidate();
      y.invalidate();
      onScreen.invalidate();
      direction.invalidate();
      distance.invalidate();
      health.invalidate();
   }
};

// Every pointer below dies with the mission: see hud_wingmen_open.
void* s_countEvent = nullptr;
void* s_events[kSlots + 1][kFieldCount];
Sent  s_sentCount;
Row   s_rows[kSlots + 1];
float s_vectors[kSlots + 1][2][3];   // position and direction, sent by pointer
bool  s_announced = false;

void invalidate()
{
   s_sentCount.invalidate();
   for (Row& r : s_rows) {
      r.invalidate_values();
      r.disabled.invalidate();
   }
}

void* find_or_create(int type, const char* name)
{
   void* cls = s_find ? s_find(pbl_hash(name)) : nullptr;
   if (!cls) return s_create(type, "%s", name);   // literal name through "%s"
   if (*(const uint32_t*)((const uint8_t*)cls + kEC_Type) != (uint32_t)type) {
      install_log("[HudWingmen] %s already exists with another type; not published", name);
      return nullptr;
   }
   return cls;
}

bool has_listener(const void* cls)
{
   if (!cls) return false;
   const uintptr_t node = (uintptr_t)cls + kEC_HandlerList;
   return *(const uintptr_t*)node != node;
}

bool row_bound(int row)
{
   for (void* cls : s_events[row])
      if (has_listener(cls)) return true;
   return false;
}

bool wingmen_bound()
{
   if (has_listener(s_countEvent)) return true;
   for (int r = 0; r < kSlots; ++r)
      if (row_bound(r)) return true;
   return false;
}

bool hud_up() { return s_active && s_eventList && *s_eventList != (uintptr_t)s_eventList; }

void send(void* cls, uint32_t data)
{
   if (!cls) return;
   struct { void* mClass; uint32_t mData; } ev = {cls, data};
   s_send(&ev, nullptr);
}

void send_pointer(void* cls, const void* data)
{
   if (!cls) return;
   struct { void* mClass; const void* mData; } ev = {cls, data};
   s_send(&ev, nullptr);
}

void send_float(void* cls, float v)
{
   uint32_t bits;
   std::memcpy(&bits, &v, sizeof(bits));
   send(cls, bits);
}

// Where markers are seen from this update, as hud_command_posts.cpp has it.
struct View {
   hud_world_markers::Camera camera;
   float origin[3];
   bool  sideSlide;
   bool  valid;
};

View read_view(uint8_t* chr)
{
   View v{};
   const uintptr_t mgr = s_cameraMgr ? *s_cameraMgr : 0;
   const uint8_t* cam = mgr ? *(const uint8_t* const*)(mgr + layout::CameraManager::kRedCamera0) : nullptr;
   if (!cam || !s_screenWidth || !s_screenHeight) return v;
   v.camera = {(const float*)(cam + layout::RedCamera::kMatrix), *(const float*)(cam + layout::RedCamera::kTanHalfFovW),
               *(const float*)(cam + layout::RedCamera::kTanHalfFovH), *s_screenWidth, *s_screenHeight};
   std::memcpy(v.origin, v.camera.matrix + 12, sizeof(v.origin));
   v.sideSlide = true;
   uint8_t* controlled = nullptr;
   if (chr) {
      controlled = *(uint8_t**)(chr + layout::Character::kRemote);
      if (!controlled) controlled = *(uint8_t**)(chr + layout::Character::kVehicle);
      if (!controlled) controlled = *(uint8_t**)(chr + layout::Character::kUnit);
   }
   if (controlled) {
      void* trackable = controlled + kCtrl_Trackable;
      uint8_t* obj = ((GameObjectOf)(*(void***)trackable)[kVt_GetGameObject / 4])(trackable);
      if (obj && (*(const uint32_t*)(obj + kGO_Flags) >> 3 & 1)) {
         std::memcpy(v.origin, obj + kGO_MatrixTrans, sizeof(v.origin));
         v.sideSlide = !((IsRtti)(*(void***)obj)[0])(obj, kFlyerRtti);
      }
   }
   v.valid = hud_world_markers::finite3(v.origin);
   return v;
}

void publish_vector(Field field, int row, int which, const float value[3])
{
   std::memcpy(s_vectors[row][which], value, sizeof(s_vectors[row][which]));
   send_pointer(s_events[row][field], s_vectors[row][which]);
}

void publish(int row, const uint8_t* obj, const View& view)
{
   Row& r = s_rows[row];
   r.disabled.invalidate();
   if (row < kSlots) {
      const float cur = *(const float*)(obj + kGO_Health), max = *(const float*)(obj + kGO_MaxHealth);
      const float health = std::isfinite(cur) && std::isfinite(max) && max > 0.0f
                              ? (cur <= 0.0f ? 0.0f : cur >= max ? 1.0f : cur / max)
                              : 1.0f;
      if (r.health.set_float(health)) send_float(s_events[row][kHealth], health);
   }
   if (!view.valid) return;
   const uint8_t* stack = *(const uint8_t* const*)(obj + kGO_SphereStack);
   const float* centre = stack ? (const float*)(stack + 0x40 + *(const int*)(obj + kGO_SphereIndex) * 0x10)
                               : (const float*)(obj + kGO_SphereCentre);
   const float* up = (const float*)(obj + kGO_MatrixUp);
   float anchor[3];
   for (int a = 0; a < 3; ++a) anchor[a] = centre[a] + up[a] * kMarkerLift;
   hud_world_markers::Placement at;
   if (!hud_world_markers::finite3(centre) ||
       !hud_world_markers::place(view.camera, anchor, view.sideSlide, at, !g_hudSubPixel))
      return;
   const bool movedX = r.x.set_float(at.position[0]);
   const bool movedY = r.y.set_float(at.position[1]);
   if (movedX || movedY) publish_vector(kPosition, row, 0, at.position);
   if (r.onScreen.set(at.onScreen ? 1 : 0)) send(s_events[row][at.onScreen ? kOnScreen : kOffScreen], 1);
   if (r.direction.set_float(at.rotation[2])) publish_vector(kDirection, row, 1, at.rotation);
   const float d = (float)hud_world_markers::distance(view.origin, centre);
   if (r.distance.set_float(d)) send_float(s_events[row][kDistance], d);
}

// An empty slot, or no target: one EventDisable on offScreen hides a marker either way.
void disable(int row)
{
   Row& r = s_rows[row];
   if (!r.disabled.set(1)) return;
   send(s_events[row][kDisable], 1);
   send(s_events[row][kOffScreen], 1);
   r.invalidate_values();
}

} // namespace

bool hud_wingmen_bound()
{
   __try {
      return hud_up() && wingmen_bound();
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

bool hud_wingmen_target_bound()
{
   __try {
      return hud_up() && row_bound(kTarget);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

void hud_wingmen_resolve(uintptr_t base)
{
   if (g_build != GameBuild::Modtools) return;   // the wingmen run on modtools only
   const auto* a = g_addr;
   if (!a->hud_event_class_find || !a->hud_event_class_create || !a->hud_event_send || !a->hud_event_class_list ||
       !a->net_game_get_local_player || !a->camera_manager_instance || !a->hud_screen_width ||
       !a->hud_screen_height) {
      install_log("[HudWingmen] not active: no address set for this build");
      return;
   }
   if (std::memcmp(resolve(base, a->hud_event_send), "\x51\x8B\x09\xE8", 4) != 0) {
      install_log("[HudWingmen] not active: Event::Send differs");
      return;
   }
   s_find = (Find)resolve(base, a->hud_event_class_find);
   s_create = (Create)resolve(base, a->hud_event_class_create);
   s_send = (Send)resolve(base, a->hud_event_send);
   s_localPlayer = (LocalPlayer)resolve(base, a->net_game_get_local_player);
   s_eventList = (const uintptr_t*)resolve(base, a->hud_event_class_list);
   s_cameraMgr = (const uintptr_t*)resolve(base, a->camera_manager_instance);
   s_screenWidth = (const uint32_t*)resolve(base, a->hud_screen_width);
   s_screenHeight = (const uint32_t*)resolve(base, a->hud_screen_height);
   s_active = true;
   install_log("[HudWingmen] available: player1.wingmen.count, player1.wingman1..8.* and player1.wingmen.target.*; "
               "while a .hud binds none, the wingmen get the game's own markers");
}

// As hud_command_posts_open: made at HUD::Manager::Open, before any .hud is read.
void hud_wingmen_open()
{
   if (!s_active) return;
   invalidate();
   s_announced = false;
   __try {
      s_countEvent = find_or_create(kTypeUint, kCountName);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      s_countEvent = nullptr;
   }
   for (int row = 0; row <= kSlots; ++row)
      for (int f = 0; f < kFieldCount; ++f) {
         s_events[row][f] = nullptr;
         if (row == kTarget && f == kHealth) continue;
         char name[64];
         if (row == kTarget)
            _snprintf_s(name, sizeof(name), _TRUNCATE, "player1.wingmen.target.%s", kFieldNames[f]);
         else
            _snprintf_s(name, sizeof(name), _TRUNCATE, "player1.wingman%d.%s", row + 1, kFieldNames[f]);
         __try {
            s_events[row][f] = find_or_create(kFieldTypes[f], name);
         } __except (EXCEPTION_EXECUTE_HANDLER) {
            s_events[row][f] = nullptr;
         }
      }
}

void hud_wingmen_update()
{
   if (!s_active) return;
   __try {
      if (!hud_up()) {
         s_countEvent = nullptr;
         for (auto& row : s_events)
            for (void*& cls : row) cls = nullptr;
         return;
      }
      const bool wingmen = wingmen_bound(), target = row_bound(kTarget);
      if (!wingmen && !target) return;
      if (!s_announced) {
         s_announced = true;
         install_log("[HudWingmen] publishing: a .hud binds the %s", wingmen && target ? "wingmen and their target"
                                                                     : wingmen         ? "wingmen"
                                                                                       : "wingmen's target");
      }
      const View view = read_view(s_localPlayer(0));
      if (wingmen) {
         uint8_t* objs[kSlots] = {};
         const int n = flyer_wingmen_hud_list(objs, kSlots);
         if (s_sentCount.set((uint32_t)n)) send(s_countEvent, (uint32_t)n);
         for (int row = 0; row < kSlots; ++row)
            if (row < n) publish(row, objs[row], view);
            else disable(row);
      }
      if (target) {
         if (uint8_t* t = flyer_wingmen_hud_target()) publish(kTarget, t, view);
         else disable(kTarget);
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A wingman torn down mid-read must not escape into the game's HUD update.
      invalidate();
   }
}
