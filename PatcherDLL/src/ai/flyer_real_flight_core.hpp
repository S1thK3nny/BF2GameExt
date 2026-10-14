#pragma once

#include "flyer_wingmen_core.hpp"   // V3 and its operations, the throttle's inverse, the ODF number reader

#include <cmath>

// =============================================================================
// Real flight on paths, on its own: how far ahead of an AI flyer the point it
// chases (its carrot) runs along its path, how much of each update the carrot
// is stepped, and the speed the flyer is asked for. No engine reads, so
// tests/flyer_real_flight_tests.cpp can exercise all of it. The reads and calls
// around it are in flyer_real_flight.hpp.
// =============================================================================

namespace flyer_real_flight {

using flyer_wingmen::clampf;
using flyer_wingmen::V3;

// ---- The carrot -----------------------------------------------------------------
// The game's path follower moves whatever matrix it is handed. Handed a carrot of
// our own in place of the flyer's, it steps that along the path (branching,
// weaving, turning on an attacker) while the flyer chases it by physics. The
// carrot runs a lead ahead: about a second of the flyer's speed, never less than
// a fighter's turning circle (~45 m at 70 m/s and 90 degrees a second), so the
// flyer cuts the path's tight corners rather than circling a point it cannot
// turn onto.
constexpr float kLeadSeconds = 1.0f;
constexpr float kMinLead     = 40.0f;    // m
constexpr float kMaxLead     = 150.0f;   // m

inline float lead_for(float speed)
{
   if (!std::isfinite(speed) || !(speed > 0.0f)) return kMinLead;
   return clampf(speed * kLeadSeconds, kMinLead, kMaxLead);
}

// How much of an update the carrot is stepped: all of it with the flyer the
// lead behind, up to kMaxPace with the flyer on top of it, none (it waits) with
// the flyer twice the lead behind.
constexpr float kMaxPace = 2.0f;

inline float pace(float distance, float lead)
{
   if (!std::isfinite(distance) || !(lead > 0.0f)) return 1.0f;
   return clampf(2.0f - distance / lead, 0.0f, kMaxPace);
}

// Further from its carrot than this the flyer has lost it (thrown off by a
// collision, a trick, a fight): the carrot starts over from the flyer.
constexpr float kLostLeads = 4.0f;
constexpr float kMinLost   = 300.0f;   // m

inline bool lost(float distance, float lead)
{
   const float limit = lead * kLostLeads > kMinLost ? lead * kLostLeads : kMinLost;
   return !(distance <= limit);   // NaN too
}

// The carrot flies the path's speed: how far it went over the part of the
// update it was stepped, smoothed over kSpeedSmoothing so a branch or a weave
// does not jerk the flyer's throttle.
constexpr float kSpeedSmoothing = 0.25f;   // s

inline float carrot_speed(float previous, float moved, float stepped, float dt)
{
   if (!(stepped > 1e-4f) || !std::isfinite(stepped) || !std::isfinite(moved)) return previous;
   const float now = moved / stepped;
   if (!(previous > 0.0f) || !std::isfinite(previous)) return now;
   const float share = std::isfinite(dt) ? clampf(dt / kSpeedSmoothing, 0.0f, 1.0f) : 1.0f;
   return previous + (now - previous) * share;
}

// The flyer is asked for the path's speed, plus kCatchUp for every metre its
// carrot is past the lead (less for every metre short of it), within what it can
// fly.
constexpr float kCatchUp = 0.5f;   // m/s per metre

inline float flyer_speed(float carrotSpeed, float distance, float lead, float minSpeed, float maxSpeed)
{
   if (!(maxSpeed > 0.0f) || !std::isfinite(maxSpeed)) return 0.0f;
   const float lo = minSpeed > 0.0f && minSpeed < maxSpeed ? minSpeed : 0.0f;
   const float base = std::isfinite(carrotSpeed) && carrotSpeed > 0.0f ? carrotSpeed : maxSpeed;
   const float gap = std::isfinite(distance) && std::isfinite(lead) ? distance - lead : 0.0f;
   return clampf(base + kCatchUp * gap, lo, maxSpeed);
}

// A flyer whose path follower finds no path flies as the AI will, then tries
// again; the game's own wait is the same.
constexpr double kRetrySeconds = 2.0;

// ---- Pursuit ----------------------------------------------------------------------
// The game's turn-and-attack (EntityFlyer::TargetReact) bends a placed flyer's
// path round toward where its attacker was, stares it down for 3.5 s, then
// branches back. A flyer on real flight chases the attacker itself instead:
// steered at where it will be, at its speed once close behind it, until the
// chase runs out, then back onto its paths.
constexpr float  kPursuitLeadMax  = 1.5f;     // s of the target's flight the point leads it by, at most
constexpr float  kPursuitMinSpeed = 20.0f;    // m/s: the lead never assumes it closes slower
constexpr float  kPursuitGap      = 120.0f;   // m behind the target it settles at
constexpr float  kPursuitGain     = 0.5f;     // m/s per metre off that gap
constexpr float  kPursuitAheadCos = 0.7f;     // the target within ~45 degrees of its nose: paced; else flat out
constexpr float  kPursuitKeepCos  = 0.7f;     // its speed held until the point is ~45 degrees off its nose
constexpr float  kPursuitLoseRange = 1000.0f; // m: further off than this, the chase is over
constexpr double kPursuitSeconds    = 15.0;   // a chase lasts this long from the last time it was provoked ...
constexpr double kPursuitMaxSeconds = 40.0;   // ... and never longer than this
constexpr double kPursuitCooldown   = 4.0;    // s back on its paths before it turns on anyone again

// Where it steers: the target led by its velocity for as long as the pursuer
// takes to cover the distance, at most kPursuitLeadMax.
inline V3 pursuit_point(V3 me, float mySpeed, V3 target, V3 targetVel)
{
   if (!flyer_wingmen::finite(targetVel)) return target;
   const float closing = std::isfinite(mySpeed) && mySpeed > kPursuitMinSpeed ? mySpeed : kPursuitMinSpeed;
   const float t = clampf(flyer_wingmen::length(target - me) / closing, 0.0f, kPursuitLeadMax);
   return target + targetVel * (std::isfinite(t) ? t : 0.0f);
}

// The speed it asks for: flat out to close or to turn onto the target; with the
// target within kPursuitAheadCos of its nose, the target's speed plus
// kPursuitGain for every metre past the gap (kPursuitGap, or a wingman's own
// from its weapon; less inside it), within what it can fly.
inline float pursuit_speed(float distance, float aheadCos, float targetSpeed, float minSpeed, float maxSpeed,
                           float gap = kPursuitGap)
{
   if (!(maxSpeed > 0.0f) || !std::isfinite(maxSpeed)) return 0.0f;
   if (!std::isfinite(distance) || !std::isfinite(aheadCos) || !std::isfinite(targetSpeed)) return maxSpeed;
   if (aheadCos < kPursuitAheadCos) return maxSpeed;
   const float lo = minSpeed > 0.0f && minSpeed < maxSpeed ? minSpeed : 0.0f;
   const float g = std::isfinite(gap) && gap > 0.0f ? gap : kPursuitGap;
   return clampf(targetSpeed + kPursuitGain * (distance - g), lo, maxSpeed);
}

// A chase's clock: it starts when the attacker provokes it, runs kPursuitSeconds
// from each provocation, and never past kPursuitMaxSeconds from the start.
struct Pursuit {
   bool   on = false;
   double started = 0.0;
   double until = 0.0;
};

inline void provoke(Pursuit& p, double now)
{
   if (!p.on) {
      p.on = true;
      p.started = now;
   }
   const double cap = p.started + kPursuitMaxSeconds;
   p.until = now + kPursuitSeconds < cap ? now + kPursuitSeconds : cap;
}

inline bool pursuit_over(const Pursuit& p, double now) { return !p.on || !(now < p.until); }

// PathFollowerRealFlight's value: 1 on, 0 off. False, leaving `on` alone, for
// anything else.
inline bool read_switch(const char* value, bool& on)
{
   double v;
   if (!flyer_wingmen::read_number(value, v)) return false;
   on = v != 0.0;
   return true;
}

// ---- How the flyers keep up, for the log ---------------------------------------------
struct Stats {
   int    samples = 0;
   double distance = 0.0, lead = 0.0, speed = 0.0, carrot = 0.0;
   float  worst = 0.0f;
   int    restarts = 0;   // carrots started over from a flyer that lost them
   int    pathless = 0;   // times a follower found no path

   void add(float d, float l, float flyerSpeed, float carrotSpeed)
   {
      if (!std::isfinite(d) || !std::isfinite(l) || !std::isfinite(flyerSpeed) || !std::isfinite(carrotSpeed)) return;
      ++samples;
      distance += d;
      lead += l;
      speed += flyerSpeed;
      carrot += carrotSpeed;
      if (d > worst) worst = d;
   }
};

} // namespace flyer_real_flight
