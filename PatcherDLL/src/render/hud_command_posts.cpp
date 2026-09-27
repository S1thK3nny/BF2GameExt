#include "pch.h"
#include "hud_command_posts.hpp"
#include "hud_command_posts_core.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/layout/character.hpp"
#include "core/layout/command_post.hpp"
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
// =============================================================================

namespace {
using namespace hud_command_posts;
namespace cp = layout::CommandPost;

constexpr int kTypeBool  = 1;  // HUD::EventClass::Type
constexpr int kTypeUint  = 3;
constexpr int kTypeFloat = 4;
constexpr int kTypeColor = 7;

constexpr uint32_t kEC_Type          = 0x04;
constexpr uint32_t kEC_HandlerList   = 0x08;   // self-linked when nobody listens
constexpr uint32_t kGO_MatrixTrans   = 0x120;  // mMatrix (+0xF0) .trans
constexpr uint32_t kGO_Flags         = 0x1FC;  // bit 3: alive
constexpr uint32_t kGO_HandleId      = 0x204;
constexpr uint32_t kGO_Team          = 0x234;  // low 4 bits, signed
constexpr uint32_t kTextureTableSize = 0x2000;
constexpr int      kMaxPosts         = 64;     // CommandPost::sPostArray's length
constexpr int      kTeams            = layout::Team::kColorCount;

enum Field { kIcon, kIconDisable, kColor, kCapture, kCaptureColor, kDisable, kFieldCount };
const char* const kFieldNames[kFieldCount] = {
   "icon", "iconDisable", "color", "capture", "captureColor", "disable" };
constexpr int kFieldTypes[kFieldCount] = {
   kTypeUint, kTypeBool, kTypeColor, kTypeFloat, kTypeColor, kTypeBool };
constexpr char kCountName[] = "player1.commandPosts.count";

using Find        = void*(__cdecl*)(uint32_t hash);
using FindFast    = void*(__fastcall*)(uint32_t hash);
using Create      = void*(__cdecl*)(int type, const char* fmt, ...);
using Send        = void(__fastcall*)(void* ev, void* edx);
using LocalPlayer = uint8_t*(__cdecl*)(unsigned localIndex);
using TableFind   = void*(__cdecl*)(const void* table, uint32_t size, uint32_t hash);
using IsNear      = bool(__cdecl*)(const float* position);

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

struct Slot {
   Sent icon, color, capture, captureColor, disabled;

   // After a disable every value is sent again, so the slot shows again.
   void invalidate_values()
   {
      icon.invalidate();
      color.invalidate();
      capture.invalidate();
      captureColor.invalidate();
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
   s_active = true;
   install_log("[HudCommandPosts] available: player1.commandPosts.count and "
               "player1.commandPost1..16.*; inert without a binding");
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
      const uint8_t* chr = s_localPlayer(0);
      const uint8_t* viewer = chr ? team_ptr(teams, valid_team(
         *(const int*)(chr + layout::Character::kTeam), kTeams)) : nullptr;
      const bool client = is_client();

      for (int slot = 0; slot < kSlots; ++slot) {
         Slot& s = s_slots[slot];
         if (slot >= shown) {
            if (s.disabled.set(1)) {
               send(s_events[slot][kDisable], 1);
               s.invalidate_values();
            }
            continue;
         }
         s.disabled.invalidate();
         publish_post(slot, posts[slot].post, posts[slot].obj, teams, viewer, client);
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A torn-down post must not escape into the game's HUD update.
      invalidate();
   }
}
