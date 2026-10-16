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

// ---- The squad key's hold, and who joins ------------------------------------------------
// Held kBreakOffHold after it goes down, the squad key breaks the flight off,
// once a press, as the trooper squad key's hold lets its followers go
// (EntitySoldier::Update: the trigger's bit 0 held past 1 s). The press itself
// acts first, as the trooper's does.
constexpr double kBreakOffHold = 1.0;   // s

struct SquadHold {
   double since = -1.0;   // when the key went down, or -1 while it is up
   bool   fired = false;  // this press broke the flight off
};

// The key as seen now (`down`: held; `pressed`: it went down this update):
// whether this is when it breaks the flight off.
inline bool squad_hold(SquadHold& h, bool down, bool pressed, double now)
{
   if (!down) {
      h = SquadHold{};
      return false;
   }
   if (pressed || h.since < 0.0) {
      h.since = now;
      h.fired = false;
      return false;
   }
   if (h.fired || !(now - h.since >= kBreakOffHold)) return false;
   h.fired = true;
   return true;
}

// Whether losses are made up: once the player called the flight up, or from
// takeoff with WingmanAuto; never in a flight the mission locked, nor once the
// player broke the flight off (until the squad key calls one up again), nor in
// a flight the player picked by hand.
inline bool tops_up(bool locked, bool brokenOff, bool handPicked, bool called, bool autoOn)
{
   return !locked && !brokenOff && !handPicked && (called || autoOn);
}

// The squad key with a flyer under the reticule: none to pick (not a friendly AI
// flyer, or one of the flight already), so the key does what it does; one that
// joins; or one that cannot (the flight is full or locked, or that flyer may
// not fly with this leader), so the key does nothing.
enum class Pick : uint8_t { None, Join, Refuse };

inline Pick pick_verdict(bool friendlyAiFlyer, bool ours, bool locked, bool mayJoin, bool room)
{
   if (!friendlyAiFlyer || ours) return Pick::None;
   return !locked && mayJoin && room ? Pick::Join : Pick::Refuse;
}

// The order a flight the squad key calls up starts with: after the player broke
// one off, form up; else the flight's start order.
inline Order call_up_order(bool afterBreakOff, Order start) { return afterBreakOff ? Order::FormUp : start; }

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
// (behind it, inside where it would turn back) it pulls out as well, and so it
// does inside where it would turn back with its line to the target `blocked`.
inline void update_run(Run& r, const RunShape& s, float distance, float aheadCos, double now, bool blocked = false)
{
   if (!std::isfinite(distance)) return;
   if (!r.extending) {
      const bool passed = std::isfinite(aheadCos) && aheadCos < 0.0f && distance < s.extendTo;
      if (distance < s.breakAt || passed || (blocked && distance < s.extendTo)) {
         r.extending = true;
         r.since = now;
      }
   } else if (distance > s.extendTo || now - r.since > s.extendFor) {
      r.extending = false;
      r.since = now;
   }
}

// Where it steers while pulling out along `out`: straight on along its heading,
// flattened off `out`, and kRunClimb out (up, for a target under open sky).
inline V3 extend_point_along(V3 me, V3 nose, V3 out)
{
   V3 flat = nose - out * flyer_wingmen::dot(nose, out);
   if (!flyer_wingmen::normalise(flat)) {
      const V3 any = std::fabs(out.y) < 0.9f ? V3{0.0f, 1.0f, 0.0f} : V3{0.0f, 0.0f, 1.0f};
      flat = any - out * flyer_wingmen::dot(any, out);
      flyer_wingmen::normalise(flat);
   }
   return me + flat * kRunOut + out * kRunClimb;
}

inline V3 extend_point(V3 me, V3 nose) { return extend_point_along(me, nose, V3{0.0f, 1.0f, 0.0f}); }

// ---- Line of sight, and targets shut in from above -------------------------------------
// A wingman fires only with nothing solid between it and its target: the line to
// it is looked along to kSightShort short of it, so what the target is mounted
// on right at it does not count.
//
// A pass assumes open sky over its target. Where a ray down onto the target from
// kOpenReach overhead meets something first (a hardpoint under a capital ship's
// hull, a gun under an overhang), the passes come in along its most open side
// instead (pick_open): of kApproachDirs directions out from it (up first, the
// other five axes, the eight corners), those whose line in from kOpenReach out
// is clear, the nearest the wingman's side, up a little first; with none clear,
// the clearest. Off that side (further than kOnSideCos from it, seen from the
// target) the wingman flies out to kSetupRange along it first, by way of a point
// it reaches in a straight line where the straight line there is blocked
// (pick_via), else further out across that side, at its own height, until it
// sees the setup point (escape_point); and it pulls out along it, not up. The
// rays look in from kOpenReach out, so what they meet is the outside of whatever
// shuts the target in (begun nearer, they could begin inside a big ship).
constexpr float kSightShort   = 10.0f;    // m
constexpr int   kApproachDirs = 14;
constexpr float kOpenReach    = 500.0f;   // m
constexpr float kSetupRange   = 400.0f;   // m
constexpr float kOnSideCos    = 0.5f;     // within 60 degrees of the open side
constexpr float kUpBias       = 0.25f;
constexpr float kEscapeStep   = 400.0f;   // m

inline V3 approach_dir(int k)
{
   constexpr float c = 0.57735027f;   // 1 / sqrt(3)
   switch (k) {
   case 0: return V3{0.0f, 1.0f, 0.0f};
   case 1: return V3{0.0f, -1.0f, 0.0f};
   case 2: return V3{1.0f, 0.0f, 0.0f};
   case 3: return V3{-1.0f, 0.0f, 0.0f};
   case 4: return V3{0.0f, 0.0f, 1.0f};
   case 5: return V3{0.0f, 0.0f, -1.0f};
   default: {
      const int n = k - 6;
      return V3{n & 1 ? -c : c, n & 2 ? -c : c, n & 4 ? -c : c};
   }
   }
}

// `clear[k]`: how far out along approach_dir(k) its line in is clear (kOpenReach:
// all of it); `toMe`: from the target to the wingman.
inline int pick_open(const float* clear, V3 toMe)
{
   const bool aimed = flyer_wingmen::normalise(toMe);
   int best = -1, clearest = -1;
   float bestScore = 0.0f;
   for (int k = 0; k < kApproachDirs; ++k) {
      if (std::isfinite(clear[k]) && (clearest < 0 || clear[k] > clear[clearest])) clearest = k;
      if (!(clear[k] >= kOpenReach)) continue;
      const V3 d = approach_dir(k);
      const float score = (aimed ? flyer_wingmen::dot(d, toMe) : 0.0f) + kUpBias * d.y;
      if (best < 0 || score > bestScore) {
         best = k;
         bestScore = score;
      }
   }
   return best >= 0 ? best : clearest >= 0 ? clearest : 0;
}

// Whether the wingman is on its target's open side, to pass at it from there.
inline bool on_open_side(V3 me, V3 target, V3 open)
{
   V3 to = me - target;
   if (!flyer_wingmen::normalise(to)) return true;
   return flyer_wingmen::dot(to, open) >= kOnSideCos;
}

inline V3 setup_point(V3 target, V3 open) { return target + open * kSetupRange; }

// Of the points out along each approach direction, the nearest the wingman
// reaches in a straight line and that sees the setup point; -1 for none.
inline int pick_via(const bool* reachable, const bool* seesSetup, const float* dist)
{
   int best = -1;
   for (int k = 0; k < kApproachDirs; ++k)
      if (reachable[k] && seesSetup[k] && std::isfinite(dist[k]) && (best < 0 || dist[k] < dist[best])) best = k;
   return best;
}

// With no such point: kEscapeStep further out from the target across its open
// side (the line from the target to it, the open side taken out), at its own
// height along that side; right over the target, along its heading so.
inline V3 escape_point(V3 me, V3 target, V3 open, V3 nose)
{
   V3 out = me - target;
   out = out - open * flyer_wingmen::dot(out, open);
   if (!flyer_wingmen::normalise(out)) {
      out = nose - open * flyer_wingmen::dot(nose, open);
      if (!flyer_wingmen::normalise(out)) {
         const V3 any = std::fabs(open.y) < 0.9f ? V3{0.0f, 1.0f, 0.0f} : V3{0.0f, 0.0f, 1.0f};
         out = any - open * flyer_wingmen::dot(any, open);
         flyer_wingmen::normalise(out);
      }
   }
   return me + out * kEscapeStep;
}

// ---- Landing with the leader ------------------------------------------------------------
// When the player lands, each wingman lands beside it, on ground level with the
// leader's (within kLandLevel) and flat. The places it tries, in order
// (land_tries): its slot's place spread out on the ground (kLandSpread times the
// slot, never nearer than kLandMinGap to the leader), the same way nearer, the
// other side, then behind the leader; each kLandSpacing from those already taken.
// The ground is probed straight down from kLandProbe over the leader's, or from
// kLandRoom under a roof over the leader (a hangar's: probing from over it found
// the roof). Under a roof a place must also be in a clear line from the leader,
// kLandClear over its ground, and the wingman's own way to it clear.
// It comes down toward a point kLandHover over its spot (lower under a low
// roof, hover_height), no faster than approach_speed, and lands (EntityFlyer::
// Land, as a node's LandOnArrival does) once within kLandStart of it across and
// kLandHigh up. With nowhere beside
// the leader it looks on ahead, and with nowhere there either it circles until a
// place is free: overhead in the open (kLoiterHeight up, kLoiterRadius round), low
// under a roof (forward_tries and roof_loiter_radius, below). When the player
// takes off, so do they.
constexpr float kLandSpread   = 1.5f;
constexpr float kLandMinGap   = 15.0f;   // m
constexpr float kLandNearest  = 10.0f;   // m: the nearest a place nearer in may be
constexpr float kLandSpacing  = 12.0f;   // m between two wingmen's places
constexpr float kLandLevel    = 8.0f;    // m
constexpr float kLandFlat     = 0.8f;    // the ground's up at least this (a cosine: ~37 degrees)
constexpr float kLandProbe    = 40.0f;   // m
constexpr float kLandRoom     = 2.0f;    // m under a roof the ground is probed from
constexpr float kLandClear    = 3.0f;    // m over the leader's ground a place's line is checked at
constexpr float kLandHover    = 15.0f;   // m
constexpr float kLandHoverMin = 4.0f;    // m
constexpr float kLandStart    = 25.0f;   // m
constexpr float kLandHigh     = 40.0f;   // m
constexpr int   kLandTries    = 5;
constexpr float kNoRoof       = 1e9f;    // no roof over the leader
constexpr float kLandEaseFrom = 120.0f;  // m across: from here in it eases off (approach_speed)
constexpr float kLoiterHeight = 150.0f;  // m
constexpr float kLoiterRadius = 200.0f;  // m
constexpr float kLoiterLead   = 0.6f;    // radians ahead on the circle

// Coming down to its place `across` metres off: `cruise` until kLandEaseFrom,
// easing to `careful` (its class's own lowest speed) by kLandStart, its slowest
// within it. It flew its slowest from 300 m out, and crawled in under fire.
inline float approach_speed(float across, float slowest, float careful, float cruise)
{
   if (!(across > kLandStart)) return slowest;   // NaN too
   if (across >= kLandEaseFrom) return cruise;
   return careful + (cruise - careful) * ((across - kLandStart) / (kLandEaseFrom - kLandStart));
}

// A slot's place on the ground from the leader, height left out.
inline V3 land_offset(V3 slot)
{
   V3 g{slot.x * kLandSpread, 0.0f, slot.z * kLandSpread};
   const float d = flyer_wingmen::length(g);
   if (!std::isfinite(d) || !(d > 1e-3f)) return V3{kLandMinGap, 0.0f, 0.0f};
   return d < kLandMinGap ? g * (kLandMinGap / d) : g;
}

// The places a wingman in slot `slot` tries, in order, as offsets on the ground
// from the leader (x right, z ahead): its slot's place (land_offset); the same
// way at the slot's own distance, then at 0.6 of it, never nearer than
// kLandNearest; the other side (x mirrored); and behind the leader, further for
// each later `rank` (its turn among the flight). How many (kLandTries).
inline int land_tries(V3 slot, int rank, V3* out)
{
   int n = 0;
   const V3 spot = land_offset(slot);
   out[n++] = spot;
   const V3 flat{slot.x, 0.0f, slot.z};
   const float d = flyer_wingmen::length(flat);
   const V3 dir = std::isfinite(d) && d > 1e-3f ? flat * (1.0f / d) : V3{1.0f, 0.0f, 0.0f};
   const float atSlot = d > kLandNearest ? d : kLandNearest;
   const float nearer = 0.6f * d > kLandNearest ? 0.6f * d : kLandNearest;
   out[n++] = dir * atSlot;
   out[n++] = dir * nearer;
   out[n++] = V3{-spot.x, 0.0f, spot.z};
   out[n++] = V3{0.0f, 0.0f, -(kLandMinGap + kLandSpacing * (float)(rank > 0 ? rank : 0))};
   return n;
}

// Where the ground is probed down from, over the leader's ground: kLandProbe, or
// kLandRoom under a roof `roof` metres over it, never under kLandClear.
inline float probe_height(float roof)
{
   const float under = std::isfinite(roof) ? roof - kLandRoom : kLandProbe;
   const float h = under < kLandProbe ? under : kLandProbe;
   return h > kLandClear ? h : kLandClear;
}

// How far over its spot it comes down to before landing: kLandHover, lower under
// a low roof (twice kLandRoom under it), never under kLandHoverMin.
inline float hover_height(float roof)
{
   const float under = std::isfinite(roof) ? roof - 2.0f * kLandRoom : kLandHover;
   const float h = under < kLandHover ? under : kLandHover;
   return h > kLandHoverMin ? h : kLandHoverMin;
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

// ---- Landing right away -------------------------------------------------------------
// The user (2026-10-14): "When I land, AI should land right away. If their land is
// interrupted by landing on top of something, that's when they re-check and try
// to find a clear spot." A wingman within kLandNowRange of its leader as the
// leader comes down lands where it is (land_here): where a ray down meets ground
// within kLandReach, level with the leader's and flat (level_spot), and no flyer
// or vehicle is between it and that ground. Else it is given a place
// (land_tries). One coming down onto a flyer or vehicle (nearer under it than
// the ground), still not down kLandStuck after it was told to land, or landed on
// top of one, is interrupted (landing_interrupted): it takes off again and is
// given a clear place, kLandRetries times at most, after which it circles (or,
// under a roof, is let go).
constexpr float  kLandNowRange = 150.0f;   // m
constexpr float  kLandReach    = 200.0f;   // m: how far down the ground and what is on it are looked for
constexpr double kLandStuck    = 8.0;      // s
constexpr int    kLandRetries  = 2;

// `below`: how far under it the ground is (infinity for none within kLandReach);
// `onto`: how far under it a flyer or vehicle is (infinity for none).
inline bool land_here(bool nearLeader, float below, float hitY, float normalY, float groundY, float onto)
{
   return nearLeader && below <= kLandReach && !(onto < below) && level_spot(hitY, normalY, groundY);
}

enum class Interrupt : uint8_t { None, Onto, Stuck, OnTop };

// `landing`: in the game's landing state (coming down); `landed`: down;
// `since`: seconds since it was told to land.
inline Interrupt landing_interrupted(bool landing, bool landed, double since, float below, float onto)
{
   const bool over = onto < below;   // NaN or infinity: no
   if (landing && over) return Interrupt::Onto;
   if (landing && since > kLandStuck) return Interrupt::Stuck;
   if (landed && over) return Interrupt::OnTop;
   return Interrupt::None;
}

// The player out of its flyer, landed: its wingmen still on their way come down
// beside it all the same, and get out as they would with it aboard, until
// kArriveFor after it got out; then the rest are let go. The user, after the
// first hangar landings: "I just don't see any late arriving wingmen".
constexpr double kArriveFor = 60.0;   // s
inline bool arrival_over(double sinceOut) { return !(sinceOut <= kArriveFor); }   // NaN: over

// Whether a place is kLandSpacing from each of the `n` already taken.
inline bool spaced(V3 spot, const V3* taken, int n)
{
   for (int i = 0; taken && i < n; ++i)
      if (!(across(spot, taken[i]) >= kLandSpacing)) return false;
   return true;
}

// Whether it may come down now: near its spot across, and not far over it.
inline bool may_land(V3 me, V3 spot)
{
   const float up = me.y - spot.y;
   return across(me, spot) <= kLandStart && up <= kLandHigh && up >= -kLandHigh;   // false for NaN
}

// The point it steers at to circle `center`, at that height: on the circle,
// kLoiterLead ahead of where it is round it (anticlockwise from above).
inline V3 loiter_point(V3 center, V3 me, float radius = kLoiterRadius)
{
   const float dx = me.x - center.x, dz = me.z - center.z;
   const float a = std::isfinite(dx) && std::isfinite(dz) && (dx != 0.0f || dz != 0.0f) ? std::atan2(dz, dx) : 0.0f;
   const float b = a + kLoiterLead;
   return {center.x + radius * std::cos(b), center.y, center.z + radius * std::sin(b)};
}

// ---- On ahead, and circling under a roof ----------------------------------------------
// With no place beside the leader (land_tries), one looks on ahead, the way the
// leader faces: kLandAhead places kLandAheadSide to its own side of the leader's
// line, from kLandMinGap ahead, every kLandSpacing (forward_tries: to 195 m); the
// caller stops at the first wall between one and the next. The user (2026-10-15):
// "Instead of a circle, they should move forward instead. If forward is not clear
// (wall) then circle until a free spot appears". With none there either it
// circles, looking for a free place again every kLandLookAgain: overhead in the
// open; under a roof low, at its hover height, round where there is most room
// (roof_loiter_radius; with no room to circle, it is let go).
constexpr float  kLandAheadSide  = 12.0f;    // m to its side of the leader's line
constexpr int    kLandAhead      = 16;       // places
constexpr float  kRoofLoiterMin  = 15.0f;    // m round
constexpr float  kRoofLoiterMax  = 30.0f;    // m round
constexpr float  kRoofLoiterRoom = 5.0f;     // m kept from what is round the circle
constexpr double kLandLookAgain  = 2.0;      // s

// The places on ahead (x right, z ahead of the leader), on the side its slot is
// (a slot on the leader's line: right for an even `rank`, left for an odd one).
// How many (kLandAhead at most).
inline int forward_tries(V3 slot, int rank, V3* out)
{
   const float side = slot.x > 0.0f ? 1.0f : slot.x < 0.0f ? -1.0f : (rank % 2 == 0 ? 1.0f : -1.0f);
   int n = 0;
   for (int k = 0; k < kLandAhead; ++k) out[n++] = V3{side * kLandAheadSide, 0.0f, kLandMinGap + kLandSpacing * (float)k};
   return n;
}

// How far round it circles under a roof with `room` metres clear every way from
// the circle's centre: that less kRoofLoiterRoom, kRoofLoiterMax at most; -1
// with less than kRoofLoiterMin left, no room to circle.
inline float roof_loiter_radius(float room)
{
   const float r = room - kRoofLoiterRoom;
   if (!(r >= kRoofLoiterMin)) return -1.0f;   // NaN too
   return r < kRoofLoiterMax ? r : kRoofLoiterMax;
}

} // namespace flyer_orders
