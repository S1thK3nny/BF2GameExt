#include "pch.h"
#include "flyer_real_flight.hpp"
#include "flyer_real_flight_core.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <unordered_map>

// See flyer_real_flight.hpp for the behaviour and the reads.

bool g_flyerRealFlight = false;

namespace {

using flyer_real_flight::Borrowed;
using flyer_real_flight::Stats;
using flyer_wingmen::V3;

// ---- Modtools -----------------------------------------------------------------
constexpr uint32_t kFly_Remote       = 0x78;     // bit 0: a network proxy, which UpdatePathFollower leaves alone
constexpr uint32_t kFly_Ctrl         = 0x308;    // Controllable::mCtrl
constexpr uint32_t kFly_Follower     = 0x3CC;    // the flyer's EntityPathFollower
constexpr uint32_t kFly_State        = 0x5A4;
constexpr uint32_t kFly_Flags        = 0x5F4;
constexpr uint32_t kFly_EnergyClass  = 0x618;    // the energy bar's class, its max at +0 ...
constexpr uint32_t kFly_Energy       = 0x61C;    // ... the energy ...
constexpr uint32_t kFly_EnergyFlags  = 0x620;    // ... bit 0: not counted
constexpr uint32_t kFly_Class        = 0x66C;    // its EntityFlyerClass ...
constexpr uint32_t kClass_PathClass  = 0x6AC;    // ... whose EntityPathFollowerClass this is ...
constexpr uint32_t kClass_BoostSpeed = 0x898;    // ... and mBoostFlyerSpeed
constexpr uint32_t kFly_FreeFlyTime  = 0x1CDC;
constexpr uint32_t kFol_LandNow      = 0xD5;     // EntityPathFollower::mbLandNow
constexpr uint32_t kVt_BranchParams  = 0x1C;     // the follower's GetBranchParams
constexpr int      kStateFlying      = 2;
constexpr uint8_t  kFlagPaths        = 0x08;     // flies its paths when it may
constexpr uint8_t  kFlagSliding      = 0x10;     // sliding round a barrier
constexpr float    kHoldFreeFly      = 0.5f;     // s of free flight a flyer is held to
constexpr float    kRaisedBy         = 1.0f;     // m: CalculateDest's point this much above the carrot was lifted

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
constexpr uint32_t kVt_IsRtti       = 0x00;
constexpr uint32_t kVt_GetVelocity  = 0x44;   // GameObject
constexpr uint32_t kGO_Class        = 0x8;
constexpr uint32_t kGO_MatrixIface  = 0xC;    // the interface a GameObject's matrix comes from ...
constexpr uint32_t kVt_GetMatrix    = 0x40;   // ... at its vptr +0x40
constexpr uint32_t kGO_Flags        = 0x1FC;  // bit 3: alive
constexpr uint32_t kGO_HandleId     = 0x204;
constexpr uint32_t kRttiUnitController = pbl_hash("UnitController");
constexpr uint32_t kHashRealFlight     = pbl_hash("PathFollowerRealFlight");

constexpr double kStaleSeconds  = 0.5;   // a carrot its flyer's update has not kept this long is gone
constexpr double kReportSeconds = 5.0;
constexpr int    kMaxReports    = 400;
constexpr int    kMaxCarrots    = 128;

// EntityPathFollower::BranchParams, as GetBranchParams fills it.
struct BranchParams {
   void*    pathClass;           // EntityPathFollowerClass*
   float*   matrix;              // the matrix the follower moves: the flyer's, or a carrot
   float    diameter;
   void*    gameObject;
   float    worldSpeed;
   float    squareRange;
   uint32_t branchPaths;
   float    cosMaxAngle;
   uint8_t  overrideBranching;
   uint8_t  fallbackToClosest;
   uint8_t  pad[2];
};
static_assert(sizeof(BranchParams) == 0x24, "GetBranchParams copies 9 dwords");
static_assert(offsetof(BranchParams, matrix) == 0x04, "mMatrix");
static_assert(offsetof(BranchParams, fallbackToClosest) == 0x21, "mFallbackToClosest");

using fn_is_rtti_t       = bool(__thiscall*)(void* self, uint32_t hash);
using fn_get_vec_t       = const float*(__thiscall*)(void* self);
using fn_branch_params_t = BranchParams*(__thiscall*)(void* follower, BranchParams* out);
using fn_step_t          = bool(__thiscall*)(void* follower, BranchParams* params, float dt, float* velocityOut,
                                             float* matrixOut, int forceBranch, float boostRatio, float energyRatio);
using fn_reset_t         = void(__thiscall*)(void* follower, void* pathClass);
using fn_land_t          = void(__thiscall*)(void* flyer);
using fn_upf_t           = void(__thiscall*)(void* flyer, float dt, float* matrix);

fn_step_t  s_step = nullptr;
fn_reset_t s_reset = nullptr;
fn_land_t  s_land = nullptr;
fn_upf_t   s_updatePathFollower = nullptr;
Borrowed   s_borrowed;
bool       s_installed = false;
double     s_ticksPerSecond = 0.0;

struct Site {
   uint8_t* at = nullptr;
   uint8_t  orig[5] = {};
};
Site s_upfSite, s_resetSite;

// ---- The ODF key, per class ---------------------------------------------------------
std::unordered_map<const void*, bool> s_classes;

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

bool alive(const uint8_t* obj) { return ((*(const uint32_t*)(obj + kGO_Flags)) >> 3) & 1u; }
uint32_t handle_of(const uint8_t* obj) { return *(const uint32_t*)(obj + kGO_HandleId); }
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

// Its class's key where it has one, else the INI.
bool wants(const uint8_t* flyer)
{
   const auto it = s_classes.find(*(const void* const*)(flyer + kGO_Class));
   return it == s_classes.end() ? g_flyerRealFlight : it->second;
}

// An AI flyer that flies paths, flying, here and not a network proxy.
bool flies_for_real(uint8_t* flyer)
{
   if (!wants(flyer) || !(flyer[kFly_Flags] & kFlagPaths) || (flyer[kFly_Remote] & 1u) || !flying(flyer) ||
       !alive(flyer))
      return false;
   uint8_t* const uc = *(uint8_t**)(flyer + kFly_Ctrl);
   return uc && is_rtti(uc, kRttiUnitController);
}

void hold(uint8_t* flyer)
{
   float& t = *(float*)(flyer + kFly_FreeFlyTime);
   if (!(t >= kHoldFreeFly)) t = kHoldFreeFly;
}

void* path_class(const uint8_t* flyer)
{
   uint8_t* const cls = *(uint8_t* const*)(flyer + kFly_Class);
   return cls ? cls + kClass_PathClass : nullptr;
}

// As UpdatePathFollower works them out.
float boost_ratio(uint8_t* flyer)
{
   const uint8_t* cls = *(const uint8_t* const*)(flyer + kFly_Class);
   if (!cls || !(*(const float*)(cls + kClass_BoostSpeed) > 0.0f)) return 0.0f;
   const float mid = s_borrowed.midSpeed(flyer, nullptr);
   return mid > 0.0f ? s_borrowed.maxSpeed(flyer, nullptr) / mid : 0.0f;
}

float energy_ratio(const uint8_t* flyer)
{
   if (flyer[kFly_EnergyFlags] & 1u) return 0.0f;
   const float* bar = *(const float* const*)(flyer + kFly_EnergyClass);
   if (!bar || !(bar[0] > 0.0f)) return 0.0f;
   return *(const float*)(flyer + kFly_Energy) / bar[0];
}

// ---- The carrots ------------------------------------------------------------------------
struct Carrot {
   uint8_t* obj = nullptr;
   uint32_t handle = 0;
   alignas(16) float m[16] = {};   // the matrix the path follower moves in place of the flyer's
   bool     pathless = false;      // its follower found no path: the AI flies it until retryAt
   double   retryAt = 0.0;
   double   seen = 0.0;            // the last update that kept it
   float    speed = 0.0f;          // the path's speed, as the carrot flies it
   float    lead = flyer_real_flight::kMinLead;
   float    distance = 0.0f;       // from the flyer, at the last update
};

Carrot s_carrots[kMaxCarrots];
Stats  s_stats;
int    s_started = 0;
bool   s_full = false;
double s_nextReport = 0.0;
int    s_reports = 0;

int find(const uint8_t* obj)
{
   for (int i = 0; i < kMaxCarrots; ++i)
      if (s_carrots[i].obj == obj) return i;
   return -1;
}

// The carrot of a flyer on real flight, kept by its update just now: -1 for none.
int live(const uint8_t* obj)
{
   const int i = find(obj);
   if (i < 0) return -1;
   const Carrot& c = s_carrots[i];
   if (handle_of(obj) != c.handle || seconds() - c.seen > kStaleSeconds) return -1;
   return i;
}

void forget(int i) { s_carrots[i] = Carrot{}; }

// The carrot from where the flyer is, its follower cleared: the next step
// branches onto a path from here.
void start(Carrot& c, uint8_t* flyer)
{
   s_reset(flyer + kFly_Follower, path_class(flyer));
   if (const float* m = matrix_of(flyer)) std::memcpy(c.m, m, sizeof c.m);
   c.pathless = false;
   c.retryAt = 0.0;
   c.speed = flyer_wingmen::length(velocity_of(flyer));
   c.distance = 0.0f;
}

void report(double now)
{
   if (now < s_nextReport || s_reports >= kMaxReports) return;
   s_nextReport = now + kReportSeconds;
   int onPaths = 0, waiting = 0;
   for (const Carrot& c : s_carrots) {
      if (!c.obj || now - c.seen > kStaleSeconds) continue;
      ++onPaths;
      if (c.pathless) ++waiting;
   }
   Stats& s = s_stats;
   if (onPaths || s.restarts || s.pathless || s_started) {
      const double n = s.samples ? s.samples : 1;
      ++s_reports;
      install_log("[RealFlight] %d AI flyers on real flight (%d waiting for a path, %d started); carrot %.0f m ahead "
                  "on average against a lead of %.0f, %.0f m at worst; %.0f m/s against the path's %.0f; %d carrots "
                  "started over, %d paths not found",
                  onPaths, waiting, s_started, s.distance / n, s.lead / n, (double)s.worst, s.speed / n, s.carrot / n,
                  s.restarts, s.pathless);
   }
   s = Stats{};
   s_started = 0;
}

// One update of the carrot: the follower stepped on it, as far as the flyer's
// distance behind allows.
void step(int i, uint8_t* flyer, float dt, double now)
{
   Carrot& c = s_carrots[i];
   const float* sm = matrix_of(flyer);
   if (!sm || !(dt > 0.0f) || !std::isfinite(dt)) return;
   uint8_t* const follower = flyer + kFly_Follower;
   const V3 ship = row(sm, 3);
   const float shipSpeed = flyer_wingmen::length(velocity_of(flyer));
   V3 carrot = row(c.m, 3);
   c.lead = flyer_real_flight::lead_for(shipSpeed > c.speed ? shipSpeed : c.speed);
   c.distance = flyer_wingmen::length(carrot - ship);
   if (flyer_real_flight::lost(c.distance, c.lead)) {
      start(c, flyer);
      ++s_stats.restarts;
      carrot = row(c.m, 3);
   }
   s_stats.add(c.distance, c.lead, shipSpeed, c.speed);
   const float stepped = dt * flyer_real_flight::pace(c.distance, c.lead);
   if (!(stepped > 0.0f)) return;   // the carrot waits for the flyer

   BranchParams bp;
   ((fn_branch_params_t)(*(void***)follower)[kVt_BranchParams / 4])(follower, &bp);
   bp.matrix = c.m;
   bp.fallbackToClosest = 1;   // as UpdatePathFollower asks
   float velocity[3] = {};
   if (!s_step(follower, &bp, stepped, velocity, nullptr, 0, boost_ratio(flyer), energy_ratio(flyer))) {
      // No path, as UpdatePathFollower takes it: the AI flies it a while.
      c.pathless = true;
      c.retryAt = now + flyer_real_flight::kRetrySeconds;
      s_reset(follower, bp.pathClass);
      ++s_stats.pathless;
      return;
   }
   if (follower[kFol_LandNow]) {
      // A node's LandOnArrival, as UpdatePathFollower takes it.
      *(float*)(flyer + kFly_FreeFlyTime) = kHoldFreeFly;
      s_land(flyer);
      forget(i);
      return;
   }
   c.speed = flyer_real_flight::carrot_speed(c.speed, flyer_wingmen::length(row(c.m, 3) - carrot), stepped, dt);
}

// ---- EntityFlyer::Update's two CALLs ------------------------------------------------------
// To UpdatePathFollower: thiscall(dt, matrix*) on the entity, RET 8, as
// __fastcall with EDX unused. A flyer held in free flight (here, or a wingman in
// formation) that something cut short (TargetReact zeroes it) would be placed
// on its path for this update: it is held again instead, and the rest of the
// update, which reads the free-flight time again, flies it.
void __fastcall update_path_follower_mt(uint8_t* flyer, void*, float dt, float* matrix)
{
   bool held = false;
   __try {
      held = live(flyer) >= 0 || (s_borrowed.wingman && s_borrowed.wingman(flyer));
      if (held) hold(flyer);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      held = false;
   }
   if (!held) s_updatePathFollower(flyer, dt, matrix);
}

// To EntityPathFollower::Reset, made every update a flyer is not on its path:
// thiscall(pathClass) on the follower, RET 4. A flyer on real flight keeps its
// follower: it is stepping the carrot.
void __fastcall free_reset_mt(uint8_t* follower, void*, void* pathClass)
{
   bool keep = false;
   __try {
      keep = live(follower - kFly_Follower) >= 0;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      keep = false;
   }
   if (!keep) s_reset(follower, pathClass);
}

// ---- Install --------------------------------------------------------------------------
bool matches(const uint8_t* at, const char* bytes, size_t len) { return std::memcmp(at, bytes, len) == 0; }

uintptr_t call_destination(const uint8_t* call)
{
   const uint8_t* dest = call + 5 + *(const int32_t*)(call + 1);
   if (dest[0] == 0xE9) dest = dest + 5 + *(const int32_t*)(dest + 1);
   return (uintptr_t)dest;
}

void redirect(Site& site, uint8_t* at, const void* to)
{
   site.at = at;
   std::memcpy(site.orig, at, 5);
   *(int32_t*)(at + 1) = (int32_t)((intptr_t)to - (intptr_t)(at + 5));
}

} // namespace

namespace flyer_real_flight {

bool on_property(void* cls, uint32_t hash, const char* value)
{
   if (hash != kHashRealFlight) return false;
   bool on;
   if (cls && read_switch(value, on)) s_classes[cls] = on;
   return true;   // no stock SetProperty knows it
}

void on_derive(const void* parent, void* child)
{
   const auto it = s_classes.find(parent);
   if (it != s_classes.end()) s_classes[child] = it->second;
   else s_classes.erase(child);
}

void on_update(uint8_t* flyer, float dt)
{
   if (!s_installed) return;
   const double now = seconds();
   int i = find(flyer);
   if (i >= 0 && s_carrots[i].handle != handle_of(flyer)) {   // the address of a flyer that is gone
      forget(i);
      i = -1;
   }
   if (!flies_for_real(flyer)) {
      // The stock code has it again: its follower resets while it flies free,
      // and it branches onto a path from where it is.
      if (i >= 0) forget(i);
      return;
   }
   if (i < 0) {
      i = find(nullptr);
      if (i < 0) {
         if (!s_full) install_log("[RealFlight] more than %d flyers on real flight: the rest are placed", kMaxCarrots);
         s_full = true;
         return;
      }
      s_carrots[i].obj = flyer;
      s_carrots[i].handle = handle_of(flyer);
      start(s_carrots[i], flyer);
      ++s_started;
   }
   Carrot& c = s_carrots[i];
   c.seen = now;
   hold(flyer);
   if (c.pathless) {
      if (now < c.retryAt) return;   // the AI flies it meanwhile
      start(c, flyer);
   }
   step(i, flyer, dt, now);
   report(now);
}

void release(const uint8_t* flyer)
{
   const int i = find(flyer);
   if (i >= 0) forget(i);
}

bool steer_before(uint8_t* flyer, Steer& st)
{
   if (!s_installed) return false;
   const int i = live(flyer);
   if (i < 0) return false;
   const Carrot& c = s_carrots[i];
   if (c.pathless) return false;
   const V3 point = row(c.m, 3);
   V3 heading = row(c.m, 2);
   if (!flyer_wingmen::finite(point) || !flyer_wingmen::normalise(heading)) return false;

   const float hi = s_borrowed.maxSpeed(flyer, nullptr);
   const float lo = s_borrowed.minSpeed(flyer, nullptr);
   const float mid = s_borrowed.midSpeed(flyer, nullptr);
   st.target = flyer_wingmen::target_for_speed(flyer_speed(c.speed, c.distance, c.lead, lo, hi), lo, mid, hi);

   uint8_t* const dest = *(uint8_t**)(flyer + kFly_Ctrl) + kUC_LowLevel + kLL_Dest;
   std::memcpy(st.saved, dest, kDestSize);
   st.dest = dest;   // from here on the AI's own goes back, whatever happens
   st.dest[kDest_Has] = 1;
   std::memcpy(st.dest + kDest_Pos, &point, 12);
   std::memcpy(st.dest + kDest_Original, &point, 12);
   std::memcpy(st.dest + kDest_Dir, &heading, 12);
   *(int*)(st.dest + kDest_Safe) = kSafeHeightFull;
   *(float*)(st.dest + kDest_Taper) = 0.0f;
   *(float*)(st.dest + kDest_Speed) = st.target;
   std::memcpy(st.point, &point, 12);
   st.carrot = i;
   return true;
}

void restore(Steer& st)
{
   if (st.dest) std::memcpy(st.dest, st.saved, kDestSize);
   st.dest = nullptr;
}

void steer_after(uint8_t* flyer, Steer& st, bool steered, const float* point, float* speed)
{
   restore(st);   // the AI's own destination back
   if (!steered || !speed || (flyer[kFly_Flags] & kFlagSliding)) return;
   // EntityFlyer::RaiseHeight lifted the point over terrain or something in the
   // way (slowing to 20 m/s for the latter): the stock speed stands.
   if (point && point[1] > st.point[1] + kRaisedBy) return;
   const float* m = matrix_of(flyer);
   if (!m) return;
   V3 to = V3{st.point[0], st.point[1], st.point[2]} - row(m, 3);
   const float cosOff = flyer_wingmen::normalise(to) ? flyer_wingmen::dot(to, row(m, 2)) : 1.0f;
   *speed = flyer_wingmen::keep_speed(*speed, st.target, cosOff);
}

bool install(uintptr_t base, const Borrowed& borrowed)
{
   const auto* a = g_addr;
   if (g_build != GameBuild::Modtools || !a->flyer_path_step || !a->flyer_update_path_follower ||
       !a->flyer_update_path_call || !a->flyer_free_reset_call || !a->path_follower_reset || !a->entity_flyer_land ||
       !borrowed.maxSpeed || !borrowed.minSpeed || !borrowed.midSpeed) {
      install_log("[RealFlight] NOT installed: no address set for this build");
      return false;
   }
   const uint8_t* const upf = (const uint8_t*)resolve(base, a->flyer_update_path_follower);
   const uint8_t* const stepFn = (const uint8_t*)resolve(base, a->flyer_path_step);
   const uint8_t* const resetFn = (const uint8_t*)resolve(base, a->path_follower_reset);
   const uint8_t* const landFn = (const uint8_t*)resolve(base, a->entity_flyer_land);
   uint8_t* const upfCall = (uint8_t*)resolve(base, a->flyer_update_path_call);
   uint8_t* const resetCall = (uint8_t*)resolve(base, a->flyer_free_reset_call);
   const bool ok =
      // UpdatePathFollower: the follower at +0x3CC, GetBranchParams at its vptr +0x1C into
      // params at [esp+0x38] ...
      matches(upf, "\x83\xEC\x50\x53\x56\x8B\xF1", 7) &&
      matches(upf + 0x59, "\x8B\x86\xCC\x03\x00\x00\x57\x8D\x4C\x24\x38\x8D\xBE\xCC\x03\x00\x00\x51\x8B\xCF\xFF\x50\x1C",
              23) &&
      // ... their mFallbackToClosest ([esp+0x59]) set, the class at +0x66C and its boost speed at +0x898 ...
      matches(upf + 0x70, "\x8B\x96\x6C\x06\x00\x00\x88\x5C\x24\x59\xD9\x82\x98\x08\x00\x00", 16) &&
      // ... the energy bar ...
      matches(upf + 0xB1, "\x84\x9E\x20\x06\x00\x00\x75\x23\x8B\x8E\x18\x06\x00\x00", 14) &&
      matches(upf + 0xCE, "\xD9\x86\x1C\x06\x00\x00", 6) &&
      // ... its CALL to the step ...
      upf[0x108] == 0xE8 && call_destination(upf + 0x108) == (uintptr_t)stepFn &&
      // ... and mbLandNow.
      matches(upf + 0x26F, "\x8A\x86\xA1\x04\x00\x00", 6) &&
      matches(stepFn, "\x55\x8B\xEC\x83\xE4\xF0\x81\xEC\xF4\x00\x00\x00", 12) &&
      matches(resetFn, "\x8B\x44\x24\x04\x56\x50\x8B\xF1", 8) &&
      // Land: the LandOnArrival fix may have detoured it already.
      (matches(landFn, "\x56\x8B\xF1\x8B\x86\xA4\x05\x00\x00", 9) || landFn[0] == 0xE9) &&
      // EntityFlyer::Update: the entity into ECX, the CALL to UpdatePathFollower, then
      // the same test again, the free-flight time read afresh ...
      matches(upfCall - 6, "\x8D\x8B\xC0\xFD\xFF\xFF", 6) && upfCall[0] == 0xE8 &&
      call_destination(upfCall) == (uintptr_t)upf && matches(upfCall + 5, "\x8A\x8B\xB4\x03\x00\x00\xF6\xC1\x08", 9) &&
      matches(upfCall + 0x24, "\xD9\x83\x9C\x1A\x00\x00", 6) &&
      // ... and, off the path, the follower into ECX and the CALL to Reset.
      matches(resetCall - 6, "\x8D\x8B\x8C\x01\x00\x00", 6) && resetCall[0] == 0xE8 &&
      call_destination(resetCall) == (uintptr_t)resetFn;
   if (!ok) {
      install_log("[RealFlight] NOT installed: the path follower or EntityFlyer's update differ");
      return false;
   }
   LARGE_INTEGER f;
   QueryPerformanceFrequency(&f);
   s_ticksPerSecond = (double)f.QuadPart;
   s_borrowed = borrowed;
   s_step = (fn_step_t)stepFn;
   s_reset = (fn_reset_t)resetFn;
   s_land = (fn_land_t)landFn;
   s_updatePathFollower = (fn_upf_t)upf;

   // The calls, during the installers' writable window.
   redirect(s_upfSite, upfCall, (const void*)&update_path_follower_mt);
   redirect(s_resetSite, resetCall, (const void*)&free_reset_mt);
   s_installed = true;
   install_log("[RealFlight] installed (prototype): %s",
               g_flyerRealFlight ? "every AI flyer that flies paths chases them by physics"
                                 : "off but for flyer classes with PathFollowerRealFlight = 1");
   return true;
}

void uninstall()
{
   for (Site* s : {&s_upfSite, &s_resetSite}) {
      if (s->at) protected_write(s->at, s->orig, 5);
      *s = Site{};
   }
   s_installed = false;
   for (int i = 0; i < kMaxCarrots; ++i) forget(i);
   s_classes.clear();
}

} // namespace flyer_real_flight
