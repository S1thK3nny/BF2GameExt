#include "pch.h"
#include "hud_command_posts.hpp"
#include "hud_command_posts_core.hpp"
#include "hud_world_markers_core.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/layout/character.hpp"
#include "core/layout/command_post.hpp"
#include "core/layout/red_camera.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <cstdio>
#include <cstring>

// =============================================================================
// What it reads, per build. Layouts and their read sites are in
// core/layout/command_post.hpp; the research is in docs/RE/HUDSystem.md.
//
//   CommandPost::sPostArray and its count   game_addrs command_post_array_ptr and
//                                           command_post_count_ptr, both pointers
//   Team::sTeams                            game_addrs team_array_base, a pointer
//   the local player's team                 layout::Character::kTeam
//
// A client only simulates a post's capture while the post is near its player:
// CommandPost::Update tests netOnClient, then NetGame::IsNearLocalPlayer
// (modtools 0x0064E3F3 and 0x0064E407, Steam and GOG 0x0047CFA3 and 0x0047CFBB).
// Elsewhere a client's timers are stale, so such a post shows at rest.
//
// A Color event (type 7) carries a pointer to the 4-byte RedColor: HUD::Event's
// RedColor constructor stores the pointer, and every EventColor handler
// dereferences it (modtools 0x00692B80, Steam 0x0054A080, GOG 0x0054ADD0).
// Icons are texture hashes (type 3), checked against the texture table as the
// class icons are, since EventBitmap does not check.
//
// The markers (position, onScreen/offScreen, direction, distance) place each
// post the way the stock Target element places objective markers; the rule is
// in hud_world_markers_core.hpp. The anchor is the stock one,
// LockOnManager::UpdateTargetVisibility (modtools 0x00454BB1): the collision
// sphere's centre, from the object's sphere stack when it has one, lifted by the
// object's up axis x 1.3 m (the 1.3f at modtools 0x00454CD7, Steam 0x0057B8AA,
// GOG 0x0057C62A). The camera is CameraManager::sInstance->mRedCamera[0], as the
// target bar reads it. Distances run from what the player controls, as the stock
// lock-on distance does (GameEvents::UpdateLockOn, modtools 0x006B2720), else
// from the camera, to the sphere's centre.
// =============================================================================

namespace {
using namespace hud_command_posts;
namespace cp = layout::CommandPost;

constexpr int kTypeBool  = 1;  // HUD::EventClass::Type
constexpr int kTypeUint  = 3;
constexpr int kTypeFloat = 4;
constexpr int kTypeColor = 7;
constexpr int kTypeVector3 = 9;

constexpr uint32_t kEC_Type          = 0x04;
constexpr uint32_t kEC_HandlerList   = 0x08;   // self-linked when nobody listens
constexpr uint32_t kGO_SphereStack   = 0x10;   // when set, the centre is at stack + 0x40 + index * 0x10
constexpr uint32_t kGO_SphereIndex   = 0x14;
constexpr uint32_t kGO_SphereCentre  = 0x18;
constexpr uint32_t kGO_MatrixUp      = 0x100;  // mMatrix (+0xF0) .up
constexpr uint32_t kGO_MatrixTrans   = 0x120;  // mMatrix (+0xF0) .trans
constexpr uint32_t kGO_Flags         = 0x1FC;  // bit 3: alive
constexpr uint32_t kGO_HandleId      = 0x204;
constexpr uint32_t kGO_Team          = 0x234;  // low 4 bits, signed
constexpr uint32_t kTextureTableSize = 0x2000;
constexpr int      kMaxPosts         = 64;     // CommandPost::sPostArray's length
constexpr int      kTeams            = layout::Team::kColorCount;
constexpr float    kMarkerLift       = 1.3f;
constexpr uint32_t kCtrl_Trackable   = 0x18;
constexpr uint32_t kVt_GetGameObject = 0x20;   // on the Trackable vptr
constexpr uint32_t kFlyerRtti        = pbl_hash("EntityFlyer");

enum Field {
   kIcon, kIconDisable, kColor, kCapture, kCaptureColor, kDisable,
   kPosition, kOnScreen, kOffScreen, kDirection, kDistance, kFieldCount
};
const char* const kFieldNames[kFieldCount] = {
   "icon", "iconDisable", "color", "capture", "captureColor", "disable",
   "position", "onScreen", "offScreen", "direction", "distance" };
constexpr int kFieldTypes[kFieldCount] = {
   kTypeUint, kTypeBool, kTypeColor, kTypeFloat, kTypeColor, kTypeBool,
   kTypeVector3, kTypeBool, kTypeBool, kTypeVector3, kTypeFloat };
constexpr char kCountName[] = "player1.commandPosts.count";

using Find        = void*(__cdecl*)(uint32_t hash);
using FindFast    = void*(__fastcall*)(uint32_t hash);
using Create      = void*(__cdecl*)(int type, const char* fmt, ...);
using Send        = void(__fastcall*)(void* ev, void* edx);
using LocalPlayer = uint8_t*(__cdecl*)(unsigned localIndex);
using TableFind   = void*(__cdecl*)(const void* table, uint32_t size, uint32_t hash);
using IsNear      = bool(__cdecl*)(const float* position);
using GameObjectOf = uint8_t*(__thiscall*)(void* self);
using IsRtti      = bool(__thiscall*)(void* self, uint32_t hash);

bool        s_active = false;
uint32_t    s_flagsOffset = cp::kFlagsModtools;
uint32_t    s_classOffset = 0;
Find        s_find = nullptr;
FindFast    s_findFast = nullptr;
Create      s_create = nullptr;
Send        s_send = nullptr;
LocalPlayer s_localPlayer = nullptr;
TableFind   s_tableFind = nullptr;
IsNear      s_isNear = nullptr;
const void* s_textureTable = nullptr;
const uintptr_t* s_eventList = nullptr;       // EventClass::sList
uint8_t** const* s_postArray = nullptr;       // each holds a pointer to its array
int* const*      s_postCount = nullptr;
uint8_t** const* s_teams = nullptr;
const uint8_t*   s_netInShell = nullptr;
const uint8_t*   s_netEnabled = nullptr;
const uint8_t*   s_netEnabledNext = nullptr;
const uint8_t*   s_netOnClient = nullptr;
const uintptr_t* s_cameraMgr = nullptr;       // CameraManager::sInstance
const uint32_t*  s_screenWidth = nullptr;
const uint32_t*  s_screenHeight = nullptr;

struct Slot {
   Sent icon, color, capture, captureColor, disabled;
   Sent x, y, onScreen, direction, distance;

   // After a disable every value is sent again, so the slot shows again.
   void invalidate_values()
   {
      icon.invalidate();
      color.invalidate();
      capture.invalidate();
      captureColor.invalidate();
      x.invalidate();
      y.invalidate();
      onScreen.invalidate();
      direction.invalidate();
      distance.invalidate();
   }
};

struct IconState {
   uint32_t hash = 0;
   bool     known = false;
   bool     loaded = false;
};

// Every pointer below dies with the mission: see hud_command_posts_open.
void*     s_countEvent = nullptr;
void*     s_events[kSlots][kFieldCount];
Sent      s_sentCount;
Slot      s_slots[kSlots];
uint32_t  s_colors[kSlots][2];   // sent by pointer, so they live here
float     s_vectors[kSlots][2][3];   // position and direction, likewise
IconState s_icons[kTeams];       // a missed texture lookup walks the whole table
bool      s_announced = false;

void invalidate()
{
   s_sentCount.invalidate();
   for (Slot& slot : s_slots) {
      slot.invalidate_values();
      slot.disabled.invalidate();
   }
   for (IconState& icon : s_icons) icon = IconState{};
}

bool guard(uintptr_t base, uintptr_t va, const char* what, const char* bytes, const char* mask)
{
   const auto* code = static_cast<const unsigned char*>(resolve(base, va));
   for (size_t i = 0; mask[i]; ++i) {
      if (mask[i] == 'x' && code[i] != static_cast<unsigned char>(bytes[i])) {
         install_log("[HudCommandPosts] not active: prologue mismatch at %s 0x%08X",
                     what, (unsigned)va);
         return false;
      }
   }
   return true;
}

void* find_event(uint32_t hash)
{
   if (s_find)     return s_find(hash);
   if (s_findFast) return s_findFast(hash);
   return nullptr;
}

void* find_or_create(int type, const char* name)
{
   void* cls = find_event(pbl_hash(name));
   if (!cls)   // literal name through "%s": Create runs its fmt through vsnprintf
      return s_create(type, "%s", name);
   if (*(const uint32_t*)((const uint8_t*)cls + kEC_Type) != (uint32_t)type) {
      install_log("[HudCommandPosts] %s already exists with another type; not published", name);
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

bool any_listener()
{
   if (has_listener(s_countEvent)) return true;
   for (auto& slot : s_events)
      for (void* cls : slot)
         if (has_listener(cls)) return true;
   return false;
}

void send(void* cls, uint32_t data)
{
   if (!cls) return;
   struct { void* mClass; uint32_t mData; } ev = { cls, data };
   s_send(&ev, nullptr);
}

void send_pointer(void* cls, const void* data)
{
   if (!cls) return;
   struct { void* mClass; const void* mData; } ev = { cls, data };
   s_send(&ev, nullptr);
}

// The engine's own test: inShell ? netEnabledNext : netEnabled, then netEnabled
// and netOnClient.
bool is_client()
{
   const bool enabled = *s_netInShell ? *s_netEnabledNext != 0 : *s_netEnabled != 0;
   return enabled && *s_netEnabled && *s_netOnClient;
}

// The post's object while its handle is current and it is alive, as the
// minimap requires before it draws a post.
uint8_t* live_object(uint8_t* post)
{
   uint8_t* obj = *(uint8_t**)(post + cp::kObject);
   if (!obj || *(const uint32_t*)(obj + kGO_HandleId) != *(const uint32_t*)(post + cp::kObject + 4))
      return nullptr;
   return (*(const uint32_t*)(obj + kGO_Flags) >> 3 & 1) ? obj : nullptr;
}

int team_of(const uint8_t* obj)
{
   int team = *(const uint32_t*)(obj + kGO_Team) & 0xF;
   if (team & 0x8) team -= 16;   // the engine's SHL 0x1C / SAR 0x1C
   return valid_team(team, kTeams);
}

const uint8_t* team_ptr(uint8_t** teams, int team)
{
   return teams ? teams[team] : nullptr;
}

// The team's SetTeamIcon texture, if it is loaded.
uint32_t team_icon(uint8_t** teams, int team)
{
   const uint8_t* t = team_ptr(teams, team);
   const uint32_t hash = t ? *(const uint32_t*)(t + layout::Team::kIcon) : 0;
   if (!hash) return 0;
   IconState& state = s_icons[team];
   if (!state.known || state.hash != hash) {
      state.hash = hash;
      state.loaded = s_tableFind(s_textureTable, kTextureTableSize, hash) != nullptr;
      state.known = true;
   }
   return state.loaded ? hash : 0;
}

uint32_t color_of(const uint8_t* viewer, int team)
{
   uint32_t color;
   std::memcpy(&color, viewer + layout::Team::kColor + team * 4, sizeof(color));
   return color;
}

void publish_color(Sent& sent, int slot, int which, Field field, uint32_t color)
{
   if (!sent.set(color)) return;
   s_colors[slot][which] = color;
   send_pointer(s_events[slot][field], &s_colors[slot][which]);
}

void publish_post(int slot, uint8_t* post, uint8_t* obj, uint8_t** teams,
                  const uint8_t* viewer, bool client)
{
   const uint8_t* cls = *(const uint8_t* const*)(post + s_classOffset);
   Capture c;
   c.team           = team_of(obj);
   c.biasTeam       = valid_team(*(const int*)(post + cp::kBiasTeam), kTeams);
   c.neutralize     = *(const float*)(post + cp::kNeutralizeTimer);
   c.capture        = *(const float*)(post + cp::kCaptureTimer);
   c.neutralizeTime = cls ? *(const float*)(cls + cp::kClassNeutralizeTime) : 0;
   c.captureTime    = cls ? *(const float*)(cls + cp::kClassCaptureTime) : 0;
   c.timersValid    = !client || s_isNear((const float*)(obj + kGO_MatrixTrans));

   Slot& s = s_slots[slot];
   const uint32_t icon = team_icon(teams, c.team);
   if (s.icon.set(icon)) {
      if (icon) send(s_events[slot][kIcon], icon);
      else      send(s_events[slot][kIconDisable], 1);
   }
   if (viewer) {
      publish_color(s.color, slot, 0, kColor, color_of(viewer, c.team));
      publish_color(s.captureColor, slot, 1, kCaptureColor, color_of(viewer, control_team(c)));
   }
   const float value = control(c);
   if (s.capture.set_float(value)) {
      uint32_t bits;
      std::memcpy(&bits, &value, sizeof(bits));
      send(s_events[slot][kCapture], bits);
   }
}

// Where markers are seen from this update: the HUD's camera, and the point
// distances run from.
struct View {
   hud_world_markers::Camera camera;
   float origin[3];
   bool  sideSlide;   // the stock rule, except in a flyer
   bool  valid;
};

View read_view(uint8_t* chr)
{
   View v{};
   const uintptr_t mgr = s_cameraMgr ? *s_cameraMgr : 0;
   const uint8_t* cam = mgr ? *(const uint8_t* const*)(mgr + layout::CameraManager::kRedCamera0) : nullptr;
   if (!cam || !s_screenWidth || !s_screenHeight) return v;
   v.camera = { (const float*)(cam + layout::RedCamera::kMatrix),
                *(const float*)(cam + layout::RedCamera::kTanHalfFovW),
                *(const float*)(cam + layout::RedCamera::kTanHalfFovH),
                *s_screenWidth, *s_screenHeight };
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

bool wants_marker(int slot)
{
   for (int f = kPosition; f <= kDistance; ++f)
      if (has_listener(s_events[slot][f])) return true;
   return false;
}

void publish_vector(Field field, int slot, int which, const float value[3])
{
   std::memcpy(s_vectors[slot][which], value, sizeof(s_vectors[slot][which]));
   send_pointer(s_events[slot][field], s_vectors[slot][which]);
}

void publish_marker(int slot, const uint8_t* obj, const View& view)
{
   const uint8_t* stack = *(const uint8_t* const*)(obj + kGO_SphereStack);
   const float* centre = stack
      ? (const float*)(stack + 0x40 + *(const int*)(obj + kGO_SphereIndex) * 0x10)
      : (const float*)(obj + kGO_SphereCentre);
   const float* up = (const float*)(obj + kGO_MatrixUp);
   float anchor[3];
   for (int a = 0; a < 3; ++a) anchor[a] = centre[a] + up[a] * kMarkerLift;
   hud_world_markers::Placement at;
   if (!hud_world_markers::finite3(centre) ||
       !hud_world_markers::place(view.camera, anchor, view.sideSlide, at)) return;

   Slot& s = s_slots[slot];
   const bool movedX = s.x.set_float(at.position[0]);
   const bool movedY = s.y.set_float(at.position[1]);
   if (movedX || movedY) publish_vector(kPosition, slot, 0, at.position);
   if (s.onScreen.set(at.onScreen ? 1 : 0))
      send(s_events[slot][at.onScreen ? kOnScreen : kOffScreen], 1);
   if (s.direction.set_float(at.rotation[2])) publish_vector(kDirection, slot, 1, at.rotation);
   const float d = (float)hud_world_markers::distance(view.origin, centre);
   if (s.distance.set_float(d)) {
      uint32_t bits;
      std::memcpy(&bits, &d, sizeof(bits));
      send(s_events[slot][kDistance], bits);
   }
}

struct Post {
   Entry    entry;
   uint8_t* post;
   uint8_t* obj;
};

} // namespace

void hud_command_posts_resolve(uintptr_t base)
{
   const bool modtools = g_build == GameBuild::Modtools;
   if (!modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;
   if (!g_addr->command_post_array_ptr || !g_addr->command_post_count_ptr ||
       !g_addr->command_post_class_off || !g_addr->team_array_base ||
       !g_addr->net_in_shell || !g_addr->net_enabled || !g_addr->net_enabled_next ||
       !g_addr->net_on_client || !g_addr->net_game_is_near_local_player ||
       !g_addr->pbl_hash_table_find || !g_addr->tex_hash_table ||
       !g_addr->hud_event_send || !g_addr->hud_event_class_list) {
      install_log("[HudCommandPosts] not active: no address set for this build");
      return;
   }
   // Create, FindByHashID and GetLocalPlayer were guarded by the shared hooks'
   // installer before this runs; the functions only this module calls are
   // guarded here.
   if (!guard(base, g_addr->net_game_is_near_local_player, "NetGame::IsNearLocalPlayer",
              modtools ? "\x55\x8B\xEC\x83\xEC\x24\xC7\x45\xFC\x00\x00\x00\x00"
                       : "\x55\x8B\xEC\x83\xEC\x2C\xC7\x45\xFC\x00\x00\x00\x00",
              "xxxxxxxxxxxxx") ||
       !guard(base, g_addr->pbl_hash_table_find, "PblHashTableCode::_Find",
              modtools ? "\x8B\x54\x24\x08\xD1\xFA\x56\x8B\x74\x24\x10\x8D"
                       : "\x55\x8B\xEC\x8B\x55\x0C\xD1\xFA\x56\x8B\x75\x10",
              "xxxxxxxxxxxx") ||
       !guard(base, g_addr->hud_event_send, "Event::Send", "\x51\x8B\x09\xE8", "xxxx"))
      return;

   s_flagsOffset = modtools ? cp::kFlagsModtools : cp::kFlagsRelease;
   s_classOffset = (uint32_t)g_addr->command_post_class_off;
   // FindByHashID takes the hash on the stack on modtools and in ECX on retail.
   void* find = resolve(base, g_addr->hud_event_class_find);
   if (modtools) s_find     = (Find)find;
   else          s_findFast = (FindFast)find;
   s_create         = (Create)resolve(base, g_addr->hud_event_class_create);
   s_send           = (Send)resolve(base, g_addr->hud_event_send);
   s_localPlayer    = (LocalPlayer)resolve(base, g_addr->net_game_get_local_player);
   s_tableFind      = (TableFind)resolve(base, g_addr->pbl_hash_table_find);
   s_isNear         = (IsNear)resolve(base, g_addr->net_game_is_near_local_player);
   s_textureTable   = resolve(base, g_addr->tex_hash_table);
   s_eventList      = (const uintptr_t*)resolve(base, g_addr->hud_event_class_list);
   s_postArray      = (uint8_t** const*)resolve(base, g_addr->command_post_array_ptr);
   s_postCount      = (int* const*)resolve(base, g_addr->command_post_count_ptr);
   s_teams          = (uint8_t** const*)resolve(base, g_addr->team_array_base);
   s_netInShell     = (const uint8_t*)resolve(base, g_addr->net_in_shell);
   s_netEnabled     = (const uint8_t*)resolve(base, g_addr->net_enabled);
   s_netEnabledNext = (const uint8_t*)resolve(base, g_addr->net_enabled_next);
   s_netOnClient    = (const uint8_t*)resolve(base, g_addr->net_on_client);
   // The markers need the camera too; the strip works without it.
   if (g_addr->camera_manager_instance && g_addr->hud_screen_width && g_addr->hud_screen_height) {
      s_cameraMgr    = (const uintptr_t*)resolve(base, g_addr->camera_manager_instance);
      s_screenWidth  = (const uint32_t*)resolve(base, g_addr->hud_screen_width);
      s_screenHeight = (const uint32_t*)resolve(base, g_addr->hud_screen_height);
   }
   s_active = true;
   install_log("[HudCommandPosts] available: player1.commandPosts.count and "
               "player1.commandPost1..16.*%s; inert without a binding",
               s_cameraMgr ? " with markers" : " (no markers: camera not addressed)");
}

// HUD::Manager::Open runs GameEvents::Open before any .lvl is read, and a .hud
// can only bind a class that already exists: create them here, finding first.
void hud_command_posts_open()
{
   if (!s_active) return;
   invalidate();
   s_announced = false;
   __try {
      s_countEvent = find_or_create(kTypeUint, kCountName);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      s_countEvent = nullptr;
   }
   for (int slot = 0; slot < kSlots; ++slot) {
      for (int f = 0; f < kFieldCount; ++f) {
         char name[64];
         _snprintf_s(name, sizeof(name), _TRUNCATE, "player1.commandPost%d.%s",
                     slot + 1, kFieldNames[f]);
         s_events[slot][f] = nullptr;
         __try {
            s_events[slot][f] = find_or_create(kFieldTypes[f], name);
         } __except (EXCEPTION_EXECUTE_HANDLER) {
            s_events[slot][f] = nullptr;
         }
      }
   }
}

void hud_command_posts_update()
{
   if (!s_active) return;
   __try {
      if (!s_eventList || *s_eventList == (uintptr_t)s_eventList) {
         // The HUD has been torn down.
         s_countEvent = nullptr;
         for (auto& slot : s_events)
            for (void*& cls : slot) cls = nullptr;
         return;
      }
      if (!any_listener()) return;
      if (!s_announced) {
         s_announced = true;
         install_log("[HudCommandPosts] publishing: a .hud binds a command post event");
      }

      uint8_t** array = *s_postArray;
      const int* countPtr = *s_postCount;
      int count = array && countPtr ? *countPtr : 0;
      if (count < 0) count = 0;
      if (count > kMaxPosts) count = kMaxPosts;

      Post posts[kMaxPosts];
      int shown = 0;
      for (int i = 0; i < count; ++i) {
         uint8_t* post = array[i];
         uint8_t* obj = post ? live_object(post) : nullptr;
         if (!obj || !(post[s_flagsOffset] & cp::kFlagHudIndexDisplay)) continue;
         posts[shown++] = { { *(const int*)(post + cp::kHudIndex), i }, post, obj };
      }
      order(posts, shown, &Post::entry);
      if (shown > kSlots) shown = kSlots;
      if (s_sentCount.set((uint32_t)shown)) send(s_countEvent, (uint32_t)shown);

      uint8_t** teams = *s_teams;
      uint8_t* chr = s_localPlayer(0);
      const uint8_t* viewer = chr ? team_ptr(teams, valid_team(
         *(const int*)(chr + layout::Character::kTeam), kTeams)) : nullptr;
      const bool client = is_client();
      const View view = read_view(chr);

      for (int slot = 0; slot < kSlots; ++slot) {
         Slot& s = s_slots[slot];
         if (slot >= shown) {
            if (s.disabled.set(1)) {
               // Its marker leaves the screen with it, so one EventDisable on
               // offScreen hides a marker either way: an element binds only one.
               send(s_events[slot][kDisable], 1);
               send(s_events[slot][kOffScreen], 1);
               s.invalidate_values();
            }
            continue;
         }
         s.disabled.invalidate();
         publish_post(slot, posts[slot].post, posts[slot].obj, teams, viewer, client);
         if (view.valid && wants_marker(slot)) publish_marker(slot, posts[slot].obj, view);
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A torn-down post must not escape into the game's HUD update.
      invalidate();
   }
}
