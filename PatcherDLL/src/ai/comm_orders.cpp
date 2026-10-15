#include "pch.h"
#include "comm_orders.hpp"
#include "comm_orders_core.hpp"
#include "ai_goals.hpp"
#include "squad_formation.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "game/Battlefront2/Source/Character.h"
#include "game/Battlefront2/Source/CollisionObject.h"
#include "render/hud_wingmen.hpp"
#include "util/install_log.hpp"

#include <detours.h>

#include <cmath>
#include <cstdio>
#include <cstring>

// See comm_orders.hpp for the behaviour.

bool g_commKeyOrders = true;

namespace {

using comm_orders::Aim;
using comm_orders::Ask;

// ---- Modtools -----------------------------------------------------------------
// A Controllable (what a character controls: Character mUnit, mVehicle, mRemote)
constexpr uint32_t kCtl_Trackable  = 0x18;    // its Trackable, whose vptr +0x20 gives the GameObject
constexpr uint32_t kVt_GetObject   = 0x20;
constexpr uint32_t kCtl_Ctrl       = 0xC8;    // Controllable::mCtrl (an EntityFlyer's +0x308)
constexpr uint32_t kCtl_EyePoint   = 0xDC;    // mEyePoint
constexpr uint32_t kCtl_EyeDir     = 0xE8;    // mEyeDir
constexpr uint32_t kCtl_AimStart   = 0x148;   // mTargetInfo.mAimStart
// A PlayerController's processed inputs, as the flyer wingmen read them
constexpr uint32_t kPC_Inputs      = 0x88;
constexpr uint32_t kInputs_Bits    = 0x10;
// A Character
constexpr uint32_t kChr_PlayerId   = 0x130;   // AI below 0
// What the crosshair is on, as the game's own auto-aim picks it: mReticuleTarget,
// an object and its handle for each weapon channel (8 bytes each)
constexpr uint32_t kCtl_Reticule   = 0x164;
constexpr int      kReticuleChannels = 2;
// A GameObject
constexpr uint32_t kVt_IsRtti      = 0x00;
constexpr uint32_t kGO_Class       = 0x08;    // its class, whose vptr +0x24 is GetSoldierType
constexpr uint32_t kVt_SoldierType = 0x24;
constexpr uint32_t kGO_Health      = 0x144;   // Damageable::mCurHealth ...
constexpr uint32_t kGO_MaxHealth   = 0x148;   // ... and mMaxHealth
constexpr uint32_t kGO_MatrixUp    = 0x100;
constexpr uint32_t kGO_MatrixTrans = 0x120;
constexpr uint32_t kGO_Flags       = 0x1FC;   // bit 3: alive; bits 0 to 2: its health type (person 0, vehicle 3 ...)
constexpr uint32_t kHealthTypeBits = 0x7;
constexpr uint32_t kGO_HandleId    = 0x204;
constexpr uint32_t kGO_Team        = 0x234;   // the low 4 bits, signed
constexpr uint32_t kRttiFlyer           = pbl_hash("EntityFlyer");
constexpr uint32_t kRttiHover           = pbl_hash("EntityHover");
constexpr uint32_t kRttiPlayerController = pbl_hash("PlayerController");
constexpr uint32_t kRttiUnitController   = pbl_hash("UnitController");
constexpr uint32_t kRttiSoldier          = pbl_hash("EntitySoldier");
// A UnitController's agent; a RepairHelper's (0x30 bytes) +4 is its agent
constexpr uint32_t kUC_Agent         = 0x2C0;
constexpr size_t   kRepairHelperSize = 0x30;
constexpr uint32_t kRH_Agent         = 0x04;
// NetGame::DisplayMessage's NG_ChatMsg (modtools: 0x128 bytes), and the live
// requests of TeamAssistManager (game_addrs.hpp)
constexpr size_t   kChatMsgSize = 0x128;
constexpr uint32_t kMsg_Id      = 0x00;
constexpr uint32_t kMsg_Flag    = 0x04;   // 1: to the sender's team, as the comm keys send it
constexpr uint32_t kMsg_Code    = 0x10;
constexpr uint32_t kTA_List     = 0x18;   // TeamAssistManager::mList; a TeamAssist's next at +0
constexpr int      kMaxLiveAssists = 48;  // of the pool's 64, which must never fill
constexpr double   kStockEvery  = 0.25;   // s between stock requests
// AIGoalManager::AddGoal's GoalTarget, and the goal types (P: AIGoalType,
// GoalTargetType)
constexpr int kGoalDestroy = 2;
constexpr int kGoalDefend  = 3;
constexpr int kGoalFollow  = 7;
constexpr int kGttPos       = 1;
constexpr int kGttCharacter = 2;
constexpr int kGttObject    = 3;
struct GoalTarget {
   int      type = 0;
   float    pos[3] = {};
   uint8_t* chr = nullptr;
   uint8_t* obj = nullptr;
   uint32_t handle = 0;
};
static_assert(sizeof(GoalTarget) == 0x1C, "GoalTarget");
// The squad commands (ai_squads.cpp, squad_formation.cpp)
constexpr int kEvqIsFollowing = 0x0F;   // EVQ_SC_IsFollowingChr
constexpr int kEvtFollowChr   = 0x10;   // EVT_SC_FollowChr, the squad key's recruit
// The crosshair's ray: what the engine's own search may pick (soldiers,
// vehicles, buildings and props, attached objects) and the ground; water left
// out (target_bar_latch.cpp's kAimRayMask). A soldier's sight: buildings, props,
// the ground (the landing ray's 0x190 less water).
constexpr int   kAimRayMask   = 0x02 | 0x08 | 0x10 | 0x20 | 0x80;
constexpr int   kSightMask    = 0x10 | 0x80;
constexpr float kAimReach     = 600.0f;   // m along the crosshair
constexpr float kSightShort   = 2.0f;     // m short of the target its sight stops
constexpr float kEyeHeight    = 1.6f;     // m over a unit's position, for a vehicle's crew
constexpr int   kMaxCharacters = 256;
constexpr int   kMaxRequests   = comm_orders::kCodes;

using fn_local_t     = uint8_t*(__cdecl*)(unsigned localIndex);
using fn_is_rtti_t   = bool(__thiscall*)(void* self, uint32_t hash);
using fn_get_obj_t   = uint8_t*(__thiscall*)(void* trackable);
using fn_ray_hit_t   = float(__cdecl*)(const float* start, const float* dir, float length, void** hit, float* normal,
                                      uint8_t** ignore, int ignoreCount, int typeMask, int ordnanceOnly);
using fn_coll_obj_t  = uint8_t*(__thiscall*)(void* collision);
using fn_affiliation_t = int(__thiscall*)(void* me, void* other);
using fn_force_t     = void(__thiscall*)(void* uc, void* target);
using fn_add_goal_t  = uint8_t*(__thiscall*)(void* mgr, uint8_t team, int type, float weight, const GoalTarget* target);
using fn_delete_goal_t = void(__thiscall*)(void* mgr, void* goal);
using fn_unassign_t  = void(__thiscall*)(void* goal);
using fn_message_t   = void(__cdecl*)(int player, const wchar_t* text, const uint8_t* color);
using fn_localize_t  = const wchar_t*(__cdecl*)(uint32_t hash, bool nullIfMissing);
using fn_add_marker_t = void(__cdecl*)(void* obj, void* region, uint32_t icon, float scale, uint32_t teams,
                                       uint32_t color, uint32_t hudTarget, uint32_t flash, uint32_t pulse,
                                       float pulseFrom);
using fn_remove_marker_t = void(__cdecl*)(void* obj, void* region, uint32_t teams);
using fn_cm_update_t = void(__cdecl*)(float dt);
using fn_display_t   = void(__cdecl*)(void* chatMsg);
using fn_request_t   = void(__cdecl*)(int code, void* chr);
using fn_aid_types_t = void(__thiscall*)(void* repairHelper, uint32_t* healthMask, uint32_t* ammoMask);
using fn_class_type_t = int(__thiscall*)(void* cls);

bool            s_installed = false;
fn_cm_update_t  s_cmUpdate = nullptr;      // ControllerManager::Update, detoured
fn_local_t      s_local = nullptr;
fn_ray_hit_t    s_rayHit = nullptr;
fn_affiliation_t s_affiliation = nullptr;
fn_force_t      s_forceTarget = nullptr;
fn_add_goal_t   s_addGoal = nullptr;
fn_delete_goal_t s_deleteGoal = nullptr;   // through ai_goals' detour, which drops the keeps
fn_unassign_t   s_unassignAll = nullptr;   // AIGoal::UnAssignAll
uint8_t* const* s_goalManager = nullptr;   // AIGoalManager::mInstance
fn_message_t    s_message = nullptr;
const uint8_t*  s_messageColor = nullptr;
fn_localize_t   s_localize = nullptr;
fn_add_marker_t s_addMarker = nullptr;
fn_remove_marker_t s_removeMarker = nullptr;
fn_display_t    s_displayMessage = nullptr;   // NetGame::DisplayMessage
fn_request_t    s_addRequest = nullptr;       // TeamAssistManager::AddRequest
uint8_t* const* s_teamAssist = nullptr;       // TeamAssistManager::sInstance
fn_aid_types_t  s_aidTypes = nullptr;         // RepairHelper::CalcMyAidTypes
double          s_stockAt = -1e9;
const uint8_t*  s_netInShell = nullptr;
const uint8_t*  s_netEnabled = nullptr;
const uint8_t*  s_netEnabledNext = nullptr;
double          s_ticksPerSecond = 1.0;
uint32_t        s_lastBits = 0;
int             s_logs = 0;
constexpr int   kMaxLogs = 300;

double seconds()
{
   LARGE_INTEGER t;
   QueryPerformanceCounter(&t);
   return (double)t.QuadPart / s_ticksPerSecond;
}

bool log_room() { return s_logs < kMaxLogs && ++s_logs; }

bool is_rtti(void* obj, uint32_t hash) { return ((fn_is_rtti_t)(*(void***)obj)[kVt_IsRtti / 4])(obj, hash); }
bool alive(const uint8_t* obj) { return ((*(const uint32_t*)(obj + kGO_Flags)) >> 3) & 1u; }
uint32_t handle_of(const uint8_t* obj) { return *(const uint32_t*)(obj + kGO_HandleId); }
int team_of(const uint8_t* obj) { return (int)((int8_t)(*(const uint8_t*)(obj + kGO_Team) << 4) >> 4); }

uint8_t* object_of(uint8_t* controllable)
{
   void* const trackable = controllable + kCtl_Trackable;
   return ((fn_get_obj_t)(*(void***)trackable)[kVt_GetObject / 4])(trackable);
}

// What a character controls now: a remote unit, else a vehicle, else itself.
uint8_t* controlled(uint8_t* chr)
{
   uint8_t* c = *(uint8_t**)(chr + layout::Character::kRemote);
   if (!c) c = *(uint8_t**)(chr + layout::Character::kVehicle);
   if (!c) c = *(uint8_t**)(chr + layout::Character::kUnit);
   return c;
}

struct Pos { float x = 0.0f, y = 0.0f, z = 0.0f; };
Pos position_of(const uint8_t* obj)
{
   const float* t = (const float*)(obj + kGO_MatrixTrans);
   return {t[0], t[1], t[2]};
}
float distance(Pos a, Pos b)
{
   const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
   return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool networked()
{
   if (!s_netInShell || !s_netEnabled || !s_netEnabledNext) return true;   // unknown: hands off
   return *s_netInShell ? *s_netEnabledNext != 0 : *s_netEnabled != 0;
}

// ---- The characters (Character::sCharacters) ------------------------------------
const uint8_t* characters(int& count)
{
   count = 0;
   if (!g_addr->char_array_base || !g_addr->max_chars) return nullptr;
   const uint8_t* const chars = *(const uint8_t* const*)resolve(g_addr->char_array_base);
   count = chars ? *(const int*)resolve(g_addr->max_chars) : 0;
   if (count < 0 || count > kMaxCharacters) count = 0;
   return chars;
}

uint8_t* character(int index)
{
   int count = 0;
   const uint8_t* const chars = characters(count);
   return chars && index >= 0 && index < count ? (uint8_t*)chars + (size_t)index * layout::Character::kSize : nullptr;
}

int character_index(const uint8_t* chr)
{
   int count = 0;
   const uint8_t* const chars = characters(count);
   if (!chars || chr < chars) return -1;
   const size_t offset = (size_t)(chr - chars);
   const int index = (int)(offset / layout::Character::kSize);
   return offset % layout::Character::kSize == 0 && index < count ? index : -1;
}

// The character whose unit, vehicle or remote unit an object is, or null.
uint8_t* character_of(const uint8_t* obj)
{
   int count = 0;
   const uint8_t* const chars = characters(count);
   for (int i = 0; chars && i < count; ++i) {
      uint8_t* const chr = (uint8_t*)chars + (size_t)i * layout::Character::kSize;
      for (const uint32_t at : {layout::Character::kUnit, layout::Character::kVehicle, layout::Character::kRemote}) {
         uint8_t* const c = *(uint8_t**)(chr + at);
         if (c && object_of(c) == obj) return chr;
      }
   }
   return nullptr;
}

// ---- The player ---------------------------------------------------------------
struct Player {
   uint8_t* chr = nullptr;
   uint8_t* ctl = nullptr;       // what it controls
   uint8_t* obj = nullptr;       // ... as a GameObject
   uint8_t* soldier = nullptr;   // its soldier (mUnit), in a vehicle too
   int      team = -1;
};

bool player(Player& p)
{
   p.chr = s_local ? s_local(0) : nullptr;
   if (!p.chr) return false;
   p.ctl = controlled(p.chr);
   p.obj = p.ctl ? object_of(p.ctl) : nullptr;
   if (!p.obj || !alive(p.obj)) return false;
   p.team = *(const int*)(p.chr + layout::Character::kTeam);
   uint8_t* const unit = *(uint8_t**)(p.chr + layout::Character::kUnit);
   p.soldier = unit ? object_of(unit) : nullptr;
   return true;
}

uint32_t comm_bits(const Player& p)
{
   uint8_t* const ctrl = *(uint8_t**)(p.ctl + kCtl_Ctrl);
   if (!ctrl || !is_rtti(ctrl, kRttiPlayerController)) return 0;
   const uint8_t* const inputs = *(const uint8_t* const*)(ctrl + kPC_Inputs);
   return inputs ? *(const uint32_t*)(inputs + kInputs_Bits) : 0;
}

// What the crosshair is on, for a key: the first thing along the line the
// crosshair is on (as target_bar_latch.cpp's aimed_unit finds it: the eye's
// line in unzoomed first person, else from mAimStart), out to kAimReach, and the
// unit the game's own auto-aim marks (auto_aimed). An object of another side is
// an enemy, of the player's a friend; the ground, a building or a prop is a
// place, with how far up it faces. Spotted and Attack take the enemy the
// auto-aim marks before the line's; Defend a friend on the line before one the
// auto-aim marks (an enemy in the lock-on cone does not stop it holding a place).
struct Aimed {
   Aim      aim = Aim::None;
   uint8_t* obj = nullptr;
   Pos      at;
   float    normalUp = 0.0f;
   float    dist = 0.0f;
};

// The unit or object the game's auto-aim has under the crosshair (a weapon
// channel's mReticuleTarget, its handle checked), else null.
uint8_t* auto_aimed(const Player& p)
{
   for (int k = 0; k < kReticuleChannels; ++k) {
      uint8_t* const t = *(uint8_t**)(p.ctl + kCtl_Reticule + k * 8);
      if (t && handle_of(t) == *(const uint32_t*)(p.ctl + kCtl_Reticule + 4 + k * 8) && alive(t) && t != p.obj)
         return t;
   }
   return nullptr;
}

Aim side_of(const Player& p, uint8_t* obj)
{
   const int side = s_affiliation ? s_affiliation(p.obj, obj) : (team_of(obj) == p.team ? 1 : -1);
   return side < 0 ? Aim::Enemy : side > 0 ? Aim::Friend : Aim::Spot;   // neutral: a place
}

Aimed aimed(const Player& p, int code)
{
   Aimed out;
   if (!s_rayHit) return out;
   const float* E = (const float*)(p.ctl + kCtl_EyeDir);
   const float* eye = (const float*)(p.ctl + kCtl_EyePoint);
   const float* aim = (const float*)(p.ctl + kCtl_AimStart);
   const float along = (aim[0] - eye[0]) * E[0] + (aim[1] - eye[1]) * E[1] + (aim[2] - eye[2]) * E[2];
   const float* from = along > 0.05f && along < 0.40f ? eye : aim;
   const float len = std::sqrt(E[0] * E[0] + E[1] * E[1] + E[2] * E[2]);
   if (!(len > 0.5f) || !std::isfinite(len) || !std::isfinite(from[0] + from[1] + from[2])) return out;
   const float start[3] = {from[0], from[1], from[2]};
   const float dir[3] = {E[0] / len, E[1] / len, E[2] / len};
   uint8_t* ignore[2] = {p.obj, nullptr};
   int ignored = 1;
   if (uint8_t* const unit = *(uint8_t**)(p.chr + layout::Character::kUnit))
      if (uint8_t* const soldier = object_of(unit); soldier && soldier != p.obj) ignore[ignored++] = soldier;
   void* hit = nullptr;
   float normal[3] = {0.0f, 1.0f, 0.0f};
   const float fraction = s_rayHit(start, dir, kAimReach, &hit, normal, ignore, ignored, kAimRayMask, 0);
   const bool met = fraction >= 0.0f && fraction < 1.0f && hit;
   uint8_t* onLine = nullptr;
   if (met) {
      out.dist = fraction * kAimReach;
      out.at = {start[0] + dir[0] * out.dist, start[1] + dir[1] * out.dist, start[2] + dir[2] * out.dist};
      out.normalUp = normal[1];
      onLine = ((fn_coll_obj_t)(*(void***)hit)[layout::CollisionObject::kVt_GetGameObject / 4])(hit);
      if (onLine && !alive(onLine)) onLine = nullptr;
   }
   uint8_t* const marked = auto_aimed(p);
   // The side the key wants, in the order it wants them.
   const bool friendFirst = code == comm_orders::kDefend;
   const Aim wanted = friendFirst ? Aim::Friend : Aim::Enemy;
   uint8_t* const first = friendFirst ? onLine : marked;
   uint8_t* const second = friendFirst ? marked : onLine;
   for (uint8_t* const obj : {first, second})
      if (obj && side_of(p, obj) == wanted) {
         out.aim = wanted;
         out.obj = obj;
         return out;
      }
   // Else what the line met: a unit of either side, or a place.
   if (onLine) {
      out.aim = side_of(p, onLine);
      out.obj = out.aim == Aim::Spot ? nullptr : onLine;
   } else if (met) {
      out.aim = Aim::Spot;   // the ground, a building, a prop
   }
   return out;
}

// ---- Who answers -----------------------------------------------------------------
// An AI of the player's side, on foot or in a ground vehicle (pilots have their
// own business), in the player's squad or not: following it (the squad key's
// recruits, EVQ_SC_IsFollowingChr) or carried over from the squad's last order;
// for a goal, not answering another order.
struct Unit {
   uint8_t* chr = nullptr;
   uint8_t* obj = nullptr;
   uint8_t* uc = nullptr;
   int      index = -1;      // its character, -1 once let go
   uint32_t life = 0;        // its soldier as it was sent (life_of): one killed is let go
   bool     squad = false;
};

struct Request {
   bool     active = false;
   int      code = -1;
   Ask      ask = Ask::Nothing;
   bool     brief = false;            // a friendly's few seconds' help (Order::brief)
   bool     any = false;              // others sent too, where the player may command any friendly
   uint8_t* goal = nullptr;           // AIGoalManager's
   int      team = -1;
   Unit     units[8];
   int      count = 0;
   uint8_t* target = nullptr;         // the enemy, friend, hover or what is aided; null for a place or the player
   uint32_t targetHandle = 0;
   Pos      spot;                     // a place's
   double   askedAt = 0.0;
   double   reachedAt = -1.0;         // a brief help: when the first of them reached the player
   double   until = 0.0;              // the others' end
   uint32_t markTeams = 0;            // the target's marker, while it has one
};
Request s_requests[kMaxRequests];   // one for each key

// The squad's last order's squadmates, handed to the next while it is made:
// they stopped following to obey, so the game counts them its squad no more.
Unit s_carried[8];
int  s_carriedCount = 0;
int  s_scriptAny = -1;   // SetCommandAnyFriendly: 1 or 0 over the class's key; -1 the class decides

// An order the squad is on: Attack, Defend, or a squadmate's aid.
bool holds_squad(const Request& r)
{
   if (r.code == comm_orders::kAttack || r.code == comm_orders::kDefend) return true;
   for (int k = 0; k < r.count; ++k)
      if (r.units[k].index >= 0 && r.units[k].squad) return true;
   return false;
}

bool answering(int index)
{
   for (const Request& r : s_requests)
      for (int k = 0; r.active && k < r.count; ++k)
         if (r.units[k].index == index) return true;
   return false;
}

bool carried(int index)
{
   for (int k = 0; k < s_carriedCount; ++k)
      if (s_carried[k].index == index) return true;
   return false;
}

// A character's life: its soldier (mUnit) while alive, by handle; 0 for none.
uint32_t life_of(uint8_t* chr)
{
   uint8_t* const unit = chr ? *(uint8_t**)(chr + layout::Character::kUnit) : nullptr;
   uint8_t* const soldier = unit ? object_of(unit) : nullptr;
   return soldier && alive(soldier) ? handle_of(soldier) : 0;
}

bool same_life(const Unit& u) { return u.index >= 0 && u.life && life_of(character(u.index)) == u.life; }

// What a unit controls now, and its controller.
uint8_t* unit_object(const Unit& u, uint8_t** uc = nullptr)
{
   uint8_t* const chr = character(u.index);
   uint8_t* const ctl = chr ? controlled(chr) : nullptr;
   if (uc) *uc = ctl ? *(uint8_t**)(ctl + kCtl_Ctrl) : nullptr;
   return ctl ? object_of(ctl) : nullptr;
}

// Following the player: its soldier, or what it controls.
bool follows(const Player& p, uint8_t* uc)
{
   if (p.soldier && squad_send_event(uc, kEvqIsFollowing, p.soldier) == 1) return true;
   return p.obj != p.soldier && squad_send_event(uc, kEvqIsFollowing, p.obj) == 1;
}

int candidates(const Player& p, Unit* out, int max, bool forGoal)
{
   int n = 0;
   int count = 0;
   const uint8_t* const chars = characters(count);
   for (int i = 0; chars && i < count && n < max; ++i) {
      uint8_t* const chr = (uint8_t*)chars + (size_t)i * layout::Character::kSize;
      if (chr == p.chr || *(const int*)(chr + kChr_PlayerId) >= 0) continue;   // players
      if (*(const int*)(chr + layout::Character::kTeam) != p.team || (forGoal && answering(i))) continue;
      uint8_t* const ctl = controlled(chr);
      uint8_t* const obj = ctl ? object_of(ctl) : nullptr;
      if (!obj || !alive(obj) || is_rtti(obj, kRttiFlyer)) continue;
      uint8_t* const uc = *(uint8_t**)(ctl + kCtl_Ctrl);
      if (!uc || !is_rtti(uc, kRttiUnitController)) continue;
      Unit u{chr, obj, uc, i, life_of(chr), false};
      u.squad = carried(i) || follows(p, uc);
      out[n++] = u;
   }
   return n;
}

// Whether the player may order any friendly, not its squad alone: a script's
// SetCommandAnyFriendly, else its soldier class's CommandAnyFriendly
// (squad_formation's keys, through ClassParent).
bool command_any(const Player& p)
{
   if (s_scriptAny >= 0) return s_scriptAny != 0;
   const squad_formation::ClassKeys* const keys = p.soldier ? squad_class_keys(p.soldier) : nullptr;
   return keys && keys->hasCommandAny && keys->commandAny;
}

// ---- Lines on the HUD ----------------------------------------------------------
struct Line {
   uint32_t       key;    // a localisation key a map or mod may word its own way
   const wchar_t* text;
};

void say(const Line& line, int n = -1)
{
   if (!s_message) return;
   const wchar_t* text = line.text;
   if (s_localize)
      if (const wchar_t* own = s_localize(line.key, true); own && *own) text = own;
   wchar_t buf[160];
   if (n >= 0) {
      swprintf_s(buf, L"%s (%d)", text, n);
      text = buf;
   }
   uint8_t color[4] = {0xFF, 0xFF, 0xFF, 0xFF};
   if (s_messageColor) std::memcpy(color, s_messageColor, sizeof color);
   s_message(0, text, color);
}

// Friendlies outside the squad are "nearby allies" (the user: "the text should
// signify it"); the squad's orders say so.
constexpr Line kSayNone      = {pbl_hash("game.commorders.none"), L"No one nearby can help"};
constexpr Line kSayNoSquad   = {pbl_hash("game.commorders.nosquad"), L"No squadmates to order"};
constexpr Line kSayAim       = {pbl_hash("game.commorders.notarget"), L"No target there"};
constexpr Line kSayNoNeed    = {pbl_hash("game.commorders.noneed"), L"You don't need that now"};
constexpr Line kSaySpotted   = {pbl_hash("game.commorders.spotted"), L"Spotted: nearby allies have it in sight"};
constexpr Line kSayAidSquad  = {pbl_hash("game.commorders.aidsquad"), L"A squadmate is coming to help"};
constexpr Line kSayAidAlly   = {pbl_hash("game.commorders.aid"), L"A nearby ally is coming to help"};
constexpr Line kSayPickup    = {pbl_hash("game.commorders.pickup"), L"A nearby ally is coming to pick you up"};
constexpr Line kSayBackup    = {pbl_hash("game.commorders.backup"), L"Nearby allies are coming to back you up"};
constexpr Line kSayAttack    = {pbl_hash("game.commorders.attack"), L"Squad: attacking your target"};
constexpr Line kSayDefend    = {pbl_hash("game.commorders.defend"), L"Squad: defending your target"};
constexpr Line kSayHold      = {pbl_hash("game.commorders.hold"), L"Squad: holding that position"};
constexpr Line kSayAttackAny = {pbl_hash("game.commorders.attackany"), L"Attacking your target"};
constexpr Line kSayDefendAny = {pbl_hash("game.commorders.defendany"), L"Defending your target"};
constexpr Line kSayHoldAny   = {pbl_hash("game.commorders.holdany"), L"Holding that position"};

// ---- Markers ---------------------------------------------------------------------
constexpr uint32_t kMarkerIcon   = pbl_hash("hud_objective_icon_circle");
constexpr uint32_t kAttackColor  = pbl_hash("red");
constexpr uint32_t kDefendColor  = pbl_hash("green");
constexpr float    kMarkerScale  = 3.0f;

void mark(Request& r, uint32_t color)
{
   if (!s_addMarker || !r.target || r.markTeams || r.team < 0) return;
   r.markTeams = 1u << r.team;
   s_addMarker(r.target, nullptr, kMarkerIcon, kMarkerScale, r.markTeams, color, 1, 0, 0, 0.0f);
}

void unmark(Request& r)
{
   if (r.markTeams && r.target && s_removeMarker && handle_of(r.target) == r.targetHandle)
      s_removeMarker(r.target, nullptr, r.markTeams);
   r.markTeams = 0;
}

// ---- Requests ---------------------------------------------------------------------
// A squadmate, still in the life it was sent in, follows the player again (the
// squad key's own EVT_SC_FollowChr, to the player's soldier). True if it took it.
bool refollow(const Player& p, const Unit& u)
{
   if (!same_life(u)) return false;
   uint8_t* uc = nullptr;
   unit_object(u, &uc);
   if (!uc || !is_rtti(uc, kRttiUnitController)) return false;
   return squad_send_event(uc, kEvtFollowChr, p.soldier ? p.soldier : p.obj) == 1;
}

const char* ask_name(Ask a)
{
   switch (a) {
   case Ask::ShootTarget: return "spotted";
   case Ask::AidPlayer: return "aid";
   case Ask::DefendPlayer: return "backup";
   case Ask::DestroyTarget: return "attack";
   case Ask::DefendTarget: return "defend";
   case Ask::DefendSpot: return "hold a place";
   case Ask::PickUpPlayer: return "pickup";
   default: return "nothing";
   }
}

// Over: its members told to wait (UnAssignAll, as the game's DeleteAIGoal does
// first: DeleteGoal changes no order, and a Defend order never ends), each unit's
// own goal let go (the next goal update gives a waiting one its team's), the
// goal deleted, the marker off; its squadmates follow the player again, or, for
// the squad's next order (`carry`), are handed to it.
void end(Request& r, const char* why, bool carry = false)
{
   if (!r.active) return;
   unmark(r);
   uint8_t* const mgr = s_goalManager ? *s_goalManager : nullptr;
   uint8_t* const goal = r.goal && ai_goals_lua::goal_exists(r.goal) ? r.goal : nullptr;
   if (goal && s_unassignAll) s_unassignAll(goal);
   Player p;
   const bool here = player(p);
   int back = 0, handed = 0, freed = 0;
   for (int k = 0; k < r.count; ++k) {
      const Unit& u = r.units[k];
      if (u.index < 0) continue;
      ai_goals_lua::clear_unit_goal(u.index);
      if (u.squad && same_life(u)) {
         if (carry && s_carriedCount < 8) {
            s_carried[s_carriedCount++] = u;
            ++handed;
            continue;
         }
         if (here && refollow(p, u)) {
            ++back;
            continue;
         }
      }
      ++freed;
   }
   if (goal && mgr && s_deleteGoal) s_deleteGoal(mgr, goal);
   if (log_room())
      install_log("[CommKeys] %s over (%s): %d back with you, %d to the next order, %d let go", ask_name(r.ask), why,
                  back, handed, freed);
   r = Request{};
}

// The squadmates carried over, if no order took them: back with the player.
void refollow_carried()
{
   Player p;
   const bool here = player(p);
   for (int k = 0; k < s_carriedCount && here; ++k) refollow(p, s_carried[k]);
   s_carriedCount = 0;
}

// A goal for the units: theirs alone (ai_goals' own goals), so no one else
// joins it by weight. False, and nothing left behind, where it could not be made.
bool give_goal(Request& r, int type, const GoalTarget& target, const Unit* units, int n)
{
   uint8_t* const mgr = s_goalManager ? *s_goalManager : nullptr;
   if (!mgr || !s_addGoal || n <= 0 || r.team < 0 || r.team > 7) return false;
   r.goal = s_addGoal(mgr, (uint8_t)r.team, type, 1.0f, &target);
   if (!r.goal) return false;
   r.count = 0;
   for (int k = 0; k < n && r.count < 8; ++k)
      if (ai_goals_lua::set_unit_goal(units[k].index, r.goal)) r.units[r.count++] = units[k];
   if (r.count == 0) {
      if (s_deleteGoal) s_deleteGoal(mgr, r.goal);
      r.goal = nullptr;
      return false;
   }
   return true;
}

// Whether a unit sees `t`: nothing solid between its eye (a soldier's own, else
// over its position) and the target's middle, kSightShort short of it.
bool sees(const Unit& u, uint8_t* t)
{
   if (!s_rayHit) return true;
   uint8_t* const ctl = controlled(u.chr);
   const float* eye = ctl ? (const float*)(ctl + kCtl_EyePoint) : nullptr;
   Pos from = position_of(u.obj);
   if (eye && std::isfinite(eye[0] + eye[1] + eye[2])) from = {eye[0], eye[1], eye[2]};
   else from.y += kEyeHeight;
   Pos to = position_of(t);
   to.y += 1.0f;
   float d[3] = {to.x - from.x, to.y - from.y, to.z - from.z};
   const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
   if (!(len > kSightShort) || !std::isfinite(len)) return true;
   for (float& c : d) c /= len;
   const float start[3] = {from.x, from.y, from.z};
   void* hit = nullptr;
   float normal[3] = {0.0f, 1.0f, 0.0f};
   uint8_t* ignore[2] = {u.obj, t};
   const float fraction = s_rayHit(start, d, len - kSightShort, &hit, normal, ignore, 2, kSightMask, 0);
   return !(fraction >= 0.0f && fraction < 1.0f);
}

// The game's own line, marker and shout for the key: NetGame::DisplayMessage, as
// each machine shows a comm key's chat online ("<name>: Medic!", from
// common.chat.<code>; TeamAssistManager::AddRequest's marker on the player and its
// shout). It finds the sender by its id and hands it on unchecked, so only where
// the player's id is its own slot; else AddRequest alone (the marker and shout).
// At most one every kStockEvery, and only while the requests' pool has room: it
// holds 64, 5 s each, and past that it overruns the heap.
int live_assists()
{
   const uint8_t* const mgr = s_teamAssist ? *s_teamAssist : nullptr;
   int n = 0;
   for (const uint8_t* t = mgr ? *(const uint8_t* const*)(mgr + kTA_List) : nullptr; t && n < 64;
        t = *(const uint8_t* const*)t)
      ++n;
   return n;
}

void stock_request(const Player& p, int code, double now)
{
   if (!s_teamAssist || !*s_teamAssist || now - s_stockAt < kStockEvery || live_assists() >= kMaxLiveAssists) return;
   s_stockAt = now;
   const int id = *(const int*)(p.chr + kChr_PlayerId);
   if (s_displayMessage && id >= 0 && character(id) == p.chr) {
      alignas(8) uint8_t msg[kChatMsgSize] = {};
      *(int*)(msg + kMsg_Id) = id;
      *(int*)(msg + kMsg_Flag) = 1;
      *(int*)(msg + kMsg_Code) = code;
      s_displayMessage(msg);
   } else if (s_addRequest) {
      s_addRequest(code, p.chr);
   }
}

// Medic, Repair, Ammo: what is aided (Medic: the player's soldier; Repair and
// Ammo: what it controls), its health type (GameObject +0x1FC & 7), and whether
// a unit can give that aid to it: on foot (in a vehicle its class is the
// vehicle's), its class one that aids at all (GetSoldierType 2 or 3: pilot,
// repair, support, special; the stock check's gate), and its weapons giving it
// (RepairHelper::CalcMyAidTypes on a stand-in whose +4 is the unit's agent, all
// that function reads). A squadmate aids too, out of following for it: the
// follow agent has no RepairHelper.
bool can_aid(const Unit& u, uint32_t type, bool ammo)
{
   if (!s_aidTypes || *(uint8_t* const*)(u.chr + layout::Character::kVehicle) || !is_rtti(u.obj, kRttiSoldier))
      return false;
   bool ok = false;
   __try {
      uint8_t* const cls = *(uint8_t* const*)(u.obj + kGO_Class);
      uint8_t* const agent = *(uint8_t* const*)(u.uc + kUC_Agent);
      if (cls && agent) {
         const int kind = ((fn_class_type_t)(*(void***)cls)[kVt_SoldierType / 4])(cls);
         if (kind == 2 || kind == 3) {
            alignas(4) uint8_t helper[kRepairHelperSize] = {};
            *(uint8_t**)(helper + kRH_Agent) = agent;
            uint32_t health = 0, ammoMask = 0;
            s_aidTypes(helper, &health, &ammoMask);
            ok = (((ammo ? ammoMask : health) >> type) & 1u) != 0;
         }
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      ok = false;
   }
   return ok;
}

uint8_t* aided(const Player& p, int code)
{
   if (code == comm_orders::kMedic && p.soldier) return p.soldier;
   return p.obj;
}

bool needs_health(const uint8_t* obj)
{
   const float cur = *(const float*)(obj + kGO_Health), max = *(const float*)(obj + kGO_MaxHealth);
   return std::isfinite(cur) && std::isfinite(max) && cur < max;
}

// Spotted: every friendly within 150 m of the enemy that sees it aims at it where
// it stands; nothing lasting.
void spotted(const Player& p, uint8_t* t)
{
   Unit all[kMaxCharacters];
   const int m = candidates(p, all, kMaxCharacters, false);
   float dist[kMaxCharacters];
   bool ok[kMaxCharacters], squad[kMaxCharacters];
   for (int i = 0; i < m; ++i) {
      dist[i] = distance(position_of(all[i].obj), position_of(t));
      ok[i] = true;
      squad[i] = all[i].squad;
   }
   const comm_orders::Order order = comm_orders::order_for(Ask::ShootTarget, false);
   int picked[kMaxCharacters];
   const int n = comm_orders::pick(dist, ok, squad, m, order.who, order.units, order.range, picked);
   int shooting = 0;
   for (int k = 0; k < n; ++k)
      if (s_forceTarget && sees(all[picked[k]], t)) {
         s_forceTarget(all[picked[k]].uc, t);
         ++shooting;
      }
   if (shooting) say(kSaySpotted, shooting);
   else say(kSayNone);
   if (log_room()) install_log("[CommKeys] spotted #%u: %d of %d near it see it and aim at it", handle_of(t), shooting, n);
}

void press(int code, double now)
{
   Player p;
   if (!player(p)) return;
   // In a flyer the keys are the wingmen's (F5, F11, F12) or nothing.
   if (is_rtti(p.obj, kRttiFlyer)) return;
   stock_request(p, code, now);
   const Aimed a = aimed(p, code);
   const bool reach = a.aim == Aim::Spot && comm_orders::reachable_spot(a.normalUp, a.dist);
   const Ask ask = comm_orders::ask_for(code, a.aim, reach);
   const bool any = command_any(p);
   const comm_orders::Order order = comm_orders::order_for(ask, any);
   if (log_room())
      install_log("[CommKeys] key %d: %s (the crosshair: %s, %.0f m)%s", code + 5, ask_name(ask),
                  a.aim == Aim::Enemy ? "an enemy" : a.aim == Aim::Friend ? "a friend" : a.aim == Aim::Spot ? "a place"
                                                                                                         : "nothing",
                  (double)a.dist, any ? "; any friendly may be ordered" : "");
   switch (ask) {
   case Ask::Nothing:
      if (code == comm_orders::kSpotted || code == comm_orders::kAttack || code == comm_orders::kDefend) say(kSayAim);
      return;
   case Ask::ShootTarget: spotted(p, a.obj); return;
   default: break;
   }
   uint8_t* const who = ask == Ask::AidPlayer ? aided(p, code) : nullptr;
   if (who && (code == comm_orders::kMedic || code == comm_orders::kRepair) && !needs_health(who)) {
      say(kSayNoNeed);
      return;
   }
   // A squad order replaces the squad's last ones (whatever their keys): their
   // squadmates are handed to it.
   const bool squadOrder = order.who == comm_orders::Who::Squad || order.who == comm_orders::Who::SquadPlus;
   s_carriedCount = 0;
   if (squadOrder) {
      for (Request& old : s_requests)
         if (old.active && holds_squad(old)) end(old, "a new squad order", true);
   } else {
      end(s_requests[code], "a new one");
   }
   Request& r = s_requests[code];
   r = Request{};
   r.ask = ask;
   r.code = code;
   r.team = p.team;
   r.brief = order.brief;
   GoalTarget target;
   int type = kGoalDefend;
   if (ask == Ask::DefendPlayer || ask == Ask::PickUpPlayer || ask == Ask::AidPlayer) {
      target.type = kGttCharacter;   // wherever the player goes
      target.chr = p.chr;
      if (ask == Ask::PickUpPlayer) type = kGoalFollow;
      if (who) {
         r.target = who;   // the request ends with it gone, or (Medic, Repair) once whole
         r.targetHandle = handle_of(who);
      }
   } else if (ask == Ask::DefendSpot) {
      target.type = kGttPos;
      target.pos[0] = a.at.x;
      target.pos[1] = a.at.y;
      target.pos[2] = a.at.z;
      r.spot = a.at;
   } else {
      r.target = a.obj;
      r.targetHandle = handle_of(a.obj);
      if (ask == Ask::DestroyTarget) type = kGoalDestroy;
      // A friend driven by a character is guarded wherever it goes; anything
      // else as an object.
      uint8_t* const chr = ask == Ask::DefendTarget ? character_of(a.obj) : nullptr;
      if (chr) {
         target.type = kGttCharacter;
         target.chr = chr;
      } else {
         target.type = kGttObject;
         target.obj = a.obj;
         target.handle = r.targetHandle;
      }
   }
   // Who goes (comm_orders_core.hpp, Order::who): Pickup a hover's driver, the
   // aid one who can give it.
   Unit all[kMaxCharacters];
   const int m = candidates(p, all, kMaxCharacters, true);
   float dist[kMaxCharacters];
   bool ok[kMaxCharacters], squad[kMaxCharacters];
   const Pos from = position_of(p.obj);
   const uint32_t kind = who ? *(const uint32_t*)(who + kGO_Flags) & kHealthTypeBits : 0;
   for (int i = 0; i < m; ++i) {
      dist[i] = distance(position_of(all[i].obj), from);
      squad[i] = all[i].squad;
      ok[i] = ask == Ask::PickUpPlayer ? is_rtti(all[i].obj, kRttiHover)
              : ask == Ask::AidPlayer  ? can_aid(all[i], kind, code == comm_orders::kAmmo)
                                       : true;
   }
   int picked[kMaxCharacters];
   int n = comm_orders::pick(dist, ok, squad, m, order.who, order.units, order.range, picked);
   if (n > 8) n = 8;
   Unit units[8];
   int mates = 0;
   for (int k = 0; k < n; ++k) {
      units[k] = all[picked[k]];
      mates += units[k].squad;
   }
   if (n == 0 || !give_goal(r, type, target, units, n)) {
      say(squadOrder && !any ? kSayNoSquad : kSayNone);
      if (log_room())
         install_log("[CommKeys] %s: no one to send%s", ask_name(ask), squadOrder && !any ? " (no squadmates)" : "");
      refollow_carried();
      r = Request{};
      return;
   }
   // Squadmates handed on that this order did not take (too far) follow again.
   for (int k = 0; k < s_carriedCount; ++k) {
      bool taken = false;
      for (int j = 0; j < r.count && !taken; ++j) taken = r.units[j].index == s_carried[k].index;
      if (!taken) refollow(p, s_carried[k]);
   }
   s_carriedCount = 0;
   r.active = true;
   r.any = mates < r.count;
   r.askedAt = now;
   r.until = now + order.lasts;
   if (ask == Ask::PickUpPlayer) {
      r.target = units[0].obj;   // the hover: the request ends as the player boards it
      r.targetHandle = handle_of(units[0].obj);
   }
   switch (ask) {
   case Ask::AidPlayer: say(mates ? kSayAidSquad : kSayAidAlly); break;
   case Ask::PickUpPlayer: say(kSayPickup); break;
   case Ask::DefendPlayer: say(kSayBackup, r.count); break;
   case Ask::DestroyTarget: say(r.any ? kSayAttackAny : kSayAttack, r.count); break;
   case Ask::DefendSpot: say(r.any ? kSayHoldAny : kSayHold, r.count); break;
   default: say(r.any ? kSayDefendAny : kSayDefend, r.count); break;
   }
   if (log_room())
      install_log("[CommKeys] %s: %d sent (%d of your squad)%s%s", ask_name(ask), r.count, mates,
                  r.brief ? ", a few seconds' help" : "", ask == Ask::DefendSpot ? ", to the place aimed at" : "");
}

// The Attack and Defend targets' markers, each frame: off while a .hud draws its
// own (hud_wingmen.hpp), on while none does; a place has none of the game's.
void update_marks()
{
   for (int which = 0; which < 2; ++which) {
      Request& r = s_requests[which == 0 ? comm_orders::kAttack : comm_orders::kDefend];
      if (!r.active || !r.target) continue;
      if (hud_orders_target_bound(which)) unmark(r);
      else mark(r, r.ask == Ask::DestroyTarget ? kAttackColor : kDefendColor);
   }
}

// Each frame: the requests' ends (a unit killed let go, its next life not sent
// back; a brief help kHelpFor once one reached the player; the others' time;
// their target down or gone; the player aboard its ride, or whole), then the keys.
void tick()
{
   const double now = seconds();
   Player p;
   const bool here = player(p);
   for (Request& r : s_requests) {
      if (!r.active) continue;
      int left = 0;
      for (int k = 0; k < r.count; ++k) {
         Unit& u = r.units[k];
         if (u.index < 0) continue;
         if (!same_life(u)) {
            ai_goals_lua::clear_unit_goal(u.index);
            u.index = -1;
            continue;
         }
         ++left;
         if (r.brief && r.reachedAt < 0.0 && here)
            if (uint8_t* const obj = unit_object(u); obj && distance(position_of(obj), position_of(p.obj)) <= comm_orders::kWithYou)
               r.reachedAt = now;
      }
      if (left == 0) end(r, "its units are gone");
      else if (r.brief ? comm_orders::help_over(now, r.askedAt, r.reachedAt) : now >= r.until)
         end(r, r.brief && r.reachedAt >= 0.0 ? "its help is done" : "its time is up");
      else if (r.target && (handle_of(r.target) != r.targetHandle || !alive(r.target)))
         end(r, r.ask == Ask::DestroyTarget ? "its target is down" : "the one it was for is gone");
      else if (r.ask == Ask::PickUpPlayer && here && p.obj == r.target)
         end(r, "you are aboard");   // its driver back to its own goal, you with it
      else if (r.ask == Ask::AidPlayer && r.code != comm_orders::kAmmo && r.target && !needs_health(r.target))
         end(r, "whole again");
   }
   update_marks();
   if (!g_commKeyOrders || networked()) {
      s_lastBits = 0;
      return;
   }
   const uint32_t bits = here ? comm_bits(p) : 0;
   const uint32_t down = comm_orders::pressed(bits, s_lastBits);
   for (int code = 0; code < comm_orders::kCodes; ++code)
      if (down & (1u << code)) press(code, now);
}

void __cdecl hooked_cm_update(float dt)
{
   __try {
      tick();
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      for (Request& r : s_requests) r = Request{};   // forget rather than act on what may be torn down
   }
   s_cmUpdate(dt);
}

bool starts_with(uintptr_t base, uintptr_t va, const char* bytes, size_t len)
{
   return std::memcmp(resolve(base, va), bytes, len) == 0;
}

} // namespace

void comm_orders_install(uintptr_t base)
{
   if (g_build != GameBuild::Modtools) {
      if (g_commKeyOrders) install_log("[CommKeys] NOT installed: the prototype runs on modtools only");
      return;
   }
   const auto* a = g_addr;
   if (!a->controller_manager_update || !a->net_game_get_local_player || !a->collision_ray_hit ||
       !a->ai_goal_add || !a->ai_goal_delete || !a->ai_goal_unassign_all || !a->ai_goal_manager_instance ||
       !a->uc_force_attack_target ||
       !a->object_affiliation || !a->char_array_base || !a->max_chars || !a->net_in_shell || !a->net_enabled ||
       !a->net_enabled_next) {
      install_log("[CommKeys] NOT installed: no address set for this build");
      return;
   }
   if (!ai_goals_lua::available()) {
      install_log("[CommKeys] NOT installed: needs the AI goals' own goals ([LimitIncreases] AIGoalLimit)");
      return;
   }
   // ControllerManager::Update's prologue; another feature's detour there (its
   // JMP) is chained (the AI squads detour it too when they are on).
   const bool ok = (starts_with(base, a->controller_manager_update, "\x51\xA1", 2) &&
                    starts_with(base, a->controller_manager_update + 6, "\x53\x55\x56\x8B\x70\x04", 6)) ||
                   *(const uint8_t*)resolve(base, a->controller_manager_update) == 0xE9;
   if (!ok) {
      install_log("[CommKeys] NOT installed: ControllerManager::Update differs");
      return;
   }
   LARGE_INTEGER f;
   QueryPerformanceFrequency(&f);
   s_ticksPerSecond = (double)f.QuadPart;
   s_local = (fn_local_t)resolve(base, a->net_game_get_local_player);
   s_rayHit = (fn_ray_hit_t)resolve(base, a->collision_ray_hit);
   s_affiliation = (fn_affiliation_t)resolve(base, a->object_affiliation);
   s_forceTarget = (fn_force_t)resolve(base, a->uc_force_attack_target);
   s_addGoal = (fn_add_goal_t)resolve(base, a->ai_goal_add);
   s_deleteGoal = (fn_delete_goal_t)resolve(base, a->ai_goal_delete);
   s_unassignAll = (fn_unassign_t)resolve(base, a->ai_goal_unassign_all);
   // The stock request and the aid's check: their bytes, else without them.
   if (a->net_display_message && a->team_assist_add_request && a->team_assist_instance &&
       starts_with(base, a->net_display_message, "\x55\x8B\xEC\x81\xEC\x7C\x01\x00\x00", 9) &&
       starts_with(base, a->team_assist_add_request, "\x6A\x14\xB9", 3)) {
      s_displayMessage = (fn_display_t)resolve(base, a->net_display_message);
      s_addRequest = (fn_request_t)resolve(base, a->team_assist_add_request);
      s_teamAssist = (uint8_t* const*)resolve(base, a->team_assist_instance);
   } else {
      install_log("[CommKeys] the stock line, marker and shout unavailable: the game's chat display differs");
   }
   if (a->repair_calc_aid_types &&
       starts_with(base, a->repair_calc_aid_types, "\x51\x53\x55\x56\x8B\x74\x24\x14", 8))
      s_aidTypes = (fn_aid_types_t)resolve(base, a->repair_calc_aid_types);
   else
      install_log("[CommKeys] Medic, Repair and Ammo unavailable: RepairHelper::CalcMyAidTypes differs");
   s_goalManager = (uint8_t* const*)resolve(base, a->ai_goal_manager_instance);
   s_netInShell = (const uint8_t*)resolve(base, a->net_in_shell);
   s_netEnabled = (const uint8_t*)resolve(base, a->net_enabled);
   s_netEnabledNext = (const uint8_t*)resolve(base, a->net_enabled_next);
   if (a->hud_message && a->hud_message_color) {
      s_message = (fn_message_t)resolve(base, a->hud_message);
      s_messageColor = (const uint8_t*)resolve(base, a->hud_message_color);
   }
   if (a->localize_find) s_localize = (fn_localize_t)resolve(base, a->localize_find);
   if (a->target_manager_add_marker && a->target_manager_remove_marker) {
      s_addMarker = (fn_add_marker_t)resolve(base, a->target_manager_add_marker);
      s_removeMarker = (fn_remove_marker_t)resolve(base, a->target_manager_remove_marker);
   }
   s_cmUpdate = (fn_cm_update_t)resolve(base, a->controller_manager_update);
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG r = DetourAttach(&(PVOID&)s_cmUpdate, hooked_cm_update);
   if (r != NO_ERROR || DetourTransactionCommit() != NO_ERROR) {
      if (r != NO_ERROR) DetourTransactionAbort();
      s_cmUpdate = nullptr;
      install_log("[CommKeys] NOT installed: detouring ControllerManager::Update failed (%ld)", (long)r);
      return;
   }
   s_installed = true;
   install_log("[CommKeys] installed (ControllerManager::Update 0x%08X): %s", (unsigned)a->controller_manager_update,
               g_commKeyOrders ? "in single player the comm keys ask the AI for help"
                               : "off ([AI] CommKeyOrders=0): the keys are the game's");
}

void comm_orders_uninstall()
{
   if (!s_installed) return;
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)s_cmUpdate, hooked_cm_update);
   DetourTransactionCommit();
   s_installed = false;
}

void comm_orders_mission_end()
{
   for (Request& r : s_requests) r = Request{};
   s_lastBits = 0;
   s_carriedCount = 0;
   s_scriptAny = -1;
}

namespace comm_orders_lua {

void set_command_any(int on) { s_scriptAny = on < 0 ? -1 : on ? 1 : 0; }

bool command_any()
{
   bool any = s_scriptAny > 0;
   __try {
      Player p;
      if (player(p)) any = ::command_any(p);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
   return any;
}

} // namespace comm_orders_lua

void comm_orders_goals_changed()
{
   for (Request& r : s_requests)
      if (r.goal && !ai_goals_lua::goal_exists(r.goal)) r.goal = nullptr;
}

bool comm_orders_hud_target(int which, uint8_t** obj, float place[3])
{
   if (which < 0 || which > 1 || !obj || !place) return false;
   const Request& r = s_requests[which == 0 ? comm_orders::kAttack : comm_orders::kDefend];
   if (!r.active) return false;
   if (r.target) {
      if (handle_of(r.target) != r.targetHandle || !alive(r.target)) return false;
      *obj = r.target;
      return true;
   }
   if (r.ask != Ask::DefendSpot) return false;
   *obj = nullptr;
   place[0] = r.spot.x;
   place[1] = r.spot.y;
   place[2] = r.spot.z;
   return true;
}
