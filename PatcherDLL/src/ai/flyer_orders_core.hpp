#pragma once

#include "flyer_wingmen_core.hpp"   // V3 and its operations

#include <cmath>
#include <cstdint>

// =============================================================================
// Orders for a player's wingmen, on their own: which order follows which, how
// long a far-off recruit has to join, which foe each wingman takes, and the
// passes it makes at a target on the ground. No engine reads, so
// tests/flyer_wingmen_tests.cpp can exercise all of it. The reads and calls
// around it are in flyer_wingmen.hpp.
// =============================================================================

namespace flyer_orders {

using flyer_wingmen::clampf;
using flyer_wingmen::V3;

// ---- The orders ---------------------------------------------------------------------
//   FormUp   hold their slots (the wingmen as they were)
//   Engage   take on enemies near the leader, else hold their slots
//   Attack   all on the target the player had when ordering, then the order
//            the attack interrupted
//   Cover    take on whoever attacks the leader, else hold their slots
enum class Order : uint8_t { FormUp, Engage, Attack, Cover };

// The squad key: form up and engage in turn; from an attack or cover, form up.
inline Order after_squad_key(Order o) { return o == Order::FormUp ? Order::Engage : Order::FormUp; }

// What an attack goes back to: the order it interrupted (an attack ordered
// during an attack goes back to the first one's).
inline Order resume_after_attack(Order current, Order resume)
{
   return current == Order::Attack ? resume : current;
}

// A word as an ODF or a mission script writes it: case, quotes, spaces, '_' and
// '-' ignored, so "Form Up", "form_up" and "FORMUP" are all `formup` (`word`
// in lower case, nothing between its letters).
inline bool word_is(const char* value, const char* word)
{
   if (!value || !word) return false;
   const char* p = value;
   for (const char* w = word;; ++p, ++w) {
      while (*p == ' ' || *p == '\t' || *p == '"' || *p == '_' || *p == '-' || *p == '\r' || *p == '\n') ++p;
      if (!*w) return !*p;
      const char c = *p >= 'A' && *p <= 'Z' ? (char)(*p - 'A' + 'a') : *p;
      if (c != *w) return false;
   }
}

// An order by its name: formup, engage, cover (or coverme), and attack (or
// attackmytarget) where `attack` allows it.
inline bool order_from(const char* value, Order& out, bool attack = false)
{
   if (word_is(value, "formup")) out = Order::FormUp;
   else if (word_is(value, "engage")) out = Order::Engage;
   else if (word_is(value, "cover") || word_is(value, "coverme")) out = Order::Cover;
   else if (attack && (word_is(value, "attack") || word_is(value, "attackmytarget"))) out = Order::Attack;
   else return false;
   return true;
}

// The name a script reads back (GetWingmenOrder).
inline const char* order_word(Order o)
{
   switch (o) {
   case Order::FormUp: return "formup";
   case Order::Engage: return "engage";
   case Order::Attack: return "attack";
   case Order::Cover: return "cover";
   }
   return "formup";
}

// A leader class's WingmanStartOrder: the order its flight starts with when
// called up; attack needs a target, so it is not one.
inline bool set_start_order(flyer_wingmen::ClassKeys& k, const char* value)
{
   Order o;
   if (!order_from(value, o)) return false;
   k.hasStartOrder = true;
   k.startOrder = (uint8_t)o;
   return true;
}

inline Order start_order(const flyer_wingmen::ClassKeys* k)
{
   return k && k->hasStartOrder ? (Order)k->startOrder : Order::FormUp;
}

// ---- Joining from afar ----------------------------------------------------------------
// Recruits come from anywhere on the map and fly in at their stretched speeds
// (the leader's plus 30 m/s). One counts as joined once within kJoinedRange of
// its slot; it has kJoinBase seconds plus one for every kJoinClosing metres it
// started off, at most kJoinMax, before it is let go.
constexpr float  kJoinedRange = 150.0f;   // m
constexpr double kJoinBase    = 20.0;     // s
constexpr float  kJoinClosing = 20.0f;    // m per second of grace
constexpr double kJoinMax     = 180.0;    // s

inline double join_seconds(float distance)
{
   if (!std::isfinite(distance) || distance < 0.0f) return kJoinMax;
   const double t = kJoinBase + (double)distance / kJoinClosing;
   return t < kJoinMax ? t : kJoinMax;
}

// ---- Who takes on whom ------------------------------------------------------------------
// Engage: enemy flyers, then enemy vehicles on the ground, within kEngageRange
// of the leader, chased until kEngageKeep from it. Cover: whoever has the
// leader as its target within kCoverRange, chased until kCoverKeep. Attack: the
// player's target, given up beyond kAttackKeep. A chasing wingman further than
// kChaseLeash from its leader drops its chase for its slot.
constexpr float kEngageRange = 800.0f;    // m from the leader
constexpr float kEngageKeep  = 1000.0f;
constexpr float kCoverRange  = 1200.0f;
constexpr float kCoverKeep   = 1500.0f;
constexpr float kAttackKeep  = 3000.0f;
constexpr float kChaseLeash  = 2500.0f;

// A leader class's WingmanEngageRange stands for kEngageRange (kMinEngageRange
// to kMaxEngageRange), and a foe is chased as much further as kEngageKeep is
// past kEngageRange (a quarter).
constexpr float kMinEngageRange = 100.0f;    // m
constexpr float kMaxEngageRange = 5000.0f;   // m

inline bool set_engage_range(flyer_wingmen::ClassKeys& k, const char* value)
{
   double v;
   if (!flyer_wingmen::read_number(value, v)) return false;
   k.hasEngageRange = true;
   k.engageRange = clampf((float)v, kMinEngageRange, kMaxEngageRange);
   return true;
}

inline float engage_range(const flyer_wingmen::ClassKeys* k) { return k && k->hasEngageRange ? k->engageRange : kEngageRange; }
inline float engage_keep(float range) { return range * (kEngageKeep / kEngageRange); }

// Each wingman takes the foe that costs it least: its distance, plus
// kGroundCost for one its role comes to second (a fighter's: on the ground,
// the air coming first; a bomber's: in the air) and kSpreadCost for every
// wingman already on it (they spread out before they double up).
constexpr float kGroundCost = 600.0f;   // m
constexpr float kSpreadCost = 400.0f;   // m

// A wingman class's WingmanRole.
enum class Role : uint8_t { Fighter, Bomber };

inline bool set_role(flyer_wingmen::ClassKeys& k, const char* value)
{
   Role r;
   if (word_is(value, "fighter")) r = Role::Fighter;
   else if (word_is(value, "bomber")) r = Role::Bomber;
   else return false;
   k.hasRole = true;
   k.role = (uint8_t)r;
   return true;
}

inline Role role_of(const flyer_wingmen::ClassKeys* k) { return k && k->hasRole ? (Role)k->role : Role::Fighter; }

struct Foe {
   V3   pos;
   bool ground = false;
   int  taken = 0;   // wingmen on it so far
};

inline float foe_cost(V3 me, const Foe& f, Role role = Role::Fighter)
{
   const bool second = role == Role::Bomber ? !f.ground : f.ground;
   return flyer_wingmen::length(f.pos - me) + (second ? kGroundCost : 0.0f) + kSpreadCost * (float)f.taken;
}

// The cheapest foe for a wingman at `me`: its index, or -1 for none.
inline int pick_foe(V3 me, const Foe* foes, int n, Role role = Role::Fighter)
{
   int best = -1;
   float bestCost = 0.0f;
   for (int i = 0; foes && i < n; ++i) {
      const float c = foe_cost(me, foes[i], role);
      if (!std::isfinite(c)) continue;
      if (best < 0 || c < bestCost) {
         best = i;
         bestCost = c;
      }
   }
   return best;
}

// ---- Passes at a target on the ground, and the gap behind a flyer -------------------------
// A flyer cannot hang behind a tank as it does behind a fighter: it dives at it,
// pulls out short of it (or once past it), flies on and up, then turns back for
// the next pass. How far is each wingman's own, from its primary weapon's ranges
// (its ODF's MinRange, OptimalRange, MaxRange: the ranges its AI fights at) and
// its cruising speed, on one scale, so each pass fires from the weapon's
// furthest reach in through its optimal range:
//   - it turns back in from its MaxRange (and at least kTurnSeconds of flight
//     past the pull-out each way); it gives up pulling out after the time that
//     takes, plus kTurnBack to come round;
//   - it pulls out just before its MinRange (kMinRangeMargin of it), and never
//     nearer than kPullOutSeconds of its speed: the room it needs to pull out (a
//     flyer turning 90 degrees a second carries on 1.4 to 2.1 s of its speed
//     toward the target as it pulls out), which is what decides it for a
//     weapon with no MinRange;
//   - behind a flyer in the air it settles kGapShare of its MaxRange back,
//     kMinGap to kMaxGap.
// A weapon with no MaxRange counts twice its OptimalRange, else kDefaultRange.
// Stock fighters' lasers (0/800/1600 m) at 60 m/s come back in from 1600 m and
// pull out at 150 m, 120 m behind a flyer; a gunship's recoilless cannon
// (0/300/600) at 40 m/s from 600 m, out at 100 m; torpedoes (MinRange 200) at
// 220 m.
constexpr float  kMinRangeMargin = 1.1f;
constexpr float  kPullOutSeconds = 2.5f;    // s
constexpr float  kTurnSeconds    = 1.0f;    // s
constexpr double kTurnBack       = 3.0;     // s
constexpr float  kGapShare       = 0.075f;
constexpr float  kMinGap         = 50.0f;   // m
constexpr float  kMaxGap         = 200.0f;  // m
constexpr float  kDefaultRange   = 600.0f;  // m
constexpr float  kSlowestRun     = 20.0f;   // m/s: the speed the shape counts at the least
constexpr float  kRunOut         = 300.0f;  // m ahead it steers while pulling out ...
constexpr float  kRunClimb       = 60.0f;   // ... and this much higher

struct RunShape {
   float  breakAt = 150.0f;   // m: pulls out this close ...
   float  extendTo = 700.0f;  // ... turns back in this far out ...
   double extendFor = 8.0;    // ... or after this long
   float  gap = 120.0f;       // m behind a flyer in the air
};

inline RunShape run_shape(float minRange, float optimal, float maxRange, float speed)
{
   const float reach = std::isfinite(maxRange) && maxRange > 0.0f  ? maxRange
                       : std::isfinite(optimal) && optimal > 0.0f ? optimal * 2.0f
                                                                  : kDefaultRange;
   const float v = std::isfinite(speed) && speed > kSlowestRun ? speed : kSlowestRun;
   const float pull = v * kPullOutSeconds;
   const float least = std::isfinite(minRange) && minRange > 0.0f ? minRange * kMinRangeMargin : 0.0f;
   RunShape s;
   s.breakAt = least > pull ? least : pull;
   s.extendTo = s.breakAt + 2.0f * v * kTurnSeconds;
   if (s.extendTo < reach) s.extendTo = reach;
   s.extendFor = (double)((s.extendTo - s.breakAt) / v) + kTurnBack;
   s.gap = clampf(reach * kGapShare, kMinGap, kMaxGap);
   return s;
}

struct Run {
   bool   extending = false;
   double since = 0.0;
};

// `aheadCos`: the cosine between its nose and the line to the target. Past it
// (behind it, inside where it would turn back) it pulls out as well.
inline void update_run(Run& r, const RunShape& s, float distance, float aheadCos, double now)
{
   if (!std::isfinite(distance)) return;
   if (!r.extending) {
      const bool passed = std::isfinite(aheadCos) && aheadCos < 0.0f && distance < s.extendTo;
      if (distance < s.breakAt || passed) {
         r.extending = true;
         r.since = now;
      }
   } else if (distance > s.extendTo || now - r.since > s.extendFor) {
      r.extending = false;
      r.since = now;
   }
}

// Where it steers while pulling out: straight on along its heading, levelled,
// and up.
inline V3 extend_point(V3 me, V3 nose)
{
   V3 flat{nose.x, 0.0f, nose.z};
   if (!flyer_wingmen::normalise(flat)) flat = V3{0.0f, 0.0f, 1.0f};
   return me + flat * kRunOut + V3{0.0f, kRunClimb, 0.0f};
}

// ---- Landing with the leader ------------------------------------------------------------
// When the player lands, each wingman lands beside it: at its slot's place spread
// out on the ground (kLandSpread times the slot, never nearer than kLandMinGap to
// the leader), where the ground there is level with the leader's (within
// kLandLevel, and nothing over it from kLandProbe up). Where it is not, the
// wingman circles overhead (kLoiterHeight up, kLoiterRadius round) instead. It
// comes down toward a point kLandHover over its spot and lands (EntityFlyer::Land,
// as a node's LandOnArrival does) once within kLandStart of it across and
// kLandHigh up. When the player takes off, so do they.
constexpr float kLandSpread   = 1.5f;
constexpr float kLandMinGap   = 15.0f;   // m
constexpr float kLandLevel    = 4.0f;    // m
constexpr float kLandFlat     = 0.85f;   // the ground's up at least this (a cosine: ~32 degrees)
constexpr float kLandProbe    = 40.0f;   // m
constexpr float kLandHover    = 15.0f;   // m
constexpr float kLandStart    = 40.0f;   // m
constexpr float kLandHigh     = 40.0f;   // m
constexpr float kLandSlowFrom = 300.0f;  // m across: from here in, at its slowest
constexpr float kLoiterHeight = 150.0f;  // m
constexpr float kLoiterRadius = 200.0f;  // m
constexpr float kLoiterLead   = 0.6f;    // radians ahead on the circle

// A slot's place on the ground from the leader, height left out.
inline V3 land_offset(V3 slot)
{
   V3 g{slot.x * kLandSpread, 0.0f, slot.z * kLandSpread};
   const float d = flyer_wingmen::length(g);
   if (!std::isfinite(d) || !(d > 1e-3f)) return V3{kLandMinGap, 0.0f, 0.0f};
   return d < kLandMinGap ? g * (kLandMinGap / d) : g;
}

// Whether the ground probed at a spot (its height, and how far up its surface
// faces) suits: level with the leader's ground and flat.
inline bool level_spot(float hitY, float normalY, float groundY)
{
   return std::isfinite(hitY) && std::isfinite(groundY) && std::fabs(hitY - groundY) <= kLandLevel &&
          normalY >= kLandFlat;
}

inline float across(V3 a, V3 b)
{
   const float dx = a.x - b.x, dz = a.z - b.z;
   return std::sqrt(dx * dx + dz * dz);
}

// Whether it may come down now: near its spot across, and not far over it.
inline bool may_land(V3 me, V3 spot)
{
   const float up = me.y - spot.y;
   return across(me, spot) <= kLandStart && up <= kLandHigh && up >= -kLandHigh;   // false for NaN
}

// The point it steers at to circle `center`, at that height: on the circle,
// kLoiterLead ahead of where it is round it (anticlockwise from above).
inline V3 loiter_point(V3 center, V3 me)
{
   const float dx = me.x - center.x, dz = me.z - center.z;
   const float a = std::isfinite(dx) && std::isfinite(dz) && (dx != 0.0f || dz != 0.0f) ? std::atan2(dz, dx) : 0.0f;
   const float b = a + kLoiterLead;
   return {center.x + kLoiterRadius * std::cos(b), center.y, center.z + kLoiterRadius * std::sin(b)};
}

} // namespace flyer_orders
