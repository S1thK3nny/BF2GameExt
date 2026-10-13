#pragma once

#include <cmath>

// =============================================================================
// Wingmen for a player's flyer, on their own: where each wingman's slot is, the
// point it is steered at, the speed it is asked for, and who may fly as one. No
// engine reads, so tests/flyer_wingmen_tests.cpp can exercise all of it. The
// reads and calls around it are in flyer_wingmen.hpp.
//
// Formation space: x to the leader's right, y up, z ahead, in metres, the
// leader at the origin and its roll left out, so a banking leader does not tip
// its wingmen into the ground.
// =============================================================================

namespace flyer_wingmen {

struct V3 { float x = 0.0f, y = 0.0f, z = 0.0f; };

inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(V3 a) { return std::sqrt(dot(a, a)); }
inline bool finite(V3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

inline bool normalise(V3& v)
{
   const float len = length(v);
   if (!(len > 1e-4f) || !std::isfinite(len)) return false;
   v = v * (1.0f / len);
   return true;
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- The formation ------------------------------------------------------------
constexpr int   kMaxWingmen     = 4;
constexpr int   kDefaultWingmen = 3;
constexpr float kDefaultSpacing = 25.0f;   // m between neighbours
constexpr float kMinSpacing     = 8.0f;
constexpr float kMaxSpacing     = 60.0f;

inline float clamp_spacing(float s) { return std::isfinite(s) ? clampf(s, kMinSpacing, kMaxSpacing) : kDefaultSpacing; }
inline int clamp_count(int n) { return n < 1 ? 1 : n > kMaxWingmen ? kMaxWingmen : n; }

// A vee: slot 0 back on the right, slot 1 back on the left, then the next pair
// a row further back, each a full spacing from its neighbours.
inline V3 slot_offset(int slot, float spacing)
{
   if (slot < 0) slot = 0;
   const float s = clamp_spacing(spacing);
   constexpr float kDiagonal = 0.70710678f;   // 45 degrees back
   const float row = (float)(slot / 2 + 1);
   const float side = slot % 2 == 0 ? 1.0f : -1.0f;
   return {side * row * s * kDiagonal, 0.0f, -row * s * kDiagonal};
}

// The leader's frame without its roll: ahead the way it points, right level
// with the ground (left-handed, y up: the right of +z is +x), up square to
// both. Pointing straight up or down there is no level right, so its own right
// (`fallbackRight`, from its matrix) stands in.
struct Frame {
   V3 right, up, fwd;
   bool valid = false;
};

inline Frame level_frame(V3 forward, V3 fallbackRight)
{
   Frame f;
   f.fwd = forward;
   if (!normalise(f.fwd)) return f;
   f.right = {f.fwd.z, 0.0f, -f.fwd.x};
   if (!normalise(f.right)) {
      f.right = fallbackRight - f.fwd * dot(fallbackRight, f.fwd);
      if (!normalise(f.right)) return f;
   }
   f.up = cross(f.fwd, f.right);
   f.valid = normalise(f.up);
   return f;
}

inline V3 to_world(const Frame& f, V3 origin, V3 offset)
{
   return origin + f.right * offset.x + f.up * offset.y + f.fwd * offset.z;
}

// Where a point lies from `from` in the leader's frame: x right, y up, z ahead.
inline V3 in_frame(const Frame& f, V3 from, V3 to)
{
   const V3 d = to - from;
   return {dot(d, f.right), dot(d, f.up), dot(d, f.fwd)};
}

// ---- Steering -------------------------------------------------------------------
// The flight code turns a flyer toward a point and slows it once the point is
// off its nose. A wingman is steered at its slot carried on by the leader's
// velocity for kLeadSeconds, a point ahead of it while it holds station, so it
// flies along with the formation rather than at a spot it would overrun.
constexpr float kLeadSeconds = 0.5f;

inline V3 steer_point(V3 slot, V3 leaderVelocity)
{
   return slot + leaderVelocity * kLeadSeconds;
}

// The speed it is asked for, m/s: the leader's, plus kCatchUp for every metre
// its slot is ahead of it (less for every metre behind), within what it can fly.
constexpr float kCatchUp = 0.5f;   // m/s per metre

inline float formation_speed(float leaderSpeed, float slotAhead, float minSpeed, float maxSpeed)
{
   if (!std::isfinite(leaderSpeed) || !std::isfinite(slotAhead) || !(maxSpeed > 0.0f)) return maxSpeed > 0.0f ? maxSpeed : 0.0f;
   const float lo = minSpeed > 0.0f && minSpeed < maxSpeed ? minSpeed : 0.0f;
   return clampf(leaderSpeed + kCatchUp * slotAhead, lo, maxSpeed);
}

// The game's own unit for it: a share of the flyer's top speed
// (AIUtil::GetTargetSpeedForRealSpeed: speed / max, within 0 to 1).
inline float target_for(float speed, float maxSpeed)
{
   if (!(maxSpeed > 0.0f) || !std::isfinite(speed)) return 1.0f;
   return clampf(speed / maxSpeed, 0.0f, 1.0f);
}

// The flight code all but stops a flyer whose point is more than ~26 degrees
// off its nose. A wingman following the formation round a turn keeps most of
// its speed until the point is kKeepSpeedCos off the nose; past that it slows
// to turn, as the stock code would have it.
constexpr float kKeepSpeedCos = 0.5f;    // 60 degrees
constexpr float kKeepShare    = 0.85f;

inline float keep_speed(float stockTarget, float formationTarget, float offNoseCos)
{
   if (!std::isfinite(offNoseCos) || offNoseCos < kKeepSpeedCos) return stockTarget;
   const float floor = formationTarget * kKeepShare;
   return stockTarget > floor ? stockTarget : floor;
}

// ---- Who flies as one -------------------------------------------------------------
// Only a flyer that can keep up: its top speed at least kKeepUp of the leader's.
constexpr float kKeepUp       = 0.9f;
constexpr float kRecruitRange = 300.0f;   // m: friendlies this near join
constexpr float kKeepRange    = 800.0f;   // m: a wingman further off than this is let go

inline bool can_keep_up(float wingmanMax, float leaderMax)
{
   return leaderMax > 0.0f && std::isfinite(wingmanMax) && wingmanMax >= kKeepUp * leaderMax;
}

// ---- How well it holds its slot, for the log ----------------------------------------
struct Stats {
   int    samples = 0;
   double distance = 0.0, ahead = 0.0, right = 0.0, up = 0.0, speed = 0.0, leaderSpeed = 0.0;
   float  worst = 0.0f;

   // `slot` is where the slot lies from the wingman, in the leader's frame.
   void add(V3 slot, float wingmanSpeed, float leader)
   {
      if (!finite(slot) || !std::isfinite(wingmanSpeed) || !std::isfinite(leader)) return;
      const float d = length(slot);
      ++samples;
      distance += d;
      ahead += slot.z;
      right += slot.x;
      up += slot.y;
      speed += wingmanSpeed;
      leaderSpeed += leader;
      if (d > worst) worst = d;
   }
};

} // namespace flyer_wingmen
