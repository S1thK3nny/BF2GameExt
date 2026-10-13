#include "pch.h"
#include "flyer_avoidance.hpp"
#include "flyer_avoidance_core.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <cstring>

// See flyer_avoidance.hpp for the behaviour and the reads.

bool g_flyerAvoidance = false;

namespace {

using flyer_avoidance::Avoid;
using flyer_avoidance::Stats;
using flyer_avoidance::Threat;
using flyer_wingmen::V3;

// ---- Modtools -----------------------------------------------------------------
constexpr uint32_t kFly_Follower     = 0x3CC;   // the flyer's EntityPathFollower, a node of the list
constexpr uint32_t kFly_State        = 0x5A4;
constexpr uint32_t kFly_SphereStack  = 0x10;    // its TreeGridObject (+0xC): the sphere stack ...
constexpr uint32_t kFly_SphereIndex  = 0x14;    // ... its place in it ...
constexpr uint32_t kFly_SphereRadius = 0x24;    // ... or, with none, its own sphere's radius
constexpr uint32_t kStack_Radius     = 0x4C;    // a stack's sphere radius, 16 bytes a sphere
constexpr uint32_t kList_Next        = 0x04;    // the follower list's nodes
constexpr uint32_t kList_Follower    = 0x0C;
constexpr uint32_t kList_Count       = 0x10;    // the head's count
constexpr int      kStateTakeoff     = 1;
constexpr int      kStateLanding     = 3;
constexpr uint32_t kDest_PosY        = 0x08;    // ControllableDestination::mPos.y

// ---- Any build ------------------------------------------------------------------
constexpr uint32_t kVt_GetVelocity  = 0x44;   // GameObject
constexpr uint32_t kGO_MatrixIface  = 0xC;    // the interface a GameObject's matrix comes from ...
constexpr uint32_t kVt_GetMatrix    = 0x40;   // ... at its vptr +0x40
constexpr uint32_t kGO_Flags        = 0x1FC;  // bit 3: alive

constexpr int    kMaxBodies     = 256;
constexpr int    kMaxNodes      = 4096;
constexpr double kStaleSeconds  = 0.003;   // one read of the flyers serves every flyer's update in a frame
constexpr double kReportSeconds = 5.0;
constexpr int    kMaxReports    = 400;

using fn_get_vec_t = const float*(__thiscall*)(void* self);

uint8_t* s_list = nullptr;   // the follower list's head node
bool     s_installed = false;
double   s_ticksPerSecond = 0.0;

struct Body {
   uint8_t* obj = nullptr;
   V3       pos, vel;
   float    radius = 0.0f;
};
Body   s_bodies[kMaxBodies];
int    s_count = 0;
double s_readAt = -1.0;

Stats  s_stats;
int    s_peak = 0;   // the most flyers in the air at once since the last report
double s_nextReport = 0.0;
int    s_reports = 0;

double seconds()
{
   LARGE_INTEGER t;
   QueryPerformanceCounter(&t);
   return (double)t.QuadPart / s_ticksPerSecond;
}

bool alive(const uint8_t* obj) { return ((*(const uint32_t*)(obj + kGO_Flags)) >> 3) & 1u; }

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

// Its collision sphere's radius, as RaiseHeight reads one.
float radius_of(const uint8_t* obj)
{
   const uint8_t* const stack = *(const uint8_t* const*)(obj + kFly_SphereStack);
   if (stack) return *(const float*)(stack + *(const int*)(obj + kFly_SphereIndex) * 16 + kStack_Radius);
   return *(const float*)(obj + kFly_SphereRadius);
}

// Every flyer in the air, once a frame.
void read_flyers(double now)
{
   if (now >= s_readAt && now - s_readAt < kStaleSeconds) return;
   s_readAt = now;
   s_count = 0;
   uint8_t* node = *(uint8_t**)(s_list + kList_Next);
   for (int i = 0; i < kMaxNodes && node && node != s_list && s_count < kMaxBodies; ++i) {
      uint8_t* const follower = *(uint8_t**)(node + kList_Follower);
      if (!follower) break;   // the head: the list's end
      node = *(uint8_t**)(node + kList_Next);
      uint8_t* const obj = follower - kFly_Follower;
      const int state = *(const int*)(obj + kFly_State);
      if (state < kStateTakeoff || state > kStateLanding || !alive(obj)) continue;
      const float* m = matrix_of(obj);
      if (!m) continue;
      Body& b = s_bodies[s_count++];
      b.obj = obj;
      b.pos = row(m, 3);
      b.vel = velocity_of(obj);
      b.radius = flyer_avoidance::clamp_radius(radius_of(obj));
   }
   if (s_count > s_peak) s_peak = s_count;
}

void report(double now)
{
   if (now < s_nextReport || s_reports >= kMaxReports) return;
   s_nextReport = now + kReportSeconds;
   if (s_stats.bent) {
      ++s_reports;
      install_log("[Avoidance] in %.0f s: %d flyer updates steered clear of another flyer (%d also eased off); the "
                  "nearest pass foreseen %.1f m, the most urgent %.2f; up to %d flyers in the air",
                  kReportSeconds, s_stats.bent, s_stats.braked, (double)s_stats.nearest, (double)s_stats.worst, s_peak);
   }
   s_stats = Stats{};
   s_peak = 0;
}

bool matches(const uint8_t* at, const uint8_t* bytes, size_t len) { return std::memcmp(at, bytes, len) == 0; }

} // namespace

namespace flyer_avoidance {

bool install(uintptr_t base)
{
   const auto* a = g_addr;
   if (g_build != GameBuild::Modtools || !a->flyer_follower_list || !a->flyer_follower_link || !a->flyer_raise_ray_call) {
      if (g_flyerAvoidance) install_log("[Avoidance] NOT installed: no address set for this build");
      return false;
   }
   uint8_t* const list = (uint8_t*)resolve(base, a->flyer_follower_list);
   const uint8_t* const link = (const uint8_t*)resolve(base, a->flyer_follower_link);
   const uint8_t* const sphere = (const uint8_t*)resolve(base, a->flyer_raise_ray_call) - 0x45;
   // The follower constructor's link-in: INC [count]; MOV [EDI+0xC], ESI; MOV [EDI], head; MOV [EDI+4], head.
   uint8_t linkIn[22] = {0xFF, 0x05, 0, 0, 0, 0, 0x89, 0x77, 0x0C, 0xC7, 0x07, 0, 0, 0, 0, 0xC7, 0x47, 0x04, 0, 0, 0, 0};
   const uint32_t head = (uint32_t)(uintptr_t)list, count = head + kList_Count;
   std::memcpy(linkIn + 2, &count, 4);
   std::memcpy(linkIn + 11, &head, 4);
   std::memcpy(linkIn + 18, &head, 4);
   // RaiseHeight's reads of a flyer's sphere: the stack +0x10, its index +0x14 (16 bytes a
   // sphere, the radius at +0x4C), else its own radius +0x24.
   static const uint8_t kSphere[] = {0x8B, 0x47, 0x10, 0x85, 0xC0, 0x74, 0x0C, 0x8B, 0x4F, 0x14, 0xC1,
                                     0xE1, 0x04, 0xD9, 0x44, 0x01, 0x4C, 0xEB, 0x03, 0xD9, 0x47, 0x24};
   if (!matches(link, linkIn, sizeof linkIn) || !matches(sphere, kSphere, sizeof kSphere)) {
      install_log("[Avoidance] NOT installed: the flyer list or a flyer's collision sphere differ");
      return false;
   }
   LARGE_INTEGER f;
   QueryPerformanceFrequency(&f);
   s_ticksPerSecond = (double)f.QuadPart;
   s_list = list;
   s_installed = true;
   install_log("[Avoidance] installed (prototype): %s", g_flyerAvoidance
                                                          ? "every AI flyer steers clear of the flyers around it"
                                                          : "off ([AI] FlyerAvoidance=1 to enable)");
   return true;
}

void uninstall()
{
   s_installed = false;
   s_list = nullptr;
   s_count = 0;
}

bool active() { return g_flyerAvoidance && s_installed; }

void adjust(uint8_t* flyer, const uint8_t* dest, float* point, float* speed)
{
   if (!active() || !flyer || !point) return;
   const double now = seconds();
   read_flyers(now);
   const float* m = matrix_of(flyer);
   if (!m) return;
   const V3 me = row(m, 3);
   V3 fwd = row(m, 2), right = row(m, 0);
   if (!flyer_wingmen::normalise(fwd) || !flyer_wingmen::normalise(right)) return;
   const V3 vel = velocity_of(flyer);
   const float radius = clamp_radius(radius_of(flyer));
   Avoid avoid;
   for (int i = 0; i < s_count; ++i) {
      const Body& b = s_bodies[i];
      if (b.obj == flyer) continue;
      const V3 to = b.pos - me;
      if (!(flyer_wingmen::dot(to, to) <= kScanRange * kScanRange)) continue;
      Threat t;
      if (assess(me, vel, radius, fwd, right, b.pos, b.vel, b.radius, t)) add(avoid, t);
   }
   s_stats.add(avoid);
   report(now);
   if (!avoid.threats) return;
   // CalculateDest lifted the point over the terrain or something in the way:
   // bend it to the side only.
   const bool lifted = dest && point[1] > *(const float*)(dest + kDest_PosY) + 1.0f;
   const V3 bent = bend(me, {point[0], point[1], point[2]}, fwd, avoid, !lifted);
   point[0] = bent.x;
   point[1] = bent.y;
   point[2] = bent.z;
   if (speed) *speed = braked(*speed, avoid);
}

} // namespace flyer_avoidance
