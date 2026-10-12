#include "pch.h"
#include "ai_squads.hpp"
#include "ai_squads_core.hpp"
#include "squad_formation.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <detours.h>

#include <cmath>
#include <cstring>

// See ai_squads.hpp for the behaviour and the per-build reads.

bool  g_aiSquads     = true;
int   g_aiSquadSize  = 3;
float g_aiSquadRange = 20.0f;

namespace {

using ai_squads::Recruit;
using ai_squads::Unit;

// ---- The same on modtools, Steam and GOG ------------------------------------
constexpr uint32_t kNodeNext        = 0x04;
constexpr uint32_t kNodePayload     = 0x0C;
constexpr uint32_t kVt_IsRtti       = 0x00;
constexpr uint32_t kCtrl_Owner      = 0x04;    // Controller::mOwner, the Controllable
constexpr uint32_t kOwner_Trackable = 0x18;    // the Controllable's Trackable interface ...
constexpr uint32_t kVt_GetTrackable = 0x20;    // ... whose vptr +0x20 gives the GameObject
constexpr uint32_t kUC_State        = 0x7C;    // 2: in combat
constexpr uint32_t kUC_Agent        = 0x2C0;   // UnitController::mAgent
constexpr uint32_t kGO_MatrixTrans  = 0x120;
constexpr uint32_t kGO_CurHealth    = 0x144;
constexpr uint32_t kGO_Flags        = 0x1FC;   // bit 3: alive
constexpr uint32_t kGO_HandleId     = 0x204;
constexpr uint32_t kGO_Team         = 0x234;   // the low 4 bits, signed
constexpr int      kStateCombat     = 2;
constexpr int      kFollowChr       = 0x10;    // EVT_SC_FollowChr
constexpr int      kIsFollowingChr  = 0x0F;    // EVQ_SC_IsFollowingChr
constexpr int      kMaxNodes        = 4096;
constexpr double   kTickSeconds     = 1.0;
constexpr float    kMinRange        = 5.0f;
constexpr float    kMaxRange        = 60.0f;

constexpr uint32_t kRttiUnitController = pbl_hash("UnitController");
constexpr uint32_t kRttiFollowAgent    = pbl_hash("UnitFollowAgent");
constexpr uint32_t kRttiSoldier        = pbl_hash("EntitySoldier");

using fn_update_t   = void(__cdecl*)(float dt);
using fn_is_rtti_t  = bool(__thiscall*)(void* self, uint32_t hash);
using fn_get_obj_t  = uint8_t*(__thiscall*)(void* self);

fn_update_t s_update = nullptr;   // ControllerManager::Update; the trampoline once detoured
uint8_t**   s_list   = nullptr;   // the global holding the controller list
bool        s_rt     = false;     // Steam / GOG: dt in XMM0
bool        s_installed = false;
float       s_range  = 20.0f;
double      s_ticksPerSecond = 0.0;
double      s_lastTick = 0.0;

double seconds_now()
{
   LARGE_INTEGER t;
   QueryPerformanceCounter(&t);
   return (double)t.QuadPart / s_ticksPerSecond;
}

bool is_rtti(void* obj, uint32_t hash)
{
   void** vt = *(void***)obj;
   return ((fn_is_rtti_t)vt[kVt_IsRtti / 4])(obj, hash);
}

// ---- What the tick sees -----------------------------------------------------
struct Seen {
   uint8_t* uc;
   uint8_t* obj;
   uint32_t handle;
};

Seen s_seen[ai_squads::kMaxUnits];
Unit s_units[ai_squads::kMaxUnits];
int  s_count = 0;

int find_seen(uint32_t handle)
{
   for (int i = 0; i < s_count; ++i)
      if (s_seen[i].handle == handle) return i;
   return -1;
}

// When each soldier was first seen: the tie-break is who has been alive longest.
struct Since {
   uint32_t handle;
   double   since;
};
Since s_since[ai_squads::kMaxUnits];
int   s_sinceCount = 0;

double alive_since(uint32_t handle, double now)
{
   for (int i = 0; i < s_sinceCount; ++i)
      if (s_since[i].handle == handle) return s_since[i].since;
   return now;
}

// Keeps the soldiers seen this tick, with when each was first seen.
void note_since(double now)
{
   Since next[ai_squads::kMaxUnits];
   for (int i = 0; i < s_count; ++i) next[i] = {s_seen[i].handle, alive_since(s_seen[i].handle, now)};
   std::memcpy(s_since, next, sizeof(Since) * s_count);
   s_sinceCount = s_count;
}

// ---- The squads this module made --------------------------------------------
struct Squad {
   uint32_t leader;
   uint32_t members[squad_formation::kMaxSquadSize];
   int      count;
};
constexpr int kMaxSquads = 128;
Squad s_squads[kMaxSquads];
int   s_squadCount = 0;

Squad* squad_of(uint32_t leader)
{
   for (int i = 0; i < s_squadCount; ++i)
      if (s_squads[i].leader == leader) return &s_squads[i];
   if (s_squadCount == kMaxSquads) return nullptr;
   Squad& s = s_squads[s_squadCount++];
   s = Squad{leader, {}, 0};
   return &s;
}

// Every AI soldier, from the controller list.
void walk(double now)
{
   s_count = 0;
   uint8_t* const list = s_list ? *s_list : nullptr;
   if (!list) return;
   const squad_formation::Defaults& d = squad_defaults();
   uint8_t* node = *(uint8_t**)(list + kNodeNext);
   for (int i = 0; i < kMaxNodes && node && s_count < ai_squads::kMaxUnits; ++i) {
      uint8_t* const ctrl = *(uint8_t**)(node + kNodePayload);
      if (!ctrl) break;   // the list's end
      node = *(uint8_t**)(node + kNodeNext);
      if (!is_rtti(ctrl, kRttiUnitController)) continue;
      uint8_t* const owner = *(uint8_t**)(ctrl + kCtrl_Owner);
      if (!owner) continue;
      void* const trackable = owner + kOwner_Trackable;
      uint8_t* const obj = ((fn_get_obj_t)(*(void***)trackable)[kVt_GetTrackable / 4])(trackable);
      if (!obj || !is_rtti(obj, kRttiSoldier)) continue;   // in a vehicle, or no soldier at all
      if (!((*(const uint32_t*)(obj + kGO_Flags) >> 3) & 1u)) continue;
      const float health = *(const float*)(obj + kGO_CurHealth);
      if (!(health > 0.0f)) continue;

      const squad_formation::ClassKeys* keys = squad_class_keys(obj);
      const bool hero = squad_is_hero(obj);
      uint8_t* const agent = *(uint8_t**)(ctrl + kUC_Agent);
      const uint32_t handle = *(const uint32_t*)(obj + kGO_HandleId);

      Unit& u = s_units[s_count];
      u = Unit{};
      u.team = (int32_t)(*(const uint32_t*)(obj + kGO_Team) << 28) >> 28;
      u.x = *(const float*)(obj + kGO_MatrixTrans);
      u.z = *(const float*)(obj + kGO_MatrixTrans + 8);
      u.rank = squad_formation::leader_rank_for(keys, d, hero);
      u.joins = squad_formation::joins_for(keys, hero);
      u.size = squad_formation::ai_size_for(keys, d);
      u.since = alive_since(handle, now);
      u.free = !(agent && is_rtti(agent, kRttiFollowAgent));
      u.combat = *(const int*)(ctrl + kUC_State) == kStateCombat;
      s_seen[s_count++] = {ctrl, obj, handle};
   }
}

// Drops the squads and members that are gone, and counts who is left.
void check_squads()
{
   int kept = 0;
   for (int i = 0; i < s_squadCount; ++i) {
      Squad s = s_squads[i];
      const int l = find_seen(s.leader);
      if (l < 0) continue;   // the leader died or took a vehicle: its followers let go themselves
      int count = 0;
      for (int m = 0; m < s.count; ++m) {
         const int f = find_seen(s.members[m]);
         if (f >= 0 && squad_send_event(s_seen[f].uc, kIsFollowingChr, s_seen[l].obj) == 1)
            s.members[count++] = s.members[m];
      }
      s.count = count;
      if (count == 0) continue;
      s_units[l].followers = count;
      s_squads[kept++] = s;
   }
   s_squadCount = kept;
}

void tick()
{
   const double now = seconds_now();
   if (now - s_lastTick < kTickSeconds) return;
   s_lastTick = now;
   walk(now);
   note_since(now);
   check_squads();
   if (s_count < 2) return;

   Recruit recruits[ai_squads::kMaxUnits];
   const int n = ai_squads::plan(s_units, s_count, s_range, recruits, ai_squads::kMaxUnits);
   for (int i = 0; i < n; ++i) {
      const Seen& follower = s_seen[recruits[i].follower];
      const Seen& leader = s_seen[recruits[i].leader];
      if (squad_send_event(follower.uc, kFollowChr, leader.obj) != 1) continue;
      Squad* s = squad_of(leader.handle);
      if (s && s->count < squad_formation::kMaxSquadSize) s->members[s->count++] = follower.handle;
   }
}

void safe_tick()
{
   __try {
      tick();
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      s_squadCount = 0;   // forget rather than act on what may be torn down
   }
}

// Modtools: cdecl(float dt).
void __cdecl hooked_update(float dt)
{
   safe_tick();
   s_update(dt);
}

// Steam and GOG: dt in XMM0, no stack argument. Every register goes back as it
// was before the original runs, which gets the caller's stack untouched.
__declspec(naked) void hooked_update_rt()
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
      pushad
      call   safe_tick
      popad
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
      jmp    dword ptr [s_update]
   }
}

// The prologue, absolute addresses masked.
bool guard(const uint8_t* code, const char* bytes, const char* mask)
{
   for (size_t i = 0; mask[i]; ++i)
      if (mask[i] == 'x' && code[i] != (uint8_t)bytes[i]) return false;
   return true;
}

} // namespace

void ai_squads_install(uintptr_t base)
{
   if (g_build != GameBuild::Modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;
   const auto* a = g_addr;
   if (!a->controller_manager_update || !a->controller_list) {
      install_log("[AISquads] NOT installed: no address set for this build");
      return;
   }
   s_rt = g_build != GameBuild::Modtools;
   LARGE_INTEGER f;
   QueryPerformanceFrequency(&f);
   s_ticksPerSecond = (double)f.QuadPart;

   squad_formation::Defaults& d = squad_defaults();
   d.aiSquads = g_aiSquads;
   d.aiSize = g_aiSquadSize < 1 ? 1 : g_aiSquadSize > squad_formation::kMaxSquadSize
                                        ? squad_formation::kMaxSquadSize : g_aiSquadSize;
   s_range = std::isfinite(g_aiSquadRange) ? g_aiSquadRange : 20.0f;
   s_range = s_range < kMinRange ? kMinRange : s_range > kMaxRange ? kMaxRange : s_range;

   const uint8_t* code = (const uint8_t*)resolve(base, a->controller_manager_update);
   const bool ok = s_rt
      ? guard(code, "\x55\x8B\xEC\x83\xE4\xF8\x83\xEC\x0C\x53\x56\x8B\x35\x00\x00\x00\x00\x57\xF3\x0F\x11\x44\x24\x0C",
              "xxxxxxxxxxxxx????xxxxxxx")
      : guard(code, "\x51\xA1\x00\x00\x00\x00\x53\x55\x56\x8B\x70\x04", "xx????xxxxxx");
   if (!ok) {
      install_log("[AISquads] NOT installed: prologue mismatch at ControllerManager::Update 0x%08X",
                  (unsigned)a->controller_manager_update);
      return;
   }
   s_list = (uint8_t**)resolve(base, a->controller_list);
   s_update = (fn_update_t)code;

   // Installed whatever the INI says: a class's SquadLeader decides for it.
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG r = DetourAttach(&(PVOID&)s_update, s_rt ? (PVOID)&hooked_update_rt : (PVOID)&hooked_update);
   if (r != NO_ERROR || DetourTransactionCommit() != NO_ERROR) {
      if (r != NO_ERROR) DetourTransactionAbort();
      s_update = nullptr;
      install_log("[AISquads] NOT installed: detouring ControllerManager::Update failed (%ld)", (long)r);
      return;
   }
   s_installed = true;
   install_log("[AISquads] installed (ControllerManager::Update 0x%08X): AI squads %s, up to %d followers "
               "within %.0f m, unless a class says otherwise", (unsigned)a->controller_manager_update,
               d.aiSquads ? "on" : "off", d.aiSize, (double)s_range);
}

void ai_squads_uninstall()
{
   if (!s_installed) return;
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)s_update, s_rt ? (PVOID)&hooked_update_rt : (PVOID)&hooked_update);
   DetourTransactionCommit();
   s_installed = false;
   s_squadCount = 0;
}
