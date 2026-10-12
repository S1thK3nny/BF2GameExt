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

constexpr uint32_t kRttiSoldier   = pbl_hash("EntitySoldier");
constexpr uint32_t kHashLeader    = pbl_hash("SquadLeader");
constexpr uint32_t kHashSize      = pbl_hash("SquadSize");
constexpr uint32_t kHashFormation = pbl_hash("SquadFormation");
constexpr uint32_t kHashSpacing   = pbl_hash("SquadSpacing");
constexpr uint32_t kHashFollower  = pbl_hash("SquadFollower");

using fn_reserve_offset_t = float*(__cdecl*)(float* out, void* leader, void* follower, int type);
using fn_count_t          = int(__cdecl*)(void* obj, int team, int type);
using fn_send_event_t     = int(__thiscall*)(void* uc, void* ev);
using fn_transform_t      = float*(__stdcall*)(float* out, const float* v, const float* m);
using fn_get_vec_t        = const float*(__thiscall*)(void* self);
using fn_is_rtti_t        = bool(__thiscall*)(void* self, uint32_t hash);

Defaults  s_defaults;
bool      s_modtools      = false;
bool      s_odf           = false;
uintptr_t s_reserveOffset = 0;   // the game's ReserveOffset
uintptr_t s_count         = 0;   // ReserveManager::GetReservationCountForObject
uintptr_t s_sendEvent     = 0;   // UnitController::SendEvent
void* const* s_transformFn = nullptr;   // the pointer D3DXVec3TransformCoord is called through

// A redirected call: where, and the bytes it had.
struct Site {
   uint8_t* at = nullptr;
   uint8_t  orig[6] = {};
   uint8_t  len = 0;
};
Site s_offsetSite, s_turnSite, s_countSite, s_recruitSite;

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
bool turn_slot(float* out, const float* v, const float* m)
{
   const uint8_t* nav = (const uint8_t*)v - kNav_FollowOffset;
   uint8_t* leader = *(uint8_t* const*)(nav + kNav_FollowTarget);
   if (!leader || *(const uint32_t*)(leader + kGO_HandleId) != *(const uint32_t*)(nav + kNav_FollowTargetId))
      return false;
   const float* velocity = call_vec(leader, kVt_GetVelocity);
   if (!velocity) return false;
   const int64_t now = now_ticks();
   LeaderHeading& entry = leader_entry(leader, now);
   const float dt = (float)((double)(now - entry.stamp) / s_ticksPerSecond);
   entry.stamp = now;
   entry.heading = squad_formation::next_heading(entry.heading, {velocity[0], velocity[2]}, {m[8], m[10]}, dt);
   if (!entry.heading.valid) return false;
   const Vec3 p = squad_formation::slot_point({v[0], v[1], v[2]}, entry.heading.dir, m + 12);
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

// ---- Tag-alongs: PlayerController::CheckForAutoFollow -----------------------
// Its stop: the player's formation reservations, as its CMP EAX,2 should see them.
int __cdecl follow_count(uint8_t* obj, int team, int type)
{
   const int count = call_count(obj, team, type);
   __try {
      const ClassKeys* keys = soldier_keys(obj);
      return squad_formation::capped_count(count, squad_formation::forms_up_for(keys, s_defaults),
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
   s_sendEvent = (uintptr_t)resolve(base, a->unit_controller_send_event);
   if (!calls(countAt, s_count) || !calls(recruitAt, s_sendEvent)) {
      install_log("[SquadFormation] tag-alongs NOT installed: CheckForAutoFollow's calls differ");
      return;
   }
   redirect(s_countSite, countAt, s_modtools ? (const void*)&follow_count : (const void*)&follow_count_rt, 5);
   redirect(s_recruitSite, recruitAt, s_modtools ? (const void*)&recruit_mt : (const void*)&recruit_rt, 5);
   install_log("[SquadFormation] tag-alongs installed: %s, up to %d, unless the player's class says otherwise",
               s_defaults.autoFormUp ? "in formation" : "loose as stock", s_defaults.size);
}

} // namespace

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
   install_formation(base);
   install_tag_alongs(base);
}

void squad_formation_uninstall()
{
   // Sections are re-protected by now, so these cannot be plain stores.
   restore(s_offsetSite);
   restore(s_turnSite);
   restore(s_countSite);
   restore(s_recruitSite);
   s_classes.clear();
}
