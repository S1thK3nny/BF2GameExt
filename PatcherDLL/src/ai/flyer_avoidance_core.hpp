#pragma once

#include "flyer_wingmen_core.hpp"   // V3 and its operations

#include <cmath>

// =============================================================================
// Flyer-to-flyer avoidance, on its own: whether two flyers will pass too close
// within the next couple of seconds, and how an AI flyer's steering point and
// speed are bent away from it. No engine reads, so tests/flyer_avoidance_tests.cpp
// can exercise all of it. The reads and calls around it are in
// flyer_avoidance.hpp.
//
// The game keeps AI flyers off barriers, danger regions and the ground, never
// off each other (docs/RE/FlyerAI.md). Every AI flyer the AI flies by physics
// looks at every other flyer in the air, AI or not: where each will be at their
// closest within kHorizon, assuming both hold their velocities. One that will
// pass inside the two flyers' radii plus kMargin is a threat; the flyer steers
// away from where it will be then, square to its own heading, the harder the
// nearer and the sooner, and eases off behind a slower one going its way. Dead
// ahead on a collision course both break right; one overtaken on its line of
// flight makes way to the left.
// =============================================================================

namespace flyer_avoidance {

using flyer_wingmen::clampf;
using flyer_wingmen::dot;
using flyer_wingmen::finite;
using flyer_wingmen::length;
using flyer_wingmen::normalise;
using flyer_wingmen::V3;

constexpr float kHorizon   = 2.0f;     // s looked ahead
constexpr float kMargin    = 8.0f;     // m kept between the two flyers' spheres
constexpr float kMinRadius = 3.0f;     // m: a flyer's collision sphere, kept within these
constexpr float kMaxRadius = 40.0f;
constexpr float kScanRange = 400.0f;   // m: flyers further off are not looked at
constexpr float kGain      = 1.5f;     // how far a full threat bends its heading (1: 45 degrees square on)
constexpr float kMaxPush   = 1.5f;     // threats added together, at most this
constexpr float kMinAhead  = 60.0f;    // m: the bent point at least this far out
constexpr float kBrake     = 0.4f;     // up to this share of its speed target shed behind a slower one
constexpr float kSameWay   = 0.5f;     // cosine: the other heading this near its own is going its way

inline float clamp_radius(float r) { return std::isfinite(r) ? clampf(r, kMinRadius, kMaxRadius) : kMinRadius; }

struct Threat {
   float urgency = 0.0f;   // 0 to 1: the nearer and the sooner, the higher
   V3    away;             // the way out, square to its heading
   float brake = 0.0f;     // 0 to kBrake: a slower flyer ahead going its way
   float pass = 0.0f;      // m between the two at their closest
   bool  aheadSameWay = false;   // the other ahead of it, going its way
};

// The other flyer, `other` with `otherVel`, against this one at `me` with
// `myVel`, heading `fwd` with `right` to its right. True, with `t` filled, when
// they will pass within their radii plus kMargin inside kHorizon.
inline bool assess(V3 me, V3 myVel, float myRadius, V3 fwd, V3 right, V3 other, V3 otherVel, float otherRadius,
                   Threat& t)
{
   if (!finite(me) || !finite(myVel) || !finite(other) || !finite(otherVel) || !finite(fwd)) return false;
   const V3 r = other - me;          // where it is, from this one
   const V3 w = otherVel - myVel;    // how it moves, from this one
   const float ww = dot(w, w);
   float tc = ww > 1e-4f ? -dot(r, w) / ww : 0.0f;
   tc = std::isfinite(tc) ? clampf(tc, 0.0f, kHorizon) : 0.0f;
   const V3 c = r + w * tc;          // where it is at the closest
   const float pass = length(c);
   const float limit = clamp_radius(myRadius) + clamp_radius(otherRadius) + kMargin;
   if (!(pass < limit)) return false;

   // Away from where it will be, square to this one's heading. On its line of
   // flight: to the right with the other ahead (two meeting head on both go
   // right and pass), to the left with it behind (one being overtaken makes way
   // the other side).
   V3 away = c * -1.0f;
   away = away - fwd * dot(away, fwd);
   if (!normalise(away)) away = dot(r, fwd) < 0.0f ? right * -1.0f : right;
   if (!normalise(away)) return false;

   t.urgency = (1.0f - pass / limit) * (1.0f - 0.5f * tc / kHorizon);
   t.away = away;
   t.pass = pass;
   // Ahead of it, going its way, and closing from behind: ease off too.
   V3 otherFwd = otherVel;
   const bool sameWay = normalise(otherFwd) && dot(otherFwd, fwd) > kSameWay;
   t.aheadSameWay = dot(r, fwd) > 0.0f && sameWay;
   t.brake = t.aheadSameWay && dot(w, fwd) < -1.0f ? kBrake * t.urgency : 0.0f;
   return true;
}

// Single file (one of the player's wingmen on its leader's trail, flyer_wingmen.cpp):
// a flyer ahead of it going its way is not passed round, which would push it off
// the path into what is beside it (a hangar's rim), but kept behind: no push to
// the side, and its speed eased by the threat's urgency (kBrake at most).
inline void single_file(Threat& t)
{
   if (!t.aheadSameWay) return;
   t.away = V3{};
   const float b = kBrake * t.urgency;
   if (b > t.brake) t.brake = b;
}

// The threats together.
struct Avoid {
   V3    push;
   float urgency = 0.0f;   // the worst one's
   float brake = 0.0f;
   float pass = -1.0f;     // the nearest pass, m
   int   threats = 0;
};

inline void add(Avoid& a, const Threat& t)
{
   a.push = a.push + t.away * t.urgency;
   if (t.urgency > a.urgency) a.urgency = t.urgency;
   if (t.brake > a.brake) a.brake = t.brake;
   if (a.pass < 0.0f || t.pass < a.pass) a.pass = t.pass;
   ++a.threats;
}

// The point it steers at, bent away: the way to `point` turned by the push, at
// least kMinAhead out. `mayDescend` false (the game lifted the point over
// terrain or something in its way) keeps it from bending any lower.
inline V3 bend(V3 me, V3 point, V3 fwd, const Avoid& a, bool mayDescend)
{
   if (!a.threats || !finite(point) || !finite(me)) return point;
   V3 to = point - me;
   const float reach = length(to);
   if (!normalise(to) && !normalise(to = fwd)) return point;
   V3 push = a.push;
   const float p = length(push);
   if (p > kMaxPush) push = push * (kMaxPush / p);
   V3 dir = to + push * kGain;
   if (!normalise(dir)) return point;
   if (!mayDescend && dir.y < to.y) {
      // Bent to the side only, at the climb it had.
      V3 flat{dir.x, 0.0f, dir.z};
      const float level = std::sqrt(clampf(1.0f - to.y * to.y, 0.0f, 1.0f));
      dir = normalise(flat) ? V3{flat.x * level, to.y, flat.z * level} : to;
   }
   const float out = std::isfinite(reach) && reach > kMinAhead ? reach : kMinAhead;
   return me + dir * out;
}

// The speed target (0 to 1, as CalculateDest leaves it), eased off behind a
// slower flyer going its way.
inline float braked(float target, const Avoid& a)
{
   if (!std::isfinite(target) || !(a.brake > 0.0f)) return target;
   return target * (1.0f - clampf(a.brake, 0.0f, kBrake));
}

// ---- For the log ----------------------------------------------------------------
struct Stats {
   int   bent = 0;         // updates a flyer's steering was bent
   int   braked = 0;       // ... and its speed eased
   float nearest = -1.0f;  // the nearest pass foreseen, m
   float worst = 0.0f;     // the highest urgency

   void add(const Avoid& a)
   {
      if (!a.threats) return;
      ++bent;
      if (a.brake > 0.0f) ++braked;
      if (a.pass >= 0.0f && (nearest < 0.0f || a.pass < nearest)) nearest = a.pass;
      if (a.urgency > worst) worst = a.urgency;
   }
};

} // namespace flyer_avoidance
