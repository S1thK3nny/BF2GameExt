#include "pch.h"
#include "squad_formation.hpp"
#include "squad_formation_core.hpp"
#include "core/entity_layout.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "entity/odf_gameext_props.hpp"
#include "game/Battlefront2/Source/GameObjectClass.h"
#include "util/install_log.hpp"

#include <cmath>
#include <cstring>
#include <unordered_map>

// See squad_formation.hpp for the behaviour and the per-build reads.

int   g_squadFormation  = (int)squad_formation::Layout::Auto;
float g_squadSpacing    = squad_formation::kDefaultSpacing;
bool  g_squadAutoFormUp = true;
int   g_squadSize       = 4;
bool  g_squadPathfinding = true;

namespace {

using squad_formation::ClassKeys;
using squad_formation::Defaults;
using squad_formation::Heading;
using squad_formation::Key;
using squad_formation::Layout;
using squad_formation::Vec2;
using squad_formation::Vec3;

// ---- The same on modtools, Steam and GOG ------------------------------------
constexpr int      kSingleFile          = 1;      // FormationType FT_SINGLEFILE: (0, 0, -slot)
constexpr int      kStockOpen           = 6;      // the layout helper's wedge: room round the follower
constexpr uint32_t kGO_Class            = 0x8;    // GameObjectClass*
constexpr uint32_t kGO_HandleId         = 0x204;
constexpr uint32_t kSoldierControllable = 0x240;  // a soldier's Controllable, where g_soldier's offsets start
constexpr uint32_t kCtrl_Owner          = 0x4;    // Controller::mOwner, the Controllable
constexpr uint32_t kVt_IsRtti           = 0x00;   // GameObject vptr
constexpr uint32_t kVt_GetVelocity      = 0x44;   // GameObject vptr
constexpr uint32_t kNav_FollowTarget    = 0x44;   // NavigatorFollowTarget::mFollowTarget: the object ...
constexpr uint32_t kNav_FollowTargetId  = 0x48;   // ... and its handle id
constexpr uint32_t kNav_FollowOffset    = 0x4C;   // NavigatorFollowTarget::mFollowOffset
constexpr uint32_t kEv_Type             = 0x34;   // EventInfo::mType
constexpr uint32_t kEv_Object           = 0x38;   // EventInfo::mGameObject
constexpr int      kFollowChr           = 0x10;   // EVT_SC_FollowChr
constexpr int      kFollowChrLoose      = 0x11;   // EVT_SC_FollowChrLoose
constexpr int      kMaxSlot             = 64;
// A follower's route (see "The route" below).
constexpr uint32_t kGO_MatrixTrans      = 0x120;  // a GameObject's position
constexpr uint32_t kGO_MatrixIface      = 0xC;    // the interface a GameObject's matrix comes from ...
constexpr uint32_t kVt_GetMatrix        = 0x40;   // ... at its vptr +0x40
constexpr uint32_t kOwner_Trackable     = 0x18;   // a Controllable's Trackable interface ...
constexpr uint32_t kVt_GetTrackable     = 0x20;   // ... whose vptr +0x20 gives the GameObject
constexpr uint32_t kLL_Owner            = 0x14;   // AILowLevel::mOwner, the UnitController
constexpr uint32_t kLL_Navigator        = 0x18;   // AILowLevel::mNavigator
constexpr uint32_t kLL_Height           = 0x6C;   // AILowLevel::mHeight
constexpr uint32_t kVt_NavIsDone        = 0x14;
constexpr int      kHeightStand         = 0;
constexpr int      kHeightSprint        = 3;

constexpr uint32_t kRttiSoldier   = pbl_hash("EntitySoldier");
constexpr uint32_t kHashLeader    = pbl_hash("SquadLeader");
constexpr uint32_t kHashSize      = pbl_hash("SquadSize");
constexpr uint32_t kHashFormation = pbl_hash("SquadFormation");
constexpr uint32_t kHashSpacing   = pbl_hash("SquadSpacing");
constexpr uint32_t kHashFollower  = pbl_hash("SquadFollower");
constexpr uint32_t kHashAutoFollow = pbl_hash("SquadAutoFollow");
constexpr uint32_t kHashCommandAny = pbl_hash("CommandAnyFriendly");   // the comm keys' (ai/comm_orders.hpp)

using fn_reserve_offset_t = float*(__cdecl*)(float* out, void* leader, void* follower, int type);
using fn_count_t          = int(__cdecl*)(void* obj, int team, int type);
using fn_send_event_t     = int(__thiscall*)(void* uc, void* ev);
using fn_event_ctor_t     = void*(__thiscall*)(void* ev, int type, void* obj);
using fn_transform_t      = float*(__stdcall*)(float* out, const float* v, const float* m);
using fn_get_vec_t        = const float*(__thiscall*)(void* self);
using fn_is_rtti_t        = bool(__thiscall*)(void* self, uint32_t hash);
using fn_get_obj_t        = uint8_t*(__thiscall*)(void* self);
using fn_is_done_t        = bool(__thiscall*)(void* self);
using fn_nav_follow_t     = void(__thiscall*)(void* lowLevel, void* leader, const float* offset);
using fn_nav_goto_t       = void(__thiscall*)(void* lowLevel, const float* dest, void* obstacle, bool a, bool b);

// EventInfo is 0x58 bytes; its destructor does nothing on any build.
constexpr size_t kEventInfoSize = 0x58;

Defaults  s_defaults;
bool      s_modtools      = false;
bool      s_odf           = false;
uintptr_t s_reserveOffset = 0;   // the game's ReserveOffset
uintptr_t s_count         = 0;   // ReserveManager::GetReservationCountForObject
uintptr_t s_sendEvent     = 0;   // UnitController::SendEvent
uintptr_t s_eventCtor     = 0;   // EventInfo::EventInfo(EventType, GameObject*)
void* const* s_transformFn = nullptr;   // the pointer D3DXVec3TransformCoord is called through
uintptr_t s_navFollow     = 0;   // AILowLevel::SetNavigator_FollowTarget
uintptr_t s_navGoto       = 0;   // AILowLevel::SetNavigator_Goto

// A redirected call: where, and the bytes it had.
struct Site {
   uint8_t* at = nullptr;
   uint8_t  orig[6] = {};
   uint8_t  len = 0;
};
Site s_offsetSite, s_turnSite, s_countSite, s_recruitSite, s_routeSite;

// ---- The game's functions, called from here ---------------------------------
// The retail ones are whole-program optimised, so the callee-saved registers are
// kept here rather than trusted to them.

// ReserveOffset: ECX = out, EDX = leader, follower and type pushed, the caller pops.
__declspec(naked) float* __cdecl call_reserve_offset_rt(float*, void*, void*, int)
{
   __asm {
      push ebp
      mov  ebp, esp
      push ebx
      push esi
      push edi
      push dword ptr [ebp + 20]      // type
      push dword ptr [ebp + 16]      // follower
      mov  ecx, dword ptr [ebp + 8]  // out
      mov  edx, dword ptr [ebp + 12] // leader
      call dword ptr [s_reserveOffset]
      add  esp, 8
      pop  edi
      pop  esi
      pop  ebx
      pop  ebp
      ret
   }
}

// GetReservationCountForObject: ECX = obj, EDX = team, type pushed, the caller pops.
__declspec(naked) int __cdecl call_count_rt(void*, int, int)
{
   __asm {
      push ebp
      mov  ebp, esp
      push ebx
      push esi
      push edi
      push dword ptr [ebp + 16]      // type
      mov  ecx, dword ptr [ebp + 8]  // obj
      mov  edx, dword ptr [ebp + 12] // team
      call dword ptr [s_count]
      add  esp, 4
      pop  edi
      pop  esi
      pop  ebx
      pop  ebp
      ret
   }
}

// SendEvent: thiscall, its argument popped by itself.
__declspec(naked) int __cdecl call_send_event_rt(void*, void*)
{
   __asm {
      push ebp
      mov  ebp, esp
      push ebx
      push esi
      push edi
      push dword ptr [ebp + 12]      // the event
      mov  ecx, dword ptr [ebp + 8]  // the UnitController
      call dword ptr [s_sendEvent]
      pop  edi
      pop  esi
      pop  ebx
      pop  ebp
      ret
   }
}

// EventInfo's constructor: thiscall, its two arguments popped by itself.
__declspec(naked) void* __cdecl call_event_ctor_rt(void*, int, void*)
{
   __asm {
      push ebp
      mov  ebp, esp
      push ebx
      push esi
      push edi
      push dword ptr [ebp + 16]      // the object
      push dword ptr [ebp + 12]      // the type
      mov  ecx, dword ptr [ebp + 8]  // the EventInfo
      call dword ptr [s_eventCtor]
      pop  edi
      pop  esi
      pop  ebx
      pop  ebp
      ret
   }
}

// AILowLevel::SetNavigator_FollowTarget: thiscall(leader, offset), RET 8.
__declspec(naked) void __cdecl call_nav_follow_rt(void*, void*, const float*)
{
   __asm {
      push ebp
      mov  ebp, esp
      push ebx
      push esi
      push edi
      push dword ptr [ebp + 16]      // the offset
      push dword ptr [ebp + 12]      // the leader
      mov  ecx, dword ptr [ebp + 8]  // the AILowLevel
      call dword ptr [s_navFollow]
      pop  edi
      pop  esi
      pop  ebx
      pop  ebp
      ret
   }
}

// AILowLevel::SetNavigator_Goto: thiscall(dest, obstacle, bool, bool), RET 0x10.
__declspec(naked) void __cdecl call_nav_goto_rt(void*, const float*, void*, int, int)
{
   __asm {
      push ebp
      mov  ebp, esp
      push ebx
      push esi
      push edi
      push dword ptr [ebp + 24]
      push dword ptr [ebp + 20]
      push dword ptr [ebp + 16]      // the obstacle
      push dword ptr [ebp + 12]      // where to
      mov  ecx, dword ptr [ebp + 8]  // the AILowLevel
      call dword ptr [s_navGoto]
      pop  edi
      pop  esi
      pop  ebx
      pop  ebp
      ret
   }
}

void call_nav_follow(void* lowLevel, void* leader, const float* offset)
{
   if (s_modtools) ((fn_nav_follow_t)s_navFollow)(lowLevel, leader, offset);
   else            call_nav_follow_rt(lowLevel, leader, offset);
}

void call_nav_goto(void* lowLevel, const float* dest)
{
   if (s_modtools) ((fn_nav_goto_t)s_navGoto)(lowLevel, dest, nullptr, false, false);
   else            call_nav_goto_rt(lowLevel, dest, nullptr, 0, 0);
}

float* call_reserve_offset(float* out, void* leader, void* follower, int type)
{
   if (s_modtools) return ((fn_reserve_offset_t)s_reserveOffset)(out, leader, follower, type);
   return call_reserve_offset_rt(out, leader, follower, type);
}

int call_count(void* obj, int team, int type)
{
   if (s_modtools) return ((fn_count_t)s_count)(obj, team, type);
   return call_count_rt(obj, team, type);
}

int call_send_event(void* uc, void* ev)
{
   if (s_modtools) return ((fn_send_event_t)s_sendEvent)(uc, ev);
   return call_send_event_rt(uc, ev);
}

const float* call_vec(void* self, uint32_t slot)
{
   void** vt = *(void***)self;
   return ((fn_get_vec_t)vt[slot / 4])(self);
}

bool is_rtti(void* obj, uint32_t hash)
{
   void** vt = *(void***)obj;
   return ((fn_is_rtti_t)vt[kVt_IsRtti / 4])(obj, hash);
}

// ---- The ODF keys, per class ------------------------------------------------
std::unordered_map<const void*, ClassKeys> s_classes;

bool on_property(void* cls, uint32_t hash, const char* value)
{
   Key key;
   switch (hash) {
   case kHashLeader:    key = Key::Leader;    break;
   case kHashSize:      key = Key::Size;      break;
   case kHashFormation: key = Key::Formation; break;
   case kHashSpacing:   key = Key::Spacing;   break;
   case kHashFollower:  key = Key::Follower;  break;
   case kHashCommandAny: key = Key::CommandAny; break;
   case kHashAutoFollow: key = Key::AutoFollow; break;
   default:             return false;
   }
   if (cls) squad_formation::set_key(s_classes[cls], key, value);
   return true;   // no stock SetProperty knows it
}

void on_derive(const void* parent, void* child)
{
   if (s_classes.empty()) return;
   const auto it = s_classes.find(parent);
   if (it == s_classes.end()) {
      s_classes.erase(child);
      return;
   }
   const ClassKeys inherited = it->second;   // copy before the insert can rehash
   s_classes[child] = inherited;
}

// The keys of a soldier's class; null for anything else, or a class with none.
const ClassKeys* soldier_keys(uint8_t* obj)
{
   if (!obj || s_classes.empty() || !is_rtti(obj, kRttiSoldier)) return nullptr;
   const void* cls = *(const void* const*)(obj + kSoldierControllable + g_soldier->classPtr);
   const auto it = s_classes.find(cls);
   return it == s_classes.end() ? nullptr : &it->second;
}

// A team's hero class (Lua SetHeroClass).
bool is_hero(const uint8_t* obj)
{
   const uint8_t* cls = *(const uint8_t* const*)(obj + kGO_Class);
   return cls && (layout::GameObjectClass::mTargetFlags(cls) & layout::GameObjectClass::kHeroClass) != 0;
}

// ---- Which way each leader's squad faces ------------------------------------
// Keyed by the leader object and its handle id, so a reused object starts
// afresh. The game thread is the only caller.
struct LeaderHeading {
   const void* leader = nullptr;
   uint32_t    handle = 0;
   Heading     heading;
   int64_t     stamp = 0;
};
constexpr int kLeaders = 64;
LeaderHeading s_leaders[kLeaders];
double s_ticksPerSecond = 0.0;

int64_t now_ticks()
{
   LARGE_INTEGER t;
   QueryPerformanceCounter(&t);
   return t.QuadPart;
}

LeaderHeading& leader_entry(const uint8_t* leader, int64_t now)
{
   const uint32_t handle = *(const uint32_t*)(leader + kGO_HandleId);
   LeaderHeading* oldest = &s_leaders[0];
   for (auto& e : s_leaders) {
      if (e.leader == leader && e.handle == handle) return e;
      if (e.stamp < oldest->stamp) oldest = &e;
   }
   *oldest = LeaderHeading{};
   oldest->leader = leader;
   oldest->handle = handle;
   oldest->stamp = now;
   return *oldest;
}

// ---- The slot: the follow agent's call to ReserveOffset ---------------------
// False to leave the slot to the game.
bool place(float* out, uint8_t* leader, uint8_t* follower, int type)
{
   if (!out || !leader || !follower) return false;
   const ClassKeys* keys = soldier_keys(leader);
   const Layout layout = squad_formation::formation_for(keys, s_defaults);
   if (layout == Layout::Stock) return false;
   // The game's reservation, and this follower's slot number among the leader's.
   call_reserve_offset(out, leader, follower, kSingleFile);
   const float number = -out[2];
   if (!std::isfinite(number) || number < -0.5f || number > (float)kMaxSlot) return false;
   const Vec2 slot = squad_formation::slot_offset(layout, (int)(number + 0.5f),
                                                  squad_formation::spacing_for(keys, s_defaults),
                                                  type == kStockOpen);
   const Vec3 tagged = squad_formation::tagged_slot(slot);
   out[0] = tagged.x;
   out[1] = tagged.y;
   out[2] = tagged.z;
   return true;
}

// Stands in for ReserveOffset at the follow agent's call, with its modtools
// convention: cdecl(out, leader, follower, type), out back in EAX.
float* __cdecl formation_offset(float* out, uint8_t* leader, uint8_t* follower, int type)
{
   __try {
      if (place(out, leader, follower, type)) return out;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
   return call_reserve_offset(out, leader, follower, type);   // the game's own slot
}

// The same at the retail call: ECX = out, EDX = leader, follower and type on
// the stack for the caller to pop. Every register but EAX comes back as it was,
// the XMM ones included, since the caller is whole-program optimised.
__declspec(naked) void formation_offset_rt()
{
   __asm {
      push   ebp
      mov    ebp, esp
      and    esp, 0xFFFFFFF0
      sub    esp, 0x80
      movups [esp + 0x00], xmm0
      movups [esp + 0x10], xmm1
      movups [esp + 0x20], xmm2
      movups [esp + 0x30], xmm3
      movups [esp + 0x40], xmm4
      movups [esp + 0x50], xmm5
      movups [esp + 0x60], xmm6
      movups [esp + 0x70], xmm7
      push   ebx
      push   esi
      push   edi
      push   ecx
      push   edx
      push   dword ptr [ebp + 12]   // type
      push   dword ptr [ebp + 8]    // follower
      push   edx                    // leader
      push   ecx                    // out
      call   formation_offset
      add    esp, 16
      pop    edx
      pop    ecx
      pop    edi
      pop    esi
      pop    ebx
      movups xmm0, [esp + 0x00]
      movups xmm1, [esp + 0x10]
      movups xmm2, [esp + 0x20]
      movups xmm3, [esp + 0x30]
      movups xmm4, [esp + 0x40]
      movups xmm5, [esp + 0x50]
      movups xmm6, [esp + 0x60]
      movups xmm7, [esp + 0x70]
      mov    esp, ebp
      pop    ebp
      ret
   }
}

// ---- The turn: NavigatorFollowTarget::UpdateTarget's D3DXVec3TransformCoord --
// A tagged offset, ours, turns to the squad's heading round the leader's
// position, every update; anything else is the game's, turned by the matrix.
// The way a leader's squad faces now, brought up to date; null while it has
// none. `m` is the leader's matrix.
const Heading* squad_heading(uint8_t* leader, const float* m)
{
   const float* velocity = call_vec(leader, kVt_GetVelocity);
   if (!velocity) return nullptr;
   const int64_t now = now_ticks();
   LeaderHeading& entry = leader_entry(leader, now);
   const float dt = (float)((double)(now - entry.stamp) / s_ticksPerSecond);
   entry.stamp = now;
   entry.heading = squad_formation::next_heading(entry.heading, {velocity[0], velocity[2]}, {m[8], m[10]}, dt);
   return entry.heading.valid ? &entry.heading : nullptr;
}

bool turn_slot(float* out, const float* v, const float* m)
{
   const uint8_t* nav = (const uint8_t*)v - kNav_FollowOffset;
   uint8_t* leader = *(uint8_t* const*)(nav + kNav_FollowTarget);
   if (!leader || *(const uint32_t*)(leader + kGO_HandleId) != *(const uint32_t*)(nav + kNav_FollowTargetId))
      return false;
   const Heading* heading = squad_heading(leader, m);
   if (!heading) return false;
   const Vec3 p = squad_formation::slot_point({v[0], v[1], v[2]}, heading->dir, m + 12);
   out[0] = p.x;
   out[1] = p.y;
   out[2] = p.z;
   return true;
}

// stdcall(out, v, m), as D3DXVec3TransformCoord: the callers treat it as the
// import it stands in for.
float* __stdcall follow_transform(float* out, const float* v, const float* m)
{
   __try {
      if (out && v && m && squad_formation::is_tagged(v[1]) && turn_slot(out, v, m)) return out;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
   return ((fn_transform_t)*s_transformFn)(out, v, m);
}

// ---- The route: the follow agent's call to SetNavigator_FollowTarget --------
// While a follower keeps up, the game's follow navigator walks it from crumb
// to crumb in straight lines. Once it falls well behind, or makes no headway
// as its squad moves on, it gets the route a unit going anywhere gets
// (SetNavigator_Goto, the planner, round obstacles and barriers) to its
// leader, whose position is always one a soldier can stand on, until it has
// caught up (squad_formation_core.hpp).
struct FollowerRoute {
   const void* follower = nullptr;
   uint32_t    handle = 0;
   squad_formation::FollowState state;
   const void* navigator = nullptr;   // the route it was last given, and its vptr: the
   const void* vptr = nullptr;        // pool can hand its address to another navigator
   int64_t     stamp = 0;
};
constexpr int kFollowers = 128;
FollowerRoute s_routes[kFollowers];

FollowerRoute& route_entry(const uint8_t* follower, int64_t now)
{
   const uint32_t handle = *(const uint32_t*)(follower + kGO_HandleId);
   FollowerRoute* oldest = &s_routes[0];
   for (auto& e : s_routes) {
      if (e.follower == follower && e.handle == handle) {
         e.stamp = now;
         return e;
      }
      if (e.stamp < oldest->stamp) oldest = &e;
   }
   *oldest = FollowerRoute{};
   oldest->follower = follower;
   oldest->handle = handle;
   oldest->stamp = now;
   return *oldest;
}

// Where the agent wants the follower: its slot, as the navigator would put it.
bool slot_in_world(uint8_t* leader, const float* offset, Vec2& slot)
{
   void* const iface = leader + kGO_MatrixIface;
   const float* m = call_vec(iface, kVt_GetMatrix);
   if (!m) return false;
   if (squad_formation::is_tagged(offset[1])) {
      const Heading* heading = squad_heading(leader, m);
      if (!heading) return false;
      const Vec3 p = squad_formation::slot_point({offset[0], offset[1], offset[2]}, heading->dir, m + 12);
      slot = {p.x, p.z};
   } else {   // a stock slot, turned by the matrix as D3DXVec3TransformCoord would
      slot = {m[12] + offset[0] * m[0] + offset[1] * m[4] + offset[2] * m[8],
              m[14] + offset[0] * m[2] + offset[1] * m[6] + offset[2] * m[10]};
   }
   return true;
}

// True when it saw to the follower's navigation itself; false to leave it to
// the game's follow navigator.
bool steer(uint8_t* lowLevel, uint8_t* leader, const float* offset)
{
   if (!lowLevel || !leader || !offset) return false;
   uint8_t* const uc = *(uint8_t**)(lowLevel + kLL_Owner);
   uint8_t* const owner = uc ? *(uint8_t**)(uc + kCtrl_Owner) : nullptr;
   if (!owner) return false;
   void* const trackable = owner + kOwner_Trackable;
   uint8_t* const follower = ((fn_get_obj_t)(*(void***)trackable)[kVt_GetTrackable / 4])(trackable);
   if (!follower) return false;
   Vec2 slot;
   if (!slot_in_world(leader, offset, slot)) return false;
   const float* fp = (const float*)(follower + kGO_MatrixTrans);
   const float* lp = (const float*)(leader + kGO_MatrixTrans);

   const int64_t now = now_ticks();
   FollowerRoute& route = route_entry(follower, now);
   uint8_t* const navigator = *(uint8_t**)(lowLevel + kLL_Navigator);
   const bool alive = navigator && navigator == route.navigator && *(void**)navigator == route.vptr &&
                      !((fn_is_done_t)(*(void***)navigator)[kVt_NavIsDone / 4])(navigator);
   const double seconds = (double)now / s_ticksPerSecond;
   const auto action = squad_formation::follow_action(route.state, {fp[0], fp[2]}, slot, {lp[0], lp[2]}, alive,
                                                      seconds);
   if (action == squad_formation::FollowAction::Crumbs) return false;
   if (action == squad_formation::FollowAction::Plan) {
      const float dest[3] = {lp[0], lp[1], lp[2]};
      call_nav_goto(lowLevel, dest);
      route.navigator = *(uint8_t**)(lowLevel + kLL_Navigator);
      route.vptr = route.navigator ? *(void* const*)route.navigator : nullptr;
   }
   // On a route the follow navigator is not there to set the pace: sprint while
   // well behind, as it would.
   const float dx = fp[0] - slot.x, dz = fp[2] - slot.z;
   *(int*)(lowLevel + kLL_Height) =
      dx * dx + dz * dz > squad_formation::kSprintBehind * squad_formation::kSprintBehind ? kHeightSprint
                                                                                           : kHeightStand;
   return true;
}

void follow_route(uint8_t* lowLevel, uint8_t* leader, const float* offset)
{
   __try {
      if (steer(lowLevel, leader, offset)) return;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
   call_nav_follow(lowLevel, leader, offset);   // the game's crumbs
}

// Modtools: thiscall(leader, offset), its two arguments popped, as __fastcall
// with EDX unused.
void __fastcall follow_route_mt(uint8_t* lowLevel, void*, uint8_t* leader, const float* offset)
{
   follow_route(lowLevel, leader, offset);
}

// Retail: ECX = the AILowLevel, the leader and the offset on the stack, popped
// here. Every register but EAX comes back as it was, the XMM ones included.
__declspec(naked) void follow_route_rt()
{
   __asm {
      push   ebp
      mov    ebp, esp
      and    esp, 0xFFFFFFF0
      sub    esp, 0x80
      movups [esp + 0x00], xmm0
      movups [esp + 0x10], xmm1
      movups [esp + 0x20], xmm2
      movups [esp + 0x30], xmm3
      movups [esp + 0x40], xmm4
      movups [esp + 0x50], xmm5
      movups [esp + 0x60], xmm6
      movups [esp + 0x70], xmm7
      push   ebx
      push   esi
      push   edi
      push   ecx
      push   edx
      push   dword ptr [ebp + 12]   // the offset
      push   dword ptr [ebp + 8]    // the leader
      push   ecx                    // the AILowLevel
      call   follow_route
      add    esp, 12
      pop    edx
      pop    ecx
      pop    edi
      pop    esi
      pop    ebx
      movups xmm0, [esp + 0x00]
      movups xmm1, [esp + 0x10]
      movups xmm2, [esp + 0x20]
      movups xmm3, [esp + 0x30]
      movups xmm4, [esp + 0x40]
      movups xmm5, [esp + 0x50]
      movups xmm6, [esp + 0x60]
      movups xmm7, [esp + 0x70]
      mov    esp, ebp
      pop    ebp
      ret    8
   }
}

// ---- Tag-alongs: PlayerController::CheckForAutoFollow -----------------------
// Its stop: the player's formation reservations, as its CMP EAX,2 should see them.
int __cdecl follow_count(uint8_t* obj, int team, int type)
{
   const int count = call_count(obj, team, type);
   __try {
      const ClassKeys* keys = soldier_keys(obj);
      return squad_formation::follow_stop_count(keys, count, squad_formation::forms_up_for(keys, s_defaults),
                                                squad_formation::size_for(keys, s_defaults));
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return count;
   }
}

// Retail: ECX = obj, EDX = team, the type on the stack for the caller to pop.
__declspec(naked) void follow_count_rt()
{
   __asm {
      push   ebp
      mov    ebp, esp
      and    esp, 0xFFFFFFF0
      sub    esp, 0x80
      movups [esp + 0x00], xmm0
      movups [esp + 0x10], xmm1
      movups [esp + 0x20], xmm2
      movups [esp + 0x30], xmm3
      movups [esp + 0x40], xmm4
      movups [esp + 0x50], xmm5
      movups [esp + 0x60], xmm6
      movups [esp + 0x70], xmm7
      push   ebx
      push   esi
      push   edi
      push   ecx
      push   edx
      push   dword ptr [ebp + 8]    // type
      push   edx                    // team
      push   ecx                    // obj
      call   follow_count
      add    esp, 12
      pop    edx
      pop    ecx
      pop    edi
      pop    esi
      pop    ebx
      movups xmm0, [esp + 0x00]
      movups xmm1, [esp + 0x10]
      movups xmm2, [esp + 0x20]
      movups xmm3, [esp + 0x30]
      movups xmm4, [esp + 0x40]
      movups xmm5, [esp + 0x50]
      movups xmm6, [esp + 0x60]
      movups xmm7, [esp + 0x70]
      mov    esp, ebp
      pop    ebp
      ret
   }
}

// Its recruit: a friendly joins the player's squad in formation, or declines,
// when the player's class has tag-alongs form up; loosely, as stock, otherwise.
int __cdecl recruit(uint8_t* uc, uint8_t* ev)
{
   __try {
      if (*(const int*)(ev + kEv_Type) == kFollowChrLoose) {
         const ClassKeys* keys = soldier_keys(*(uint8_t* const*)(ev + kEv_Object));
         if (squad_formation::forms_up_for(keys, s_defaults)) {
            // CheckForAutoFollow sends this only to soldiers.
            uint8_t* const ctrl = *(uint8_t* const*)(uc + kCtrl_Owner);
            uint8_t* const follower = ctrl ? ctrl - kSoldierControllable : nullptr;
            if (follower && !squad_formation::joins_for(soldier_keys(follower), is_hero(follower)))
               return 0;   // turns the player down; the next friendly is asked
            *(int*)(ev + kEv_Type) = kFollowChr;
         }
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
   return call_send_event(uc, ev);
}

// Modtools: thiscall(EventInfo*), popping its argument, as __fastcall with EDX unused.
int __fastcall recruit_mt(uint8_t* uc, void*, uint8_t* ev)
{
   return recruit(uc, ev);
}

// Retail: ECX = the UnitController, the event on the stack, popped here.
__declspec(naked) void recruit_rt()
{
   __asm {
      push   ebp
      mov    ebp, esp
      and    esp, 0xFFFFFFF0
      sub    esp, 0x80
      movups [esp + 0x00], xmm0
      movups [esp + 0x10], xmm1
      movups [esp + 0x20], xmm2
      movups [esp + 0x30], xmm3
      movups [esp + 0x40], xmm4
      movups [esp + 0x50], xmm5
      movups [esp + 0x60], xmm6
      movups [esp + 0x70], xmm7
      push   ebx
      push   esi
      push   edi
      push   ecx
      push   edx
      push   dword ptr [ebp + 8]    // the event
      push   ecx                    // the UnitController
      call   recruit
      add    esp, 8
      pop    edx
      pop    ecx
      pop    edi
      pop    esi
      pop    ebx
      movups xmm0, [esp + 0x00]
      movups xmm1, [esp + 0x10]
      movups xmm2, [esp + 0x20]
      movups xmm3, [esp + 0x30]
      movups xmm4, [esp + 0x40]
      movups xmm5, [esp + 0x50]
      movups xmm6, [esp + 0x60]
      movups xmm7, [esp + 0x70]
      mov    esp, ebp
      pop    ebp
      ret    4
   }
}

// ---- Install ----------------------------------------------------------------
// A CALL rel32's destination, through one incremental-link JMP thunk.
uintptr_t call_destination(const uint8_t* call)
{
   const uint8_t* dest = call + 5 + *(const int32_t*)(call + 1);
   if (dest[0] == 0xE9) dest = dest + 5 + *(const int32_t*)(dest + 1);
   return (uintptr_t)dest;
}

bool calls(const uint8_t* at, uintptr_t function)
{
   return at[0] == 0xE8 && call_destination(at) == function;
}

// UpdateTarget's turn: modtools CALLs a thunk that JMPs through the pointer;
// Steam and GOG CALL through it (FF 15, its address moved with the exe).
bool calls_through(const uint8_t* at, uintptr_t pointer)
{
   if (s_modtools) {
      if (at[0] != 0xE8) return false;
      const uint8_t* thunk = at + 5 + *(const int32_t*)(at + 1);
      return thunk[0] == 0xFF && thunk[1] == 0x25 && *(const uint32_t*)(thunk + 2) == (uint32_t)pointer;
   }
   return at[0] == 0xFF && at[1] == 0x15 && *(const uint32_t*)(at + 2) == (uint32_t)pointer;
}

// CALL `target` over the instruction at `at`, `len` bytes, NOPs after. .text is
// writable during install.
void redirect(Site& site, uint8_t* at, const void* target, uint8_t len)
{
   site.at = at;
   site.len = len;
   std::memcpy(site.orig, at, len);
   at[0] = 0xE8;
   *(int32_t*)(at + 1) = (int32_t)((intptr_t)target - (intptr_t)(at + 5));
   for (uint8_t i = 5; i < len; ++i) at[i] = 0x90;
}

void restore(Site& site)
{
   if (site.at) protected_write(site.at, site.orig, site.len);
   site = Site{};
}

const char* layout_name(Layout layout)
{
   switch (layout) {
   case Layout::Stock:  return "stock";
   case Layout::Wedge:  return "wedge";
   case Layout::File:   return "file";
   case Layout::Column: return "column";
   case Layout::Line:   return "line";
   default:             return "auto";
   }
}

void install_formation(uintptr_t base)
{
   const auto* a = g_addr;
   if (!a->follow_reserve_offset_call || !a->reserve_offset || !a->follow_target_transform_call ||
       !a->follow_target_transform_ptr) {
      install_log("[SquadFormation] formations NOT installed: no address set for this build");
      return;
   }
   uint8_t* const offsetAt = (uint8_t*)resolve(base, a->follow_reserve_offset_call);
   uint8_t* const turnAt = (uint8_t*)resolve(base, a->follow_target_transform_call);
   const uintptr_t pointer = (uintptr_t)resolve(base, a->follow_target_transform_ptr);
   s_reserveOffset = (uintptr_t)resolve(base, a->reserve_offset);
   // The slot and its turn go in together: a tagged slot turned by the matrix
   // would face the leader's camera again.
   if (!calls(offsetAt, s_reserveOffset) || !calls_through(turnAt, pointer)) {
      install_log("[SquadFormation] formations NOT installed: the follow agent's calls differ");
      return;
   }
   s_transformFn = (void* const*)pointer;
   redirect(s_offsetSite, offsetAt, s_modtools ? (const void*)&formation_offset : (const void*)&formation_offset_rt, 5);
   redirect(s_turnSite, turnAt, (const void*)&follow_transform, s_modtools ? 5 : 6);
   install_log("[SquadFormation] formations installed: %s, %.1f m apart unless a class says otherwise, "
               "facing the way the squad travels", layout_name(s_defaults.formation), (double)s_defaults.spacing);
}

void install_tag_alongs(uintptr_t base)
{
   const auto* a = g_addr;
   if (!a->auto_follow_count_call || !a->reservation_count_for_object || !a->auto_follow_recruit_call ||
       !a->unit_controller_send_event) {
      install_log("[SquadFormation] tag-alongs NOT installed: no address set for this build");
      return;
   }
   uint8_t* const countAt = (uint8_t*)resolve(base, a->auto_follow_count_call);
   uint8_t* const recruitAt = (uint8_t*)resolve(base, a->auto_follow_recruit_call);
   s_count = (uintptr_t)resolve(base, a->reservation_count_for_object);
   if (!s_sendEvent || !calls(countAt, s_count) || !calls(recruitAt, s_sendEvent)) {
      install_log("[SquadFormation] tag-alongs NOT installed: CheckForAutoFollow's calls differ");
      return;
   }
   redirect(s_countSite, countAt, s_modtools ? (const void*)&follow_count : (const void*)&follow_count_rt, 5);
   redirect(s_recruitSite, recruitAt, s_modtools ? (const void*)&recruit_mt : (const void*)&recruit_rt, 5);
   install_log("[SquadFormation] tag-alongs installed: %s, up to %d, unless the player's class says otherwise",
               s_defaults.autoFormUp ? "in formation" : "loose as stock", s_defaults.size);
}

// Followers' routes: the follow agent's call to SetNavigator_FollowTarget, and
// SetNavigator_Goto by its prologue (no absolute addresses in it).
void install_routes(uintptr_t base)
{
   if (!g_squadPathfinding) {
      install_log("[SquadFormation] follower routes off (SquadPathfinding=0): followers walk the crumbs");
      return;
   }
   const auto* a = g_addr;
   if (!a->follow_set_navigator_call || !a->ai_set_navigator_follow_target || !a->ai_set_navigator_goto) {
      install_log("[SquadFormation] follower routes NOT installed: no address set for this build");
      return;
   }
   uint8_t* const at = (uint8_t*)resolve(base, a->follow_set_navigator_call);
   const uintptr_t follow = (uintptr_t)resolve(base, a->ai_set_navigator_follow_target);
   const uint8_t* go = (const uint8_t*)resolve(base, a->ai_set_navigator_goto);
   const bool gotoOk = s_modtools ? std::memcmp(go, "\x83\xEC\x20\x53\x55\x56\x57\x8B\xF9", 9) == 0
                                  : std::memcmp(go, "\x55\x8B\xEC\x83\xEC\x24\x53\x56\x57\x8B\xF9", 11) == 0;
   if (!calls(at, follow) || !gotoOk) {
      install_log("[SquadFormation] follower routes NOT installed: the follow agent's navigator calls differ");
      return;
   }
   s_navFollow = follow;
   s_navGoto = (uintptr_t)go;
   redirect(s_routeSite, at, s_modtools ? (const void*)&follow_route_mt : (const void*)&follow_route_rt, 5);
   install_log("[SquadFormation] follower routes installed: a follower left %.0f m behind, or stuck, plans a route "
               "to its leader", (double)squad_formation::kPathFar);
}

// SendEvent and EventInfo's constructor, for the tag-alongs and AI squads, each
// by its prologue (no absolute addresses in either).
bool starts_with(const uint8_t* code, const char* bytes, size_t len)
{
   return std::memcmp(code, bytes, len) == 0;
}

void resolve_events(uintptr_t base)
{
   const auto* a = g_addr;
   if (!a->unit_controller_send_event || !a->event_info_ctor) return;
   const uint8_t* send = (const uint8_t*)resolve(base, a->unit_controller_send_event);
   const uint8_t* ctor = (const uint8_t*)resolve(base, a->event_info_ctor);
   const bool ok = s_modtools
      ? starts_with(send, "\x83\xEC\x34\x56\x8B\xF1\x8B\x8E\xC0\x02\x00\x00", 12) &&
        starts_with(ctor, "\x83\xEC\x0C\x56\x57\x33\xFF\x83\xEC\x08", 10)
      : starts_with(send, "\x55\x8B\xEC\x83\xEC\x34\x56\x8B\xF1\x8B\x8E\xC0\x02\x00\x00", 15) &&
        starts_with(ctor, "\x55\x8B\xEC\x83\xEC\x10\x83\xEC\x08", 9);
   if (!ok) {
      install_log("[SquadFormation] SendEvent or EventInfo's constructor differs: no squad events");
      return;
   }
   s_sendEvent = (uintptr_t)send;
   s_eventCtor = (uintptr_t)ctor;
}

} // namespace

const squad_formation::ClassKeys* squad_class_keys(uint8_t* soldier)
{
   return soldier_keys(soldier);
}

bool squad_is_hero(const uint8_t* obj)
{
   return obj && is_hero(obj);
}

squad_formation::Defaults& squad_defaults()
{
   return s_defaults;
}

int squad_send_event(void* unitController, int type, void* leader)
{
   if (!s_sendEvent || !s_eventCtor || !unitController || !leader) return -1;
   alignas(16) uint8_t ev[kEventInfoSize + 8] = {};
   if (s_modtools) ((fn_event_ctor_t)s_eventCtor)(ev, type, leader);
   else            call_event_ctor_rt(ev, type, leader);
   return call_send_event(unitController, ev);
}

void squad_formation_install(uintptr_t base)
{
   if (g_build != GameBuild::Modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;
   s_modtools = g_build == GameBuild::Modtools;
   LARGE_INTEGER f;
   QueryPerformanceFrequency(&f);
   s_ticksPerSecond = (double)f.QuadPart;

   s_defaults.formation = (squad_formation::Layout)g_squadFormation;
   s_defaults.spacing = squad_formation::clamp_spacing(g_squadSpacing);
   s_defaults.size = g_squadSize < 1 ? 1 : g_squadSize > squad_formation::kMaxSquadSize
                                            ? squad_formation::kMaxSquadSize : g_squadSize;
   s_defaults.autoFormUp = g_squadAutoFormUp;

   // Installed whatever the INI says: a class's ODF keys decide for it, and with
   // neither set every call goes on to the game unchanged.
   s_odf = odf_add_property_handler(on_property) && odf_add_derive_handler(on_derive);
   if (!s_odf) install_log("[SquadFormation] ODF keys unavailable: no listener slot left; the INI decides");
   resolve_events(base);
   install_formation(base);
   install_tag_alongs(base);
   install_routes(base);
}

void squad_formation_uninstall()
{
   // Sections are re-protected by now, so these cannot be plain stores.
   restore(s_offsetSite);
   restore(s_turnSite);
   restore(s_countSite);
   restore(s_recruitSite);
   restore(s_routeSite);
   s_classes.clear();
}
