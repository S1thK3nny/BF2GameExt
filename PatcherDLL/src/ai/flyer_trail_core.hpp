#pragma once

#include "flyer_wingmen_core.hpp"

#include <limits>

// =============================================================================
// The leader's trail, on its own: the path the player's flyer flew, as points,
// for a wingman to fly where a wall stands between it and where it is going
// (into and out of a hangar, round a building). No engine reads, so
// tests/flyer_wingmen_tests.cpp can exercise all of it; flyer_wingmen.cpp
// records the leader, looks along the lines with rays and steers.
//
// The user (2026-10-14), asked how wingmen out in open space could follow into a
// hangar reliably: "Possibly? Not got a clue how that would be done reliably
// however", then "Yep, get them done". It needs nothing from the map: a wingman
// only flies through where the leader's flyer has been.
//
//   - Recording: a point each time the leader has flown kStep (record), the last
//     kPoints kept, about 2 km; a jump of kJump or more at once (a respawn, a
//     new flyer) is not flown, so the trail starts again there.
//   - Taking it: a wingman whose line to its slot (or, landing, to its place) is
//     blocked takes the trail at the point nearest it (nearest), if that is
//     within kNear and in a clear line (takes).
//   - Flying it: it steers at the furthest of the points kCuts ahead it has a
//     clear line to (cut_candidates), else the next, at the speed that keeps it
//     at its place in a line behind the leader, kGap apart (behind_place), as
//     the formation's speed keeps it at its slot.
//   - Leaving it: its line clear kLeaveAfter looks in a row (leaves).
//   - Stuck: no further point for kStuckFor while its place is ahead (stuck); it
//     is left to its own AI for kWaitFor, then tries again. It never grinds on
//     into a wall.
//   - Under a roof (a ray up from the leader, the landing's roof test) the
//     slots close up to kSqueeze of their spacing, kSqueezeRate a second
//     (squeeze), so more of them fit inside.
//   - The game's lift off (lift_off): under a roof itself, or with the leader
//     under one and its line clear as looked within kFreshLook, a wingman is not
//     sent up over what is ahead, as on the trail. The user, of hangars after
//     the first play: "they seem to panic in tight spaces".
// =============================================================================

namespace flyer_trail {

using flyer_wingmen::V3;

constexpr float  kStep        = 8.0f;     // m flown between points
constexpr int    kPoints      = 256;      // kept: about 2 km
constexpr float  kJump        = 100.0f;   // m from the newest point at once: not flown
constexpr float  kNear        = 300.0f;   // m: a wingman further than this from every point does not take it
constexpr int    kWindow      = 64;       // points past its own that a wingman's place is looked for in
constexpr double kLookEvery   = 0.25;     // s between looks along a wingman's lines
constexpr int    kLeaveAfter  = 3;        // clear looks in a row: back to its slot
constexpr double kStuckFor    = 6.0;      // s without a further point, its place ahead: it waits ...
constexpr double kWaitFor     = 10.0;     // ... left to its own AI this long
constexpr float  kSqueeze     = 0.5f;     // its slots' share of their spacing under a roof ...
constexpr float  kSqueezeRate = 0.5f;     // ... reached at this much a second
constexpr int    kCuts[]      = {16, 8, 4, 2};   // points ahead it may cut to, the furthest first
constexpr int    kMaxCuts     = 4;
constexpr double kFreshLook   = 1.0;      // s: a look along its line older than this says nothing

// The points, oldest first. Their numbers count up for good, so a number a
// wingman holds is never another point's after the trail starts again.
struct Trail {
   V3    at[kPoints];
   float along[kPoints] = {};   // m flown to each from the trail's first
   int   first = 0;             // the oldest point's number
   int   count = 0;
};

inline int   index_of(int n) { return ((n % kPoints) + kPoints) % kPoints; }
inline int   newest(const Trail& t) { return t.first + t.count - 1; }
inline bool  has(const Trail& t, int n) { return t.count > 0 && n >= t.first && n <= newest(t); }
inline V3    point(const Trail& t, int n) { return t.at[index_of(n)]; }
inline float along(const Trail& t, int n) { return t.along[index_of(n)]; }
inline int   next_of(const Trail& t, int n) { return n < newest(t) ? n + 1 : newest(t); }

// No points; the next one's number comes after every number given out.
inline void clear(Trail& t)
{
   t.first = newest(t) + 1;
   t.count = 0;
}

// The leader at `p`: a point once it has flown kStep past the newest. True when
// one was added.
inline bool record(Trail& t, V3 p)
{
   if (!flyer_wingmen::finite(p)) return false;
   float flown = 0.0f;
   if (t.count > 0) {
      const int last = newest(t);
      const float d = flyer_wingmen::length(p - point(t, last));
      if (!(d < kJump)) {
         clear(t);   // not flown: it starts again here
      } else {
         if (d < kStep) return false;
         flown = along(t, last) + d;
      }
   }
   const int n = t.first + t.count;
   t.at[index_of(n)] = p;
   t.along[index_of(n)] = flown;
   if (t.count < kPoints) ++t.count;
   else ++t.first;   // the oldest made way
   return true;
}

// The point nearest `p`: from point `from` on, kWindow past it at most, so a
// wingman's place never goes back; the whole trail where `from` is not on it. Of
// those within half a step of the nearest, the newest (flown in and out again:
// the way out). -1 with none; `dist`, how far it is.
inline int nearest(const Trail& t, V3 p, int from, float* dist = nullptr)
{
   if (t.count == 0 || !flyer_wingmen::finite(p)) return -1;
   const bool on = has(t, from);
   const int lo = on ? from : t.first;
   const int hi = on && from + kWindow < newest(t) ? from + kWindow : newest(t);
   float best = std::numeric_limits<float>::infinity();
   for (int n = lo; n <= hi; ++n) {
      const float d = flyer_wingmen::length(point(t, n) - p);
      if (d < best) best = d;
   }
   if (!std::isfinite(best)) return -1;
   for (int n = hi; n >= lo; --n) {
      const float d = flyer_wingmen::length(point(t, n) - p);
      if (d <= best + 0.5f * kStep) {
         if (dist) *dist = d;
         return n;
      }
   }
   return -1;
}

// A wingman takes the trail: its line blocked and the trail's nearest point
// within kNear (its line to that point, clear, is the caller's ray).
inline bool takes(bool lineBlocked, float nearestDist) { return lineBlocked && nearestDist <= kNear; }

// A look along its line on the trail: true once it has been clear kLeaveAfter
// looks in a row.
inline bool leaves(int& clearLooks, bool clear)
{
   clearLooks = clear ? clearLooks + 1 : 0;
   return clearLooks >= kLeaveAfter;
}

// How far the leader is along its trail: to the newest point and past it.
inline float leader_along(const Trail& t, V3 leader)
{
   if (t.count == 0) return 0.0f;
   const float past = flyer_wingmen::length(leader - point(t, newest(t)));
   return along(t, newest(t)) + (std::isfinite(past) ? past : 0.0f);
}

// How far a wingman at point `at` is behind its place in the line, kGap behind
// the leader for each in front of it (`rank` 0 first): positive behind, as a
// slot ahead of it is, so the formation's speed (formation_speed) closes it.
inline float behind_place(const Trail& t, float leaderAlong, int rank, float gap, int at)
{
   if (!has(t, at) || !std::isfinite(leaderAlong) || !std::isfinite(gap)) return 0.0f;
   return leaderAlong - gap * (float)(rank + 1) - along(t, at);
}

// The points past `at` a wingman may steer at, the furthest first, the newest
// the furthest any can be. How many went into `out` (kMaxCuts at most).
inline int cut_candidates(const Trail& t, int at, int* out)
{
   if (!has(t, at)) return 0;
   int n = 0;
   for (const int ahead : kCuts) {
      const int c = at + ahead < newest(t) ? at + ahead : newest(t);
      if (c <= at || (n > 0 && out[n - 1] == c)) continue;
      out[n++] = c;
   }
   return n;
}

// No further point for kStuckFor while its place is a gap or more ahead.
inline bool stuck(double sinceGained, float behind, float gap)
{
   return sinceGained > kStuckFor && behind > gap;
}

// Its slots' share of their spacing, `dt` seconds on: toward kSqueeze under a
// roof, back toward 1 out from under it, kSqueezeRate a second.
inline float squeeze(float now, bool roofed, float dt)
{
   if (!std::isfinite(now) || now < kSqueeze || now > 1.0f) now = 1.0f;
   if (!(dt > 0.0f) || !std::isfinite(dt)) return now;
   const float to = roofed ? kSqueeze : 1.0f;
   const float step = kSqueezeRate * dt;
   if (now < to) return now + step < to ? now + step : to;
   return now - step > to ? now - step : to;
}

// ---- Through a hangar's mouth ------------------------------------------------------
// The user, after the first entries along the trail: "they have a good chance to
// hit the rim trying to enter despite following my path". On the trail a
// wingman's lines are checked at its whole size (the caller's rays: the line and
// four beside it, its radius plus kFitMargin out), so it cuts no corner its wings
// cannot clear; the point it steers at is moved off what is beside it
// (centre_offset: to the middle of an opening narrower than twice kCentreReach,
// out to its size from one wall nearer than that); and within kDoorSlow along
// the trail of where the leader went under a roof (near_door), or under one
// itself, it flies at its slowest.
constexpr float kFitMargin   = 2.0f;    // m beyond its radius
constexpr float kCentreReach = 40.0f;   // m looked out each way from the point it steers at
constexpr float kDoorSlow    = 60.0f;   // m along the trail either side of the door

// How far to move a point toward `right` between something solid `left` and
// `right` metres off either side (infinity or NaN: nothing within
// kCentreReach), for a flyer needing `need` either side: to the middle with
// both within reach; away from one nearer than `need`; else not at all.
inline float centre_offset(float left, float right, float need)
{
   const bool l = left < kCentreReach, r = right < kCentreReach;
   if (l && r) return 0.5f * (right - left);
   if (l && left < need) return need - left;
   if (r && right < need) return right - need;
   return 0.0f;
}

// Whether a point `along` the trail is within kDoorSlow of the door, `door` the
// along of the point where the leader went under a roof (NaN: none).
inline bool near_door(float along, float door) { return std::fabs(along - door) <= kDoorSlow; }   // NaN: no

// ---- The way out ------------------------------------------------------------------
// One let go under a roof flies back out the way the leader flew in: along the
// trail backwards, older points first (nearest_back, back_candidates), until it
// has been out from under the roof kOutLooks looks in a row (out_clear), then it
// is let go; one not on it within kOutJoin, stuck (no older point for kStuckFor)
// or still going after kOutMax is let go where it is. The user: "can it fly back
// out the way it flew in, probably by [following] my path backwards? Lots of
// hangars are single entry/exit".
constexpr int    kOutLooks = 2;      // roof looks in a row with none over it: out
constexpr double kOutJoin  = 2.0;    // s to get onto the trail
constexpr double kOutMax   = 45.0;   // s on the way out at most

inline int prev_of(const Trail& t, int n) { return n > t.first ? n - 1 : t.first; }

// The point nearest `p` going back: from point `from` back, kWindow before it at
// most, so its place never goes forward again; the whole trail where `from` is
// not on it. Of those within half a step of the nearest, the oldest (flown in and
// out again: the way in). -1 with none; `dist`, how far it is.
inline int nearest_back(const Trail& t, V3 p, int from, float* dist = nullptr)
{
   if (t.count == 0 || !flyer_wingmen::finite(p)) return -1;
   const bool on = has(t, from);
   const int hi = on ? from : newest(t);
   const int lo = on && from - kWindow > t.first ? from - kWindow : t.first;
   float best = std::numeric_limits<float>::infinity();
   for (int n = lo; n <= hi; ++n) {
      const float d = flyer_wingmen::length(point(t, n) - p);
      if (d < best) best = d;
   }
   if (!std::isfinite(best)) return -1;
   for (int n = lo; n <= hi; ++n) {
      const float d = flyer_wingmen::length(point(t, n) - p);
      if (d <= best + 0.5f * kStep) {
         if (dist) *dist = d;
         return n;
      }
   }
   return -1;
}

// The points before `at` it may steer at going back, the furthest first, the
// oldest the furthest any can be. How many went into `out` (kMaxCuts at most).
inline int back_candidates(const Trail& t, int at, int* out)
{
   if (!has(t, at)) return 0;
   int n = 0;
   for (const int back : kCuts) {
      const int c = at - back > t.first ? at - back : t.first;
      if (c >= at || (n > 0 && out[n - 1] == c)) continue;
      out[n++] = c;
   }
   return n;
}

// A roof look on the way out: true once none has been over it kOutLooks looks
// in a row.
inline bool out_clear(int& clearLooks, bool roofed)
{
   clearLooks = roofed ? 0 : clearLooks + 1;
   return clearLooks >= kOutLooks;
}

// Whether the game's lift is off for a wingman (EntityFlyer::RaiseHeight, which
// sends it 25 m over the top of what is ahead, a hangar's whole ship): under a
// roof itself, or with its leader under one and nothing solid on its line, as
// looked `lookAge` seconds ago, so it flies in through a hangar's mouth. With a
// wall in its way it keeps the lift, the game's own answer to what is ahead.
inline bool lift_off(bool roofed, bool leaderRoofed, bool lineShut, double lookAge)
{
   return roofed || (leaderRoofed && !lineShut && lookAge >= 0.0 && lookAge <= kFreshLook);
}

} // namespace flyer_trail
