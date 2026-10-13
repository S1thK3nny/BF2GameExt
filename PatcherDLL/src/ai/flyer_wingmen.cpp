#include "pch.h"
#include "flyer_wingmen.hpp"
#include "flyer_wingmen_core.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "game/Battlefront2/Source/Character.h"
#include "util/install_log.hpp"

#include <detours.h>

#include <cmath>
#include <cstring>

// See flyer_wingmen.hpp for the behaviour and the reads.

bool  g_flyerWingmen        = false;
int   g_flyerWingmenCount   = flyer_wingmen::kDefaultWingmen;
float g_flyerWingmenSpacing = flyer_wingmen::kDefaultSpacing;

namespace {

using flyer_wingmen::Frame;
using flyer_wingmen::Stats;
using flyer_wingmen::V3;

// ---- Modtools -----------------------------------------------------------------
constexpr uint32_t kFly_Controllable = 0x240;   // UpdateIndirect's `this`, from the entity
constexpr uint32_t kFly_Ctrl         = 0x308;   // Controllable::mCtrl
constexpr uint32_t kFly_State        = 0x5A4;
constexpr uint32_t kFly_Flags        = 0x5F4;
constexpr uint32_t kFly_FreeFlyTime  = 0x1CDC;
constexpr int      kStateFlying      = 2;
constexpr uint8_t  kFlagPaths        = 0x08;    // flies its paths when it may
constexpr uint8_t  kFlagSliding      = 0x10;    // sliding round a barrier
constexpr float    kHoldFreeFly      = 0.5f;    // s of free flight a path flyer is held to

constexpr uint32_t kUC_LowLevel   = 0x2C4;
constexpr uint32_t kLL_Dest       = 0x1C;
constexpr size_t   kDestSize      = 0x34;
constexpr uint32_t kDest_Has      = 0x00;
constexpr uint32_t kDest_Pos      = 0x04;
constexpr uint32_t kDest_Original = 0x10;
constexpr uint32_t kDest_Dir      = 0x1C;
constexpr uint32_t kDest_Safe     = 0x28;
constexpr uint32_t kDest_Taper    = 0x2C;
constexpr uint32_t kDest_Speed    = 0x30;
constexpr int      kSafeHeightFull = 0;

// ---- Any build ------------------------------------------------------------------
constexpr uint32_t kVt_IsRtti       = 0x00;   // a GameObject's or a Controller's
constexpr uint32_t kVt_GetVelocity  = 0x44;   // GameObject
constexpr uint32_t kGO_MatrixIface  = 0xC;    // the interface a GameObject's matrix comes from ...
constexpr uint32_t kVt_GetMatrix    = 0x40;   // ... at its vptr +0x40
constexpr uint32_t kGO_Flags        = 0x1FC;  // bit 3: alive
constexpr uint32_t kGO_HandleId     = 0x204;
constexpr uint32_t kGO_Team         = 0x234;  // the low 4 bits, signed
constexpr uint32_t kCtrl_Owner      = 0x04;   // Controller::mOwner, the Controllable
constexpr uint32_t kOwner_Trackable = 0x18;   // a Controllable's Trackable interface ...
constexpr uint32_t kVt_GetTrackable = 0x20;   // ... whose vptr +0x20 gives the GameObject
constexpr uint32_t kNodeNext        = 0x04;   // sAICtrls nodes
constexpr uint32_t kNodePayload     = 0x0C;
constexpr int      kMaxNodes        = 4096;
constexpr uint32_t kRttiFlyer          = pbl_hash("EntityFlyer");
constexpr uint32_t kRttiUnitController = pbl_hash("UnitController");

constexpr double kTickSeconds   = 1.0;
constexpr double kReportSeconds = 5.0;
constexpr int    kMaxReports    = 400;

using fn_is_rtti_t    = bool(__thiscall*)(void* self, uint32_t hash);
using fn_get_obj_t    = uint8_t*(__thiscall*)(void* self);
using fn_get_vec_t    = const float*(__thiscall*)(void* self);
using fn_speed_t      = float(__thiscall*)(void* flyer);
using fn_local_t      = uint8_t*(__cdecl*)(uint32_t localIndex);
using fn_update_t     = bool(__fastcall*)(void* ctrl, void* edx, float dt);
using fn_calc_dest_t  = bool(__thiscall*)(void* flyer, float dt, void* dest, float* point, float* speed,
                                          float* myPos);

fn_update_t s_update    = nullptr;
uintptr_t   s_calcDest  = 0;
fn_speed_t  s_maxSpeed  = nullptr;
fn_speed_t  s_minSpeed  = nullptr;
fn_local_t  s_local     = nullptr;
uint8_t**   s_list      = nullptr;   // the global holding sAICtrls
bool        s_detoured  = false;
double      s_ticksPerSecond = 0.0;

// A redirected call: where, and the bytes it had.
struct Site {
   uint8_t* at = nullptr;
   uint8_t  orig[5] = {};
};
Site s_callSite;

// ---- The squadron -----------------------------------------------------------------
struct Wingman {
   uint8_t* obj = nullptr;     // the flyer
   uint32_t handle = 0;
   uint8_t* uc = nullptr;      // the UnitController flying it
   int      slot = 0;
   Stats    stats;
};

Wingman  s_wing[flyer_wingmen::kMaxWingmen];
int      s_count = 0;
uint8_t* s_leader = nullptr;
uint32_t s_leaderHandle = 0;
double   s_nextTick = 0.0;
double   s_nextReport = 0.0;
int      s_reports = 0;

double seconds()
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

uint8_t* object_of(uint8_t* controllable)
{
   void* const trackable = controllable + kOwner_Trackable;
   return ((fn_get_obj_t)(*(void***)trackable)[kVt_GetTrackable / 4])(trackable);
}

bool alive(const uint8_t* obj) { return ((*(const uint32_t*)(obj + kGO_Flags)) >> 3) & 1u; }
uint32_t handle_of(const uint8_t* obj) { return *(const uint32_t*)(obj + kGO_HandleId); }
int team_of(const uint8_t* obj) { return (int32_t)(*(const uint32_t*)(obj + kGO_Team) << 28) >> 28; }
bool flying(const uint8_t* flyer) { return *(const int*)(flyer + kFly_State) == kStateFlying; }

const float* matrix_of(uint8_t* obj)
{
   void* const iface = obj + kGO_MatrixIface;
   return ((fn_get_vec_t)(*(void***)iface)[kVt_GetMatrix / 4])(iface);
}

V3 velocity_of(uint8_t* obj)
{
   const float* v = ((fn_get_vec_t)(*(void***)obj)[kVt_GetVelocity / 4])(obj);
   return v ? V3{v[0], v[1], v[2]} : V3{};
}

V3 row(const float* m, int r) { return {m[r * 4], m[r * 4 + 1], m[r * 4 + 2]}; }   // right, up, forward, position

// The flyer the host's own player flies, while it flies.
uint8_t* player_flyer()
{
   uint8_t* const chr = s_local ? s_local(0) : nullptr;
   uint8_t* const vehicle = chr ? *(uint8_t**)(chr + layout::Character::kVehicle) : nullptr;
   if (!vehicle) return nullptr;
   uint8_t* const obj = object_of(vehicle);
   if (!obj || !is_rtti(obj, kRttiFlyer) || !alive(obj) || !flying(obj)) return nullptr;
   return obj;
}

Wingman* find(const uint8_t* obj)
{
   for (int i = 0; i < s_count; ++i)
      if (s_wing[i].obj == obj) return &s_wing[i];
   return nullptr;
}

void drop(int i, const char* why)
{
   install_log("[Wingmen] slot %d (#%u) let go: %s", s_wing[i].slot + 1, s_wing[i].handle, why);
   s_wing[i] = s_wing[--s_count];
   s_wing[s_count] = Wingman{};
}

void release_all(const char* why)
{
   if (s_count) install_log("[Wingmen] %d let go: %s", s_count, why);
   s_count = 0;
   for (auto& w : s_wing) w = Wingman{};
   s_leader = nullptr;
   s_leaderHandle = 0;
}

// Still a wingman? Why not, or null.
const char* lost(const Wingman& w, const uint8_t* leader)
{
   if (handle_of(w.obj) != w.handle || !alive(w.obj)) return "gone";
   if (*(uint8_t* const*)(w.obj + kFly_Ctrl) != w.uc) return "flown by someone else";
   if (!flying(w.obj)) return "not flying";
   if (team_of(w.obj) != team_of(leader)) return "changed sides";
   const float* a = (const float*)matrix_of(w.obj);
   const float* b = (const float*)matrix_of((uint8_t*)leader);
   if (!a || !b) return "no position";
   if (flyer_wingmen::length(row(a, 3) - row(b, 3)) > flyer_wingmen::kKeepRange) return "too far";
   return nullptr;
}

int free_slot()
{
   for (int s = 0; s < flyer_wingmen::kMaxWingmen; ++s) {
      bool taken = false;
      for (int i = 0; i < s_count; ++i) taken |= s_wing[i].slot == s;
      if (!taken) return s;
   }
   return -1;
}

// The nearest friendly AI flyers that can keep up, into the free slots.
void fill(uint8_t* leader, int want)
{
   uint8_t* const list = s_list ? *s_list : nullptr;
   if (!list) return;
   const float* lm = matrix_of(leader);
   if (!lm) return;
   const V3 lp = row(lm, 3);
   const float leaderMax = s_maxSpeed(leader);
   const int team = team_of(leader);

   struct Candidate { uint8_t* obj; uint8_t* uc; float d; };
   Candidate best[flyer_wingmen::kMaxWingmen];
   int found = 0;
   uint8_t* node = *(uint8_t**)(list + kNodeNext);
   for (int i = 0; i < kMaxNodes && node; ++i) {
      uint8_t* const uc = *(uint8_t**)(node + kNodePayload);
      if (!uc) break;   // the list's end
      node = *(uint8_t**)(node + kNodeNext);
      if (!is_rtti(uc, kRttiUnitController)) continue;
      uint8_t* const owner = *(uint8_t**)(uc + kCtrl_Owner);
      if (!owner) continue;
      uint8_t* const obj = object_of(owner);
      if (!obj || obj == leader || !is_rtti(obj, kRttiFlyer) || !alive(obj) || !flying(obj)) continue;
      if (*(uint8_t* const*)(obj + kFly_Ctrl) != uc || team_of(obj) != team || find(obj)) continue;
      const float* m = matrix_of(obj);
      if (!m) continue;
      const float d = flyer_wingmen::length(row(m, 3) - lp);
      if (!(d <= flyer_wingmen::kRecruitRange)) continue;
      if (!flyer_wingmen::can_keep_up(s_maxSpeed(obj), leaderMax)) continue;
      // Keep the `want` nearest, nearest first.
      if (found < want) best[found++] = {obj, uc, d};
      else if (d < best[want - 1].d) best[want - 1] = {obj, uc, d};
      else continue;
      for (int k = found - 1; k > 0 && best[k].d < best[k - 1].d; --k) {
         const Candidate t = best[k];
         best[k] = best[k - 1];
         best[k - 1] = t;
      }
   }
   for (int i = 0; i < found && s_count < flyer_wingmen::kMaxWingmen; ++i) {
      const int slot = free_slot();
      if (slot < 0) break;
      Wingman& w = s_wing[s_count++];
      w = Wingman{};
      w.obj = best[i].obj;
      w.handle = handle_of(best[i].obj);
      w.uc = best[i].uc;
      w.slot = slot;
      install_log("[Wingmen] slot %d: #%u joined, %.0f m off, top speed %.0f m/s against the leader's %.0f",
                  slot + 1, w.handle, (double)best[i].d, (double)s_maxSpeed(w.obj), (double)leaderMax);
   }
}

void report(double now)
{
   if (now < s_nextReport || s_reports >= kMaxReports) return;
   s_nextReport = now + kReportSeconds;
   for (int i = 0; i < s_count; ++i) {
      Stats& s = s_wing[i].stats;
      if (!s.samples) continue;
      const double n = s.samples;
      ++s_reports;
      install_log("[Wingmen] slot %d: off its slot by %.1f m on average, %.1f m at worst (slot %.1f m ahead, %.1f "
                  "right, %.1f up); %.0f m/s against the leader's %.0f",
                  s_wing[i].slot + 1, s.distance / n, (double)s.worst, s.ahead / n, s.right / n, s.up / n,
                  s.speed / n, s.leaderSpeed / n);
      s = Stats{};
   }
}

void tick(double now)
{
   uint8_t* const leader = player_flyer();
   if (!leader) {
      release_all("the player is not flying");
      return;
   }
   if (leader != s_leader || handle_of(leader) != s_leaderHandle) {
      release_all("a new leader");
      s_leader = leader;
      s_leaderHandle = handle_of(leader);
   }
   for (int i = s_count - 1; i >= 0; --i)
      if (const char* why = lost(s_wing[i], leader)) drop(i, why);
   const int want = flyer_wingmen::clamp_count(g_flyerWingmenCount);
   if (s_count < want) fill(leader, want - s_count);
   report(now);
}

// ---- EntityFlyer::UpdateIndirect ----------------------------------------------------
bool __fastcall hooked_update(void* ctrl, void* edx, float dt)
{
   __try {
      const double now = seconds();
      if (now >= s_nextTick) {
         s_nextTick = now + kTickSeconds;
         tick(now);
      }
      // A wingman that flies paths is held off them, so it flies by physics.
      uint8_t* const obj = (uint8_t*)ctrl - kFly_Controllable;
      if (find(obj) && (obj[kFly_Flags] & kFlagPaths)) {
         float& t = *(float*)(obj + kFly_FreeFlyTime);
         if (!(t >= kHoldFreeFly)) t = kHoldFreeFly;
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
   return s_update(ctrl, edx, dt);
}

// ---- EntityFlyer::CalculateDest, at UpdateIndirect's call ---------------------------
struct Steer {
   Wingman* w = nullptr;
   uint8_t* dest = nullptr;          // AILowLevel::mDest
   uint8_t  saved[kDestSize] = {};   // the AI's own
   V3       point;                   // where it is steered
   float    target = 0.0f;           // the speed asked for, a share of its top speed
   float    leaderSpeed = 0.0f;
   Frame    frame;
   V3       slot;
};

bool steer_before(uint8_t* flyer, Steer& st)
{
   Wingman* const w = find(flyer);
   if (!w || !s_leader) return false;
   if (handle_of(flyer) != w->handle || *(uint8_t* const*)(flyer + kFly_Ctrl) != w->uc) return false;
   if (handle_of(s_leader) != s_leaderHandle || !alive(s_leader)) return false;
   const float* lm = matrix_of(s_leader);
   const float* wm = matrix_of(flyer);
   if (!lm || !wm) return false;
   const V3 leaderVel = velocity_of(s_leader);
   st.frame = flyer_wingmen::level_frame(row(lm, 2), row(lm, 0));
   if (!st.frame.valid || !flyer_wingmen::finite(leaderVel)) return false;
   st.slot = flyer_wingmen::to_world(st.frame, row(lm, 3),
                                     flyer_wingmen::slot_offset(w->slot, g_flyerWingmenSpacing));
   st.point = flyer_wingmen::steer_point(st.slot, leaderVel);
   st.leaderSpeed = flyer_wingmen::length(leaderVel);
   const float ahead = flyer_wingmen::in_frame(st.frame, row(wm, 3), st.slot).z;
   const float maxSpeed = s_maxSpeed(flyer);
   const float speed = flyer_wingmen::formation_speed(st.leaderSpeed, ahead, s_minSpeed(flyer), maxSpeed);
   st.target = flyer_wingmen::target_for(speed, maxSpeed);

   uint8_t* const dest = *(uint8_t**)(flyer + kFly_Ctrl) + kUC_LowLevel + kLL_Dest;
   std::memcpy(st.saved, dest, kDestSize);
   st.dest = dest;   // from here on the AI's own goes back, whatever happens
   st.dest[kDest_Has] = 1;
   std::memcpy(st.dest + kDest_Pos, &st.point, 12);
   std::memcpy(st.dest + kDest_Original, &st.point, 12);
   std::memcpy(st.dest + kDest_Dir, &st.frame.fwd, 12);
   *(int*)(st.dest + kDest_Safe) = kSafeHeightFull;
   *(float*)(st.dest + kDest_Taper) = 0.0f;
   *(float*)(st.dest + kDest_Speed) = st.target;
   st.w = w;
   return true;
}

void steer_after(uint8_t* flyer, Steer& st, bool steered, float* speed)
{
   std::memcpy(st.dest, st.saved, kDestSize);   // the AI's own destination back
   const float* wm = matrix_of(flyer);
   if (!wm) return;
   const V3 pos = row(wm, 3), fwd = row(wm, 2);
   if (steered && speed && !(flyer[kFly_Flags] & kFlagSliding)) {
      V3 to = st.point - pos;
      const float cosOff = flyer_wingmen::normalise(to) ? flyer_wingmen::dot(to, fwd) : 1.0f;
      *speed = flyer_wingmen::keep_speed(*speed, st.target, cosOff);
   }
   st.w->stats.add(flyer_wingmen::in_frame(st.frame, pos, st.slot), flyer_wingmen::length(velocity_of(flyer)),
                   st.leaderSpeed);
}

// thiscall(dt, dest, point, speed, myPos) on the entity, RET 0x14, as __fastcall
// with EDX unused.
bool __fastcall calc_dest_mt(uint8_t* flyer, void*, float dt, void* dest, float* point, float* speed, float* myPos)
{
   Steer st;
   bool steering = false;
   __try {
      steering = steer_before(flyer, st);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      steering = false;
      if (st.dest) std::memcpy(st.dest, st.saved, kDestSize);
   }
   const bool r = ((fn_calc_dest_t)s_calcDest)(flyer, dt, dest, point, speed, myPos);
   if (steering) {
      __try {
         steer_after(flyer, st, r, speed);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
   }
   return r;
}

// ---- Install --------------------------------------------------------------------------
bool starts_with(uintptr_t base, uintptr_t va, const char* bytes, size_t len)
{
   return std::memcmp(resolve(base, va), bytes, len) == 0;
}

uintptr_t call_destination(const uint8_t* call)
{
   const uint8_t* dest = call + 5 + *(const int32_t*)(call + 1);
   if (dest[0] == 0xE9) dest = dest + 5 + *(const int32_t*)(dest + 1);
   return (uintptr_t)dest;
}

} // namespace

void flyer_wingmen_install(uintptr_t base)
{
   if (!g_flyerWingmen) return;
   if (g_build != GameBuild::Modtools) {
      install_log("[Wingmen] NOT installed: the prototype runs on modtools only");
      return;
   }
   const auto* a = g_addr;
   if (!a->flyer_update_indirect || !a->flyer_calc_dest_call || !a->flyer_calc_dest || !a->flyer_max_speed ||
       !a->flyer_min_speed || !a->ai_controller_list || !a->net_game_get_local_player) {
      install_log("[Wingmen] NOT installed: no address set for this build");
      return;
   }
   // Prologues up to `this` landing in its register, and the getters' test.
   uint8_t* const at = (uint8_t*)resolve(base, a->flyer_calc_dest_call);
   const bool ok =
      starts_with(base, a->flyer_update_indirect, "\x81\xEC\x84\x00\x00\x00\x53\x55\x56\x8B\xF1", 11) &&
      starts_with(base, a->flyer_calc_dest, "\x83\xEC\x28\x53\x55\x56\x8B\xE9\x8B\x85\x08\x03\x00\x00", 14) &&
      starts_with(base, a->flyer_max_speed, "\xD9\x05\x6C\xA0\xA2\x00\xD9\x81\xFC\x05\x00\x00", 12) &&
      starts_with(base, a->flyer_min_speed, "\xD9\x05\x6C\xA0\xA2\x00\xD9\x81\xFC\x05\x00\x00", 12) &&
      at[0] == 0xE8 && call_destination(at) == (uintptr_t)resolve(base, a->flyer_calc_dest);
   if (!ok) {
      install_log("[Wingmen] NOT installed: EntityFlyer's update or its calls differ");
      return;
   }
   LARGE_INTEGER f;
   QueryPerformanceFrequency(&f);
   s_ticksPerSecond = (double)f.QuadPart;
   g_flyerWingmenCount = flyer_wingmen::clamp_count(g_flyerWingmenCount);
   g_flyerWingmenSpacing = flyer_wingmen::clamp_spacing(g_flyerWingmenSpacing);
   s_calcDest = (uintptr_t)resolve(base, a->flyer_calc_dest);
   s_maxSpeed = (fn_speed_t)resolve(base, a->flyer_max_speed);
   s_minSpeed = (fn_speed_t)resolve(base, a->flyer_min_speed);
   s_local = (fn_local_t)resolve(base, a->net_game_get_local_player);
   s_list = (uint8_t**)resolve(base, a->ai_controller_list);

   // The call, during the installers' writable window.
   s_callSite.at = at;
   std::memcpy(s_callSite.orig, at, 5);
   *(int32_t*)(at + 1) = (int32_t)((intptr_t)&calc_dest_mt - (intptr_t)(at + 5));

   s_update = (fn_update_t)resolve(base, a->flyer_update_indirect);
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG r = DetourAttach(&(PVOID&)s_update, hooked_update);
   if (r != NO_ERROR || DetourTransactionCommit() != NO_ERROR) {
      if (r != NO_ERROR) DetourTransactionAbort();
      std::memcpy(at, s_callSite.orig, 5);
      s_callSite = Site{};
      install_log("[Wingmen] NOT installed: detouring EntityFlyer::UpdateIndirect failed (%ld)", (long)r);
      return;
   }
   s_detoured = true;
   install_log("[Wingmen] installed (prototype): up to %d friendly AI flyers fly a vee on yours, %.0f m apart",
               g_flyerWingmenCount, (double)g_flyerWingmenSpacing);
}

void flyer_wingmen_uninstall()
{
   if (s_callSite.at) protected_write(s_callSite.at, s_callSite.orig, 5);
   s_callSite = Site{};
   if (s_detoured) {
      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourDetach(&(PVOID&)s_update, hooked_update);
      DetourTransactionCommit();
      s_detoured = false;
   }
}
