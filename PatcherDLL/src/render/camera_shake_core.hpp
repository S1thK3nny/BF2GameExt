#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// =============================================================================
// Camera shake maths, kept free of engine access so tests/camera_shake_tests.cpp
// can check it standalone.
//
// A shake is an offset of the camera in its own frame: a turn (pitch, yaw,
// roll) and a move (right, up, back). Two kinds build it:
//
//  - A one-off shake, from an event (a shot, a hit, a landing, a roll). It
//    rises to its peak over the first part of its length and eases back to
//    nothing, a smoothstep each way. At a rate of 0 it is a single push in one
//    direction; above 0 it swings back and forth at that rate inside the same
//    envelope.
//  - A held shake, while a state lasts (sprinting, boosting, turning, braking,
//    the stock blast queue). It swings at its rate, scaled by how strongly the
//    state holds at the moment.
//
// Everything is sampled by time, so the result is the same at any frame rate.
// The shapes follow the BFIII-derived camera spec: fire, hit and landing are
// smoothstep kicks, sprinting a railed judder, a blast three slow sines on roll
// and position. A flyer's boost, turns and bumps share one turbulence, after
// the user's reference model: a sine per axis near 11 Hz.
// =============================================================================

namespace camera_shake {

constexpr float kPi       = 3.14159265358979f;
constexpr float kDegToRad = kPi / 180.0f;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clamp01(float v) { return clampf(v, 0.0f, 1.0f); }
inline float smoothstep(float x) { x = clamp01(x); return x * x * (3.0f - 2.0f * x); }

// Band-limited noise in -1..1: three sines at unrelated frequencies. Continuous
// in t, so consecutive frames differ by a little, never by a jump. Time is in
// seconds, and double: the sine arguments reach millions of radians in a long
// session, where a float would step by several hundredths of a radian.
inline float noise(double t, float hz, float phase)
{
   const double w = 2.0 * 3.14159265358979323846 * hz;
   return static_cast<float>((std::sin(w * t + phase)
                            + 0.5  * std::sin(w * 1.93 * t + phase * 2.1 + 1.3)
                            + 0.25 * std::sin(w * 3.07 * t + phase * 3.7 + 2.9)) / 1.75);
}

// Per-axis phases, so no two axes move in step.
constexpr float kPhasePitch = 0.0f;
constexpr float kPhaseYaw   = 2.4f;
constexpr float kPhaseRoll  = 4.1f;
constexpr float kPhaseRight = 1.3f;
constexpr float kPhaseUp    = 3.3f;
constexpr float kPhaseBack  = 5.2f;

// A flyer's turbulence, after the reference model: one sine per axis, pitch at
// the rate, yaw at 1.3 times it and roll at 0.7 times (11, 14.3 and 7.7 Hz at
// 11), each from its own phase. Push, which the model leaves still, gets a
// sine of its own. `phase` moves all four together.
struct TurbulenceSines {
   float pitch = 0.0f;
   float yaw   = 0.0f;
   float roll  = 0.0f;
   float back  = 0.0f;
};

inline TurbulenceSines turbulence_sines(double t, float hz, float phase = 0.0f)
{
   const double w = 2.0 * 3.14159265358979323846 * hz;
   TurbulenceSines s;
   s.pitch = static_cast<float>(std::sin(w * t + phase));
   s.yaw   = static_cast<float>(std::sin(1.3 * w * t + 2.1 + phase));
   s.roll  = static_cast<float>(std::sin(0.7 * w * t + 4.2 + phase));
   s.back  = static_cast<float>(std::sin(1.1 * w * t + 0.9 + phase));
   return s;
}

// -----------------------------------------------------------------------------
// The offset
// -----------------------------------------------------------------------------

// Angles in radians, movement in metres, all in the camera's own frame. The
// signs are the ones an ODF uses: pitch up, yaw right and roll clockwise (the
// view tilting to the right) are positive; `apply` turns them into the
// engine's.
struct Offset {
   float pitch = 0.0f;
   float yaw   = 0.0f;
   float roll  = 0.0f;
   float right = 0.0f;
   float up    = 0.0f;
   float back  = 0.0f;

   Offset& operator+=(const Offset& o)
   {
      pitch += o.pitch; yaw += o.yaw; roll += o.roll;
      right += o.right; up += o.up; back += o.back;
      return *this;
   }

   Offset scaled(float s) const
   {
      Offset o = *this;
      o.pitch *= s; o.yaw *= s; o.roll *= s;
      o.right *= s; o.up *= s; o.back *= s;
      return o;
   }

   bool negligible() const
   {
      return std::fabs(pitch) + std::fabs(yaw) + std::fabs(roll) < 1e-6f &&
             std::fabs(right) + std::fabs(up) + std::fabs(back) < 1e-5f;
   }
};

// A shake goes as far as its ODF says: there is no cap, so an ODF's values are
// the whole story. Only an axis that is not a number at all (a value read from
// a bad pointer, say) is dropped, since the camera matrix would never recover.
inline Offset finite_offset(const Offset& o)
{
   const auto keep = [](float v) { return std::isfinite(v) ? v : 0.0f; };
   Offset c;
   c.pitch = keep(o.pitch);
   c.yaw   = keep(o.yaw);
   c.roll  = keep(o.roll);
   c.right = keep(o.right);
   c.up    = keep(o.up);
   c.back  = keep(o.back);
   return c;
}

// The turn alone, for first person: the camera sits at the eye there, and a
// move would carry the arms or the cockpit along with it.
inline Offset turn_only(Offset o)
{
   o.right = 0.0f;
   o.up    = 0.0f;
   o.back  = 0.0f;
   return o;
}

// -----------------------------------------------------------------------------
// ODF values
// -----------------------------------------------------------------------------

// "min max", or one number for both ends. A one-off shake picks a value in the
// range each time it plays; a held shake uses the larger end.
struct Range {
   float lo = 0.0f;
   float hi = 0.0f;

   float largest() const { return std::fmax(std::fabs(lo), std::fabs(hi)); }
};

// Reads "a" or "a b" (either order). Returns false and leaves `out` alone when
// the text does not start with a finite number.
inline bool parse_range(const char* text, Range& out)
{
   if (!text) return false;
   char* end = nullptr;
   const float a = std::strtof(text, &end);
   if (end == text || !std::isfinite(a)) return false;
   const char* rest = end;
   const float b = std::strtof(rest, &end);
   const bool two = end != rest && std::isfinite(b);
   out.lo = two ? std::fmin(a, b) : a;
   out.hi = two ? std::fmax(a, b) : a;
   return true;
}

// Reads a number that must not be negative. Returns false on anything else.
inline bool parse_amount(const char* text, float& out)
{
   if (!text) return false;
   char* end = nullptr;
   const float v = std::strtof(text, &end);
   if (end == text || !std::isfinite(v) || v < 0.0f) return false;
   out = v;
   return true;
}

// One end of a threshold: a number, or one of the flyer class's own speeds
// (its ODF's MinSpeed, MidSpeed, MaxSpeed or BoostSpeed), which differ per
// class and are looked up in play.
enum class SpeedRef : uint8_t { None, Min, Mid, Max, Boost };

struct Bound {
   float    value = 0.0f;
   SpeedRef ref   = SpeedRef::None;
};

// When a shake plays: one value, or a pair kept in the order written. See
// threshold_level.
struct Threshold {
   Bound from;
   Bound to;
   bool  pair = false;
};

// A speed name at the start of `text`, any case. Returns what follows it, or
// null when there is none.
inline const char* parse_speed_name(const char* text, SpeedRef& out)
{
   static const struct { const char* name; SpeedRef ref; } kNames[] = {
      { "minspeed", SpeedRef::Min }, { "midspeed", SpeedRef::Mid },
      { "maxspeed", SpeedRef::Max }, { "boostspeed", SpeedRef::Boost },
   };
   for (const auto& n : kNames) {
      size_t i = 0;
      while (n.name[i] && std::tolower(static_cast<unsigned char>(text[i])) == n.name[i]) ++i;
      if (!n.name[i] && !std::isalnum(static_cast<unsigned char>(text[i]))) {
         out = n.ref;
         return text + i;
      }
   }
   return nullptr;
}

// One end: a number or a speed name, after any spaces. Returns what follows
// it, or null.
inline const char* parse_bound(const char* text, Bound& out)
{
   while (*text == ' ' || *text == '\t') ++text;
   SpeedRef ref = SpeedRef::None;
   if (const char* rest = parse_speed_name(text, ref)) {
      out = { 0.0f, ref };
      return rest;
   }
   char* end = nullptr;
   const float v = std::strtof(text, &end);
   if (end == text || !std::isfinite(v)) return nullptr;
   out = { v, SpeedRef::None };
   return end;
}

// Reads "a" or "a b", each a number or a speed name. Returns false and leaves
// `out` alone when the text does not start with either.
inline bool parse_threshold(const char* text, Threshold& out)
{
   if (!text) return false;
   Bound a, b;
   const char* rest = parse_bound(text, a);
   if (!rest) return false;
   const bool two = parse_bound(rest, b) != nullptr;
   out.from = a;
   out.to   = two ? b : a;
   out.pair = two;
   return true;
}

// A flyer class's four speeds, from its ODF. `known` is false where the class
// cannot be read, and then no speed name resolves.
struct FlyerSpeeds {
   float min   = 0.0f;
   float mid   = 0.0f;
   float max   = 0.0f;
   float boost = 0.0f;
   bool  known = false;
};

// An end's value for a class. False when it names a speed the class does not
// have: BoostSpeed on a class that cannot boost, or any name where the speeds
// are not known.
inline bool bound_value(const Bound& b, const FlyerSpeeds& s, float& out)
{
   switch (b.ref) {
   case SpeedRef::None:  out = b.value; return true;
   case SpeedRef::Min:   out = s.min;   return s.known;
   case SpeedRef::Mid:   out = s.mid;   return s.known;
   case SpeedRef::Max:   out = s.max;   return s.known;
   case SpeedRef::Boost: out = s.boost; return s.known && s.boost > 0.0f;
   }
   return false;
}

// How far into its threshold `v` is, 0..1. One value is a step: 1 at or past
// it. A pair grows from its first end to its second, so a pair written high to
// low grows as `v` falls. 0 when an end does not resolve.
inline float threshold_level(const Threshold& th, const FlyerSpeeds& s, float v)
{
   float a, b;
   if (!std::isfinite(v) || !bound_value(th.from, s, a) || !bound_value(th.to, s, b)) return 0.0f;
   if (!th.pair || a == b) return v >= a ? 1.0f : 0.0f;
   return clamp01((v - a) / (b - a));
}

// Whether `v` has reached a threshold's first end, counting the way the pair
// runs. False when an end does not resolve.
inline bool threshold_reached(const Threshold& th, const FlyerSpeeds& s, float v)
{
   float a, b;
   if (!std::isfinite(v) || !bound_value(th.from, s, a) || !bound_value(th.to, s, b)) return false;
   return th.pair && b < a ? v <= a : v >= a;
}

// Whether a threshold grows as its value rises (a single value, or a pair
// written low to high).
inline bool threshold_rising(const Threshold& th, const FlyerSpeeds& s)
{
   float a, b;
   if (!bound_value(th.from, s, a) || !bound_value(th.to, s, b)) return true;
   return !(th.pair && b < a);
}

// One shake's settings: angles in degrees and movement in metres, as an ODF
// gives them. Length and rise only mean something to a one-off shake.
struct Shape {
   Range pitch, yaw, roll, push;
   // A one-off shake: how long it lasts, and the share of that spent rising to
   // its peak. A shake that lasts: how long it takes to fade in and out in all,
   // and the share of that spent fading in (held_fade).
   float length = 0.0f;   // seconds
   float rise   = 0.0f;   // share of the length, 0..1
   float rate   = 0.0f;   // swings per second; 0 makes a one-off shake a single push
   // One-off: how many can run at once, added together (see KickChannel).
   // Blast: the amount of stock Shake it levels off at (see blast_amount).
   float limit  = 0.0f;
   // When a flyer's boost, turn, brake or collision shake plays, in that
   // shake's own measure (see the Triggers section).
   Threshold threshold;
   // BoostShake and BrakeShake: the share left once the flyer reaches the
   // speed it is heading for, 0..1.
   float steady = 0.0f;
   // The push goes out and back once (a one-off shake) or holds steady (a
   // shake that lasts) while the angles swing at the rate. Off, it swings with
   // them, which moves the camera in and out.
   bool  pushOnce = true;
   // StrikeShake: whether a swing that lands only on teammates counts.
   bool  teammates = true;
};

constexpr Bound speed_bound(SpeedRef r) { return { 0.0f, r }; }
constexpr Bound value_bound(float v) { return { v, SpeedRef::None }; }
constexpr Threshold between(Bound a, Bound b) { return { a, b, true }; }

// The defaults, used for whatever a class leaves unset. Fire, hit, landing,
// sprint and blast are the spec's tune; the rest are first guesses.
//
// The pushes are the distances they used to show. Until 2026-10-01 a push
// piled up in BF2's camera (LeftShake); at 60 frames a second, with the
// soldier camera they were tuned under (MoveTensionZ "30 8"), fire's 0.015
// showed as about 0.06, the roll's 0.1 as 0.7 and the blast's 0.03 as 0.08.
namespace defaults {
// pitch, yaw, roll, push, length, rise, rate, limit; then threshold and steady
// Shots add together, up to eight running at once, as in the reference model.
constexpr Shape kFire         = { { 0.25f, 0.4f }, { -0.12f, 0.12f }, { 0.0f, 0.0f }, { 0.06f, 0.06f }, 0.22f, 0.25f, 0.0f, 8.0f };
constexpr Shape kHit          = { { 2.0f, 4.0f }, { -2.0f, 2.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, 0.25f, 0.3f, 0.0f, 1.0f };
constexpr Shape kLandSoldier  = { { -2.25f, -2.25f }, { 0.92f, 0.92f }, { 0.44f, 0.44f }, { 0.0f, 0.0f }, 0.67f, 0.15f, 0.0f, 1.0f };
// A combat roll eases the camera back and tips the view toward the feet, the
// direction of the spec's roll sweep, then settles. Tuned in play by the user.
constexpr Shape kRollSoldier  = { { -5.0f, -5.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.7f, 0.7f }, 1.0f, 0.5f, 0.0f, 1.0f };
// The sprint judder fades in and out in a third of a second each way, the
// spec's 3 per second.
constexpr Shape kSprintSoldier = { { 0.16f, 0.16f }, { 0.06f, 0.06f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, 2.0f / 3.0f, 0.5f, 4.0f, 0.0f };
// Flyers. Boost, turn and collision are the reference model's turbulence: half
// a degree a side at 11 Hz, a bump up to a degree and a half that fades fast.
// Boost and turn fade in and out in half a second each way (the model's 6 per
// second), the brake in a quarter and out in three quarters.
// Boost plays between the class's own MaxSpeed and BoostSpeed; a brake from
// the moment the brake is held, full at MaxSpeed and fading out toward
// MinSpeed; a turn after 2 to 3 seconds of hard turning; and a bump of any
// size, growing to full at 40 m/s. Take-off and landing stay small until they
// are tuned in play.
constexpr Shape kBoost        = { { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.0f, 0.0f }, 1.0f, 0.5f, 11.0f, 0.0f,
                                  between(speed_bound(SpeedRef::Max), speed_bound(SpeedRef::Boost)), 0.25f };
constexpr Shape kTurn         = { { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.0f, 0.0f }, 1.0f, 0.5f, 11.0f, 0.0f,
                                  between(value_bound(2.0f), value_bound(3.0f)), 0.0f };
constexpr Shape kBrake        = { { 0.4f, 0.4f }, { 0.4f, 0.4f }, { 0.2f, 0.2f }, { 0.0f, 0.0f }, 1.0f, 0.25f, 11.0f, 0.0f,
                                  between(speed_bound(SpeedRef::Min), speed_bound(SpeedRef::Max)), 0.0f };
constexpr Shape kCollision    = { { 1.5f, 1.5f }, { 1.5f, 1.5f }, { 1.5f, 1.5f }, { 0.0f, 0.0f }, 0.3f, 0.0f, 11.0f, 1.0f,
                                  between(value_bound(0.0f), value_bound(40.0f)), 0.0f };
constexpr Shape kTrickRoll    = { { 1.0f, 1.0f }, { 1.0f, 1.0f }, { 2.0f, 2.0f }, { 0.0f, 0.0f }, 0.6f, 0.1f, 5.0f, 1.0f };
constexpr Shape kTrickFlip    = { { 2.0f, 2.0f }, { 1.0f, 1.0f }, { 1.0f, 1.0f }, { 0.0f, 0.0f }, 0.8f, 0.1f, 5.0f, 1.0f };
constexpr Shape kTakeoff      = { { 0.3f, 0.3f }, { 0.0f, 0.0f }, { -0.2f, 0.2f }, { 0.0f, 0.0f }, 1.0f, 0.3f, 8.0f, 1.0f };
constexpr Shape kLanding      = { { -0.75f, -0.75f }, { 0.0f, 0.0f }, { -0.25f, 0.25f }, { 0.0f, 0.0f }, 0.5f, 0.12f, 0.0f, 1.0f };
// Melee, first guesses until they are tuned in play: a light swing; a strike
// that jars the view down; a block or a blocked swing that knocks it up, the
// blocked swing hardest; a deflected bolt lighter than a blocked blade.
constexpr Shape kSwing        = { { 0.15f, 0.25f }, { -0.2f, 0.2f }, { -0.3f, 0.3f }, { 0.0f, 0.0f }, 0.3f, 0.3f, 0.0f, 1.0f };
constexpr Shape kStrike       = { { -0.6f, -0.4f }, { -0.4f, 0.4f }, { -0.5f, 0.5f }, { 0.04f, 0.04f }, 0.25f, 0.12f, 0.0f, 1.0f };
constexpr Shape kBlock        = { { 0.4f, 0.6f }, { -0.6f, 0.6f }, { -0.6f, 0.6f }, { 0.05f, 0.05f }, 0.3f, 0.1f, 0.0f, 1.0f };
constexpr Shape kDeflect      = { { 0.2f, 0.3f }, { -0.3f, 0.3f }, { -0.3f, 0.3f }, { 0.02f, 0.02f }, 0.2f, 0.15f, 0.0f, 1.0f };
constexpr Shape kSwingBlocked = { { 0.6f, 0.8f }, { -0.6f, 0.6f }, { -0.4f, 0.4f }, { 0.06f, 0.06f }, 0.35f, 0.1f, 0.0f, 1.0f };
// Walkers, first guesses until they are tuned in play: a step is a short dip
// that rolls toward the foot that landed (step_side picks the roll's sign); a
// jump a small lift; a landing a dip sized by how fast it came down, from 30%
// at 2 m/s to full at 10 m/s. Turning on the spot sways the view gently, and
// boosting sways it between MaxSpeed and BoostSpeed for as long as it lasts.
constexpr Shape kStep         = { { -0.4f, -0.25f }, { -0.1f, 0.1f }, { 0.2f, 0.35f }, { 0.0f, 0.0f }, 0.35f, 0.15f, 0.0f, 1.0f };
constexpr Shape kJump         = { { 0.6f, 0.9f }, { -0.2f, 0.2f }, { -0.3f, 0.3f }, { 0.0f, 0.0f }, 0.4f, 0.25f, 0.0f, 1.0f };
constexpr Shape kLandWalker   = { { -1.5f, -1.0f }, { -0.3f, 0.3f }, { -0.6f, 0.6f }, { 0.0f, 0.0f }, 0.5f, 0.12f, 0.0f, 1.0f,
                                  between(value_bound(2.0f), value_bound(10.0f)), 0.0f };
constexpr Shape kTurnWalker   = { { 0.15f, 0.15f }, { 0.1f, 0.1f }, { 0.4f, 0.4f }, { 0.0f, 0.0f }, 1.0f, 0.5f, 1.5f, 0.0f };
constexpr Shape kBoostWalker  = { { 0.3f, 0.3f }, { 0.2f, 0.2f }, { 0.3f, 0.3f }, { 0.0f, 0.0f }, 1.0f, 0.3f, 2.0f, 0.0f,
                                  between(speed_bound(SpeedRef::Max), speed_bound(SpeedRef::Boost)), 1.0f };
// Per unit of the stock `Shake` amount.
// The blast's push sways the camera's position with it, part of how it reads.
constexpr Shape kBlast        = { { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 6.0f, 6.0f }, { 0.08f, 0.08f }, 0.0f, 0.0f, 3.0f, 2.5f,
                                  {}, 0.0f, false };
} // namespace defaults

// A small generator of our own: drawing from the game's PblRandom would change
// the simulation's sequence.
class Rng {
public:
   explicit Rng(uint32_t seed = 0x9E3779B9u) : m_s(seed ? seed : 1u) {}
   void seed(uint32_t s) { m_s = s ? s : 1u; }

   uint32_t next()
   {
      m_s ^= m_s << 13;
      m_s ^= m_s >> 17;
      m_s ^= m_s << 5;
      return m_s;
   }

   float unit() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }   // 0..1
   float in(const Range& r) { return r.lo + (r.hi - r.lo) * unit(); }

private:
   uint32_t m_s;
};

// -----------------------------------------------------------------------------
// One-off shakes
// -----------------------------------------------------------------------------

// 0 at the start, 1 at rise * length, back to 0 at length, a smoothstep each way.
// A rise of 0 starts at full strength.
inline float envelope(float age, float length, float rise)
{
   if (!(length > 0.0f) || age < 0.0f || age >= length) return 0.0f;
   const float up = clamp01(rise) * length;
   if (age < up) return smoothstep(age / up);
   const float down = length - up;
   return down > 0.0f ? 1.0f - smoothstep((age - up) / down) : 0.0f;
}

struct Kick {
   Offset from;    // where the channel stood when a restart replaced it; fades out over the rise
   Offset peak;
   float  length = 0.0f;
   float  rise   = 0.0f;
   float  rate   = 0.0f;
   float  phase  = 0.0f;   // this kick's own offset into the swing
   float  age    = 0.0f;
   bool   turbulent = false;   // swings as a flyer's turbulence rather than noise
   bool   pushOnce  = true;    // the push rises and fades without swinging

   bool alive() const { return age < length; }

   Offset value(double t) const
   {
      if (!alive()) return {};
      Offset v = peak;
      if (rate > 0.0f && turbulent) {
         const TurbulenceSines s = turbulence_sines(t, rate, phase);
         v.pitch *= s.pitch;
         v.yaw   *= s.yaw;
         v.roll  *= s.roll;
         if (!pushOnce) {
            v.right *= s.back;
            v.up    *= s.back;
            v.back  *= s.back;
         }
      } else if (rate > 0.0f) {
         v.pitch *= noise(t, rate, kPhasePitch + phase);
         v.yaw   *= noise(t, rate, kPhaseYaw + phase);
         v.roll  *= noise(t, rate, kPhaseRoll + phase);
         if (!pushOnce) {
            v.right *= noise(t, rate, kPhaseRight + phase);
            v.up    *= noise(t, rate, kPhaseUp + phase);
            v.back  *= noise(t, rate, kPhaseBack + phase);
         }
      }
      v = v.scaled(envelope(age, length, rise));
      const float up = clamp01(rise) * length;
      if (age < up) v += from.scaled(1.0f - smoothstep(age / up));
      return v;
   }
};

// A kick drawn from a shape: each angle and the push picked in its range, then
// scaled. The push goes back along the view.
inline Kick make_kick(const Shape& s, float scale, Rng& rng, bool turbulent = false)
{
   Kick k;
   k.peak.pitch = rng.in(s.pitch) * kDegToRad * scale;
   k.peak.yaw   = rng.in(s.yaw) * kDegToRad * scale;
   k.peak.roll  = rng.in(s.roll) * kDegToRad * scale;
   k.peak.back  = rng.in(s.push) * scale;
   k.length = s.length;
   k.rise   = clamp01(s.rise);
   k.rate   = s.rate;
   k.phase  = rng.unit() * 6.2831853f;
   k.turbulent = turbulent;
   k.pushOnce  = s.pushOnce;
   return k;
}

// How many of one shake can run at once, from its Limit: a whole number from 1
// to kMaxKicks, the reference model's eight. Fractions round down.
constexpr int kMaxKicks = 8;

inline int kick_cap(float limit)
{
   if (!(limit >= 2.0f)) return 1;   // 1, less, or not a number
   return limit >= static_cast<float>(kMaxKicks) ? kMaxKicks : static_cast<int>(limit);
}

// The one-off shakes of one kind, as the reference model runs fire kicks: each
// repeat is a kick of its own and those still running add together, up to the
// shake's Limit. When that many are running, the oldest makes room by fading
// out over the new kick's rise rather than vanishing, so the view never jumps
// (dropping it outright showed as a hitch). At a Limit of 1 that makes a
// repeat take over from wherever the view stands.
class KickChannel {
public:
   void trigger(Kick k, double t, float limit = 1.0f)
   {
      if (!(k.length > 0.0f)) return;
      const int cap = kick_cap(limit);
      Offset dropped;
      while (m_count >= cap) {
         dropped += m_kicks[0].value(t);
         remove(0);
      }
      k.from = dropped;
      k.age  = 0.0f;
      m_kicks[m_count++] = k;
   }

   void advance(float dt)
   {
      for (int i = 0; i < m_count;) {
         m_kicks[i].age += dt;
         if (m_kicks[i].alive()) ++i;
         else remove(i);
      }
   }

   Offset value(double t) const
   {
      Offset sum;
      for (int i = 0; i < m_count; ++i) sum += m_kicks[i].value(t);
      return sum;
   }

   bool active() const { return m_count > 0; }
   int  count() const { return m_count; }
   void clear() { m_count = 0; }

private:
   void remove(int i)
   {
      for (int j = i + 1; j < m_count; ++j) m_kicks[j - 1] = m_kicks[j];
      --m_count;
   }

   Kick m_kicks[kMaxKicks];
   int  m_count = 0;
};

// -----------------------------------------------------------------------------
// Held shakes
// -----------------------------------------------------------------------------

// Moves `level` toward `target` by at most `rate * dt`.
inline float approach(float level, float target, float rate, float dt)
{
   const float step = rate * dt;
   if (level < target) return std::fmin(target, level + step);
   return std::fmax(target, level - step);
}

// Eases toward its target, quickly on the way up and more slowly on the way
// down. Exponential, so the frame rate does not change the result.
struct Hold {
   float level = 0.0f;

   void advance(float target, float dt, float rise, float fall)
   {
      if (dt <= 0.0f) return;
      const float tau = target > level ? rise : fall;
      level += (target - level) * (tau > 0.0f ? 1.0f - std::exp(-dt / tau) : 1.0f);
   }
};

// A held shake's fade, from its Length and Rise as a one-off shake splits its
// length: Rise x Length to fade in, the rest to fade out, in seconds.
struct Fade {
   float in  = 0.0f;
   float out = 0.0f;
};

inline Fade held_fade(const Shape& s)
{
   const float length = std::isfinite(s.length) && s.length > 0.0f ? s.length : 0.0f;
   const float rise = clamp01(s.rise);
   return { length * rise, length * (1.0f - rise) };
}

// An exponential fade covers 95% of the way in three of its time constants,
// which is what "fades in over its time" means here.
constexpr float kFadeConstants = 3.0f;

inline void fade_toward(Hold& h, float target, float dt, const Fade& f)
{
   h.advance(target, dt, f.in / kFadeConstants, f.out / kFadeConstants);
}

// The same fade as a straight ramp, all the way in exactly its time: the
// sprint judder's, which was tuned that way.
inline float ramp_toward(float level, float target, float dt, const Fade& f)
{
   if (!(dt > 0.0f)) return level;
   const float time = target > level ? f.in : f.out;
   if (!(time > 0.0f)) return target;
   return approach(level, target, 1.0f / time, dt);
}

// A held shake's push with Shape::pushOnce: a steady move back along the view,
// in and out only as the shake itself comes and goes.
inline void hold_push(Offset& o, float m)
{
   o.right = 0.0f;
   o.up    = 0.0f;
   o.back  = m;
}

// A smooth swing at the shape's rate, each axis on its own noise.
inline Offset sway(const Shape& s, double t, float level)
{
   Offset o;
   if (!(level > 0.0f) || !(s.rate > 0.0f)) return o;
   const float a = kDegToRad * level;
   o.pitch = s.pitch.largest() * a * noise(t, s.rate, kPhasePitch);
   o.yaw   = s.yaw.largest() * a * noise(t, s.rate, kPhaseYaw);
   o.roll  = s.roll.largest() * a * noise(t, s.rate, kPhaseRoll);
   const float m = s.push.largest() * level;
   if (s.pushOnce) {
      hold_push(o, m);
      return o;
   }
   o.right = m * noise(t, s.rate, kPhaseRight);
   o.up    = m * noise(t, s.rate, kPhaseUp);
   o.back  = m * noise(t, s.rate, kPhaseBack);
   return o;
}

// A flyer's boost or turn: the turbulence held at `level`, each axis on its own
// sine (turbulence_sines).
inline Offset turbulence(const Shape& s, double t, float level)
{
   Offset o;
   if (!(level > 0.0f) || !(s.rate > 0.0f)) return o;
   const TurbulenceSines w = turbulence_sines(t, s.rate);
   const float a = kDegToRad * level;
   o.pitch = s.pitch.largest() * a * w.pitch;
   o.yaw   = s.yaw.largest() * a * w.yaw;
   o.roll  = s.roll.largest() * a * w.roll;
   o.back  = s.push.largest() * level * (s.pushOnce ? 1.0f : w.back);
   return o;
}

// How hard a signal is driven into its limits: well past them, so the result
// flips between them rather than following the curve.
constexpr float kRail = 6.0f;
inline float rail(float v) { return clampf(v * kRail, -1.0f, 1.0f); }

// Sprinting, after the spec's movement shake: a railed judder. Pitch flips
// between its limits at about `rate`; yaw follows the stride, two footfalls to
// a stride of 2.8 / rate seconds (0.7 s at the default 4), with a little noise.
inline Offset judder(const Shape& s, double t, float level)
{
   Offset o;
   if (!(level > 0.0f) || !(s.rate > 0.0f)) return o;
   const double strideHz = s.rate / 2.8;
   const float step = static_cast<float>(std::sin(4.0 * 3.14159265358979323846 * strideHz * t));
   const float a = kDegToRad * level;
   o.pitch = s.pitch.largest() * a * rail(noise(t, s.rate, kPhasePitch));
   o.yaw   = s.yaw.largest() * a * rail(step * std::fabs(step) + 0.5f * noise(t, s.rate, kPhaseYaw + kPi));
   o.roll  = s.roll.largest() * a * rail(noise(t, s.rate, kPhaseRoll));
   const float m = s.push.largest() * level;
   if (s.pushOnce) {
      hold_push(o, m);
      return o;
   }
   o.right = m * rail(noise(t, s.rate, kPhaseRight));
   o.up    = m * rail(step);
   o.back  = m * rail(noise(t, s.rate, kPhaseBack));
   return o;
}

// The blast, from the stock queue (explosions, walker deaths, flyer crashes):
// three sines near the shape's rate at the spec's ratios (3.43 / 2.67 / 2.87 Hz
// at a rate of 3). The amplitude follows the strongest running shake, levelling
// off towards `cap` units (the shape's limit, 2.5 by default) so a detpack is
// not four times a grenade.
inline float blast_amount(float strongest, float cap)
{
   if (!(strongest > 0.0f) || !(cap > 0.0f)) return 0.0f;
   return cap * std::tanh(strongest / cap);
}

inline Offset blast(const Shape& s, double t, float amount)
{
   Offset o;
   if (!(amount > 0.0f) || !(s.rate > 0.0f)) return o;
   const double w = 2.0 * 3.14159265358979323846 * s.rate;
   const float sx = static_cast<float>(std::sin(w * 1.143 * t));
   const float sy = static_cast<float>(std::sin(w * 0.889 * t + 0.7));
   const float sz = static_cast<float>(std::sin(w * 0.956 * t + 1.9));
   const float a = kDegToRad * amount;
   o.pitch = s.pitch.largest() * a * sy;
   o.yaw   = s.yaw.largest() * a * sz;
   o.roll  = s.roll.largest() * a * (0.6f * sx + 0.4f * sz);
   const float m = s.push.largest() * amount;
   if (s.pushOnce) {
      hold_push(o, m);
      return o;
   }
   o.right = m * sy;
   o.up    = m * sx;
   o.back  = m * sz;
   return o;
}

// -----------------------------------------------------------------------------
// Triggers
// -----------------------------------------------------------------------------

// What each flyer threshold measures:
//   BoostShake      the flyer's speed, m/s, or its class's speeds by name
//   BrakeShake      the same, while it brakes
//   TurnShake       seconds of hard turning (TurnCount)
//   CollisionShake  how much a bump changes its velocity, m/s

// BoostShake and BrakeShake: where the speed sits in the threshold, at full
// strength while the flyer is still speeding up (boost) or slowing down
// (brake), and at `steady` of that once it gets where it is going.
inline float boost_level(float position, bool heading, float steady)
{
   return clamp01(position) * (heading ? 1.0f : clamp01(steady));
}

// Inside a landing region BF2 caps a flyer's speeds (GetFlyerMaxSpeed,
// GetFlyerMidSpeed and GetFlyerMinSpeed, the only readers of these fixed
// values): MaxSpeed to at most 60, MidSpeed to at most 20, and MinSpeed to a
// fifth of itself, at most 10. BoostSpeed is left alone.
constexpr float kLandSpeedMax     = 60.0f;
constexpr float kLandSpeedMid     = 20.0f;
constexpr float kLandSpeedMin     = 10.0f;
constexpr float kLandSpeedMinMult = 0.2f;

inline FlyerSpeeds landing_speeds(FlyerSpeeds s)
{
   s.max = std::fmin(s.max, kLandSpeedMax);
   s.mid = std::fmin(s.mid, kLandSpeedMid);
   s.min = std::fmin(s.min * kLandSpeedMinMult, kLandSpeedMin);
   return s;
}

// The speed BF2 is taking a flyer to: its BoostSpeed while it boosts, if it
// has one, and otherwise the forward and back input's point on its speeds,
// from MidSpeed toward MaxSpeed forward and toward MinSpeed back, capped
// inside a landing region, as EntityFlyer::Update works it out.
inline float throttle_target(const FlyerSpeeds& speeds, float move, bool boosting, bool landing = false)
{
   if (boosting && speeds.boost > 0.0f) return speeds.boost;
   const FlyerSpeeds s = landing ? landing_speeds(speeds) : speeds;
   const float m = std::isfinite(move) ? clampf(move, -1.0f, 1.0f) : 0.0f;
   return m >= 0.0f ? s.mid + (s.max - s.mid) * m : s.mid + (s.mid - s.min) * m;
}

// The throttle as the pilot holds it. BF2 reads the throttle and the roll as
// one stick and scales both down when together they reach past its rim
// (PlayerController::Update), so full throttle held through a roll reaches the
// flyer as 0.71, and it steers toward a lower speed until the roll ends. A
// pair on the rim is taken back out to the edge of the square, which keeps a
// held key full; anything inside the rim is left as it is.
constexpr float kStickRim = 0.98f;   // squared length that counts as on the rim

inline float throttle_intent(float move, float roll)
{
   if (!std::isfinite(move) || !std::isfinite(roll)) return move;
   const float larger = std::fmax(std::fabs(move), std::fabs(roll));
   if (!(larger > 0.0f) || move * move + roll * roll < kStickRim) return move;
   return move / larger;
}

// How far short of that speed a flyer still counts as speeding up or slowing
// down: a twentieth of its speed range. Steering drops the speed along the
// nose by a metre or two a second, which BF2 then speeds back up from; that
// stays inside it, while a change of throttle is well outside.
constexpr float kHeadingShare = 0.05f;

inline float heading_margin(const FlyerSpeeds& s)
{
   return std::fmax(1.0f, kHeadingShare * std::fabs(std::fmax(s.max, s.boost) - s.min));
}

// Where the throttle cannot be read, speeding up or slowing down more gently
// than this, in m/s per second, counts as holding speed.
constexpr float kHoldingSpeed = 2.0f;

// A speed-up the pilot asked for: the throttle, or a boost, raising the speed
// it asks for by more than the margin since the last speed-up got there.
// `asked` is the speed the throttle as held asks for (throttle_intent), and
// `target` the one BF2 is steering toward, which a roll lowers until it ends.
// Getting back what a roll or a hard turn cost is not a speed-up, so once the
// flyer arrives only a rise of `asked` starts another.
struct SpeedUp {
   bool  on   = false;   // under way
   bool  seen = false;   // `low` holds a speed
   float low  = 0.0f;    // the lowest asked-for speed since the last speed-up arrived

   void reset() { *this = SpeedUp{}; }

   // Whether the flyer is speeding up now.
   bool update(float asked, float target, float speed, float margin)
   {
      if (!std::isfinite(asked) || !std::isfinite(target) || !std::isfinite(speed)) return false;
      if (!seen || asked < low) {
         low  = asked;
         seen = true;
      }
      if (asked - low > margin) on = true;
      if (on && target - speed <= margin) {
         on  = false;
         low = asked;
      }
      return on;
   }
};

// BrakeShake counts downward: one value means at or below it. A pair runs
// from its first end to its second, as everywhere else.
inline float brake_position(const Threshold& th, const FlyerSpeeds& s, float v)
{
   if (th.pair) return threshold_level(th, s, v);
   float a;
   if (!std::isfinite(v) || !bound_value(th.from, s, a)) return 0.0f;
   return v <= a ? 1.0f : 0.0f;
}

// How fast a flyer's nose swings round, in radians a second, from its forward
// axis a frame apart. Rolling alone does not move the nose, so it does not
// count. Measured rather than read off the controls because the mouse never
// builds BF2's own hard-turn count (Controllable::mTurnBuildup), which only
// grows while a stick is held near full.
inline float nose_turn_rate(const float before[3], const float now[3], float dt)
{
   if (!(dt > 0.0f)) return 0.0f;
   const float lb = std::sqrt(before[0] * before[0] + before[1] * before[1] + before[2] * before[2]);
   const float ln = std::sqrt(now[0] * now[0] + now[1] * now[1] + now[2] * now[2]);
   if (!(lb > 1e-6f) || !(ln > 1e-6f) || !std::isfinite(lb) || !std::isfinite(ln)) return 0.0f;
   float chord2 = 0.0f;
   for (int i = 0; i < 3; ++i) {
      const float d = now[i] / ln - before[i] / lb;
      chord2 += d * d;
   }
   return 2.0f * std::asin(clampf(0.5f * std::sqrt(chord2), 0.0f, 1.0f)) / dt;
}

// Turning hard: the nose swinging at least this share of the faster of the
// class's PitchRate and TurnRate, which is about what full stick gives it
// (EntityFlyer::Update blends the two by how far each way the stick is
// pushed). A class with neither counts from 45 degrees a second.
constexpr float kHardTurnShare    = 0.6f;
constexpr float kHardTurnFallback = 0.7853982f;

inline float hard_turn_rate(float pitchRate, float turnRate)
{
   const float r = std::fmax(pitchRate, turnRate);
   return std::isfinite(r) && r > 0.0f ? kHardTurnShare * r : kHardTurnFallback;
}

// Seconds of hard turning. It counts while the flyer turns hard and holds
// through a lull of up to kTurnLull, such as a mouse between two pushes; a
// longer one starts it again.
constexpr float kTurnLull = 0.25f;

struct TurnCount {
   float time = 0.0f;
   float lull = 0.0f;

   void reset() { *this = TurnCount{}; }

   void update(bool hard, float dt)
   {
      if (!(dt > 0.0f)) return;
      if (hard) {
         time += dt;
         lull = 0.0f;
         return;
      }
      lull += dt;
      if (lull > kTurnLull) time = 0.0f;
   }
};

// Braking: the forward and back input held at least this far back. Letting
// go of the throttle also slows a flyer down, back to its MidSpeed, but that
// is not braking.
constexpr float kBrakeInput = 0.1f;

// CollisionShake's size for a bump: nothing short of the threshold; a pair
// starts at kBumpLeast of full at its first end and grows to full at its
// second, so a graze still registers; one value gives full size past it.
constexpr float kBumpLeast = 0.3f;

inline float bump_scale(const Threshold& th, const FlyerSpeeds& s, float impact)
{
   if (!threshold_reached(th, s, impact)) return 0.0f;
   if (!th.pair) return 1.0f;
   return kBumpLeast + (1.0f - kBumpLeast) * threshold_level(th, s, impact);
}

// BF2's own collision impact, as its flyer collision code works it out: the
// flyer's speed just after the bump / 40 + 0.01, at most 1. It passes
// impact x 0.8 as the stock shake's amount; a speed back from that stops at
// 40 m/s.
constexpr float kStockBumpAmount = 0.8f;

inline float stock_bump_speed(float amount)
{
   const float impact = amount / kStockBumpAmount;
   return std::isfinite(impact) ? clampf((impact - 0.01f) * 40.0f, 0.0f, 40.0f) : 0.0f;
}

// Walkers. A walker class's speeds for a Threshold: MaxSpeed and BoostSpeed are
// its own; it has no MinSpeed, which reads as 0, and no cruising MidSpeed,
// which reads as MaxSpeed.
inline FlyerSpeeds walker_speeds(float maxSpeed, float boostSpeed)
{
   FlyerSpeeds s;
   s.min = 0.0f;
   s.mid = maxSpeed;
   s.max = maxSpeed;
   s.boost = boostSpeed;
   s.known = std::isfinite(maxSpeed) && std::isfinite(boostSpeed);
   return s;
}

// The bits of mFootState that are feet: BF2 sets foot i's bit for the update
// it lands in and clears it in the next.
inline uint32_t feet_mask(int feet)
{
   if (feet <= 0) return 0;
   return feet >= 6 ? 0x3Fu : (1u << feet) - 1u;
}

// Feet that landed since the last look: only a bit that has come on counts,
// so a camera looking twice between two updates sees a step once.
inline uint32_t feet_landed(uint32_t before, uint32_t now) { return now & ~before; }

// Which way a step rolls the view, toward the foot that landed: -1 for left
// feet alone, 1 for right feet alone, 0 for both. The feet go left, right in
// each pair, the order the ODF gives TerrainLeft and TerrainRight in.
inline int step_side(uint32_t landed)
{
   const bool left  = (landed & 0x15u) != 0;   // feet 0, 2, 4
   const bool right = (landed & 0x2Au) != 0;   // feet 1, 3, 5
   return left == right ? 0 : (left ? -1 : 1);
}

// A walker's time off the ground, from BF2's own count (seconds since its feet
// last touched it, zeroed on contact), and how fast it was coming down. A
// walker in its stride touches the ground nearly every update, so only a
// drop of at least kWalkerMinAirtime counts as a landing.
constexpr float kWalkerMinAirtime = 0.5f;

struct WalkerAir {
   float air  = -1.0f;   // last look's count; -1 before the first
   float fall = 0.0f;    // fastest drop since it left the ground, m/s

   void reset() { *this = WalkerAir{}; }

   // The speed it came down at when it lands after kWalkerMinAirtime or more,
   // else -1. `vy` is its vertical velocity, up positive.
   float update(float count, float vy)
   {
      if (!std::isfinite(count)) return -1.0f;
      float landed = -1.0f;
      if (count > 0.0f && std::isfinite(vy)) fall = std::fmax(fall, -vy);
      if (air >= kWalkerMinAirtime && count < air) landed = fall;
      if (count < air || count <= 0.0f) fall = 0.0f;
      air = count;
      return landed;
   }
};

// One shot per weapon per frame. BF2 signals fire from every round it makes
// (Ordnance::Ordnance calls the firing weapon's SignalFire), so a shotgun's
// pellets, or a salvo with no delay between its shots, signal several times
// at once: they are one shot and kick once. A salvo with a delay spreads its
// shots over frames and kicks for each.
struct ShotGate {
   static constexpr int kWeapons = 4;
   double      time = -1.0;
   const void* fired[kWeapons] = {};
   int         count = 0;

   // True for a weapon's first signal at this time.
   bool first(const void* weapon, double t)
   {
      if (t != time) {
         time = t;
         count = 0;
      }
      for (int i = 0; i < count; ++i)
         if (fired[i] == weapon) return false;
      if (count < kWeapons) fired[count++] = weapon;
      return true;
   }
};

// Melee. A team from GameObject::mTeam, a 4-bit signed field; two objects are
// teammates when they share a team above 0 (team 0 is no one's teammate).
inline int team_from_bits(uint32_t raw) { return static_cast<int32_t>(raw << 28) >> 28; }
inline bool same_team(int a, int b) { return a > 0 && a == b; }

// What a swing had struck before an update, to tell what it struck during it.
// BF2 keeps a list per attack of the swing (WeaponMelee's DamageData), each
// object once, listed when the blade first reaches it, blocked or not. A new
// attack goes on the front of the list. Up to kMeleeAttacks are kept; a walk
// of the list after the update stops at the same number, so an attack it
// reaches is either new or in the tally.
constexpr int kMeleeAttacks = 16;

struct MeleeTally {
   const void* attack[kMeleeAttacks] = {};
   int listed[kMeleeAttacks] = {};
   int attacks = 0;

   void add(const void* a, int n)
   {
      if (attacks >= kMeleeAttacks) return;
      attack[attacks] = a;
      listed[attacks] = n;
      ++attacks;
   }

   // How many of an attack's objects were listed already: none for an
   // attack that is new since.
   int before(const void* a) const
   {
      for (int i = 0; i < attacks; ++i)
         if (attack[i] == a) return listed[i];
      return 0;
   }
};

// Turns the watched unit's health into hits. A drop of at least the threshold
// hits at once; drops during the cooldown after it gather and hit when it ends,
// so steady damage makes a few shakes a second rather than one a frame. What has
// gathered leaks away at the threshold every kHitLeak seconds, so a drain slower
// than that (a hero's, or regeneration's noise) never adds up to a hit.
constexpr float kHitCooldown = 0.15f;   // seconds
constexpr float kHitLeak     = 0.5f;    // seconds
constexpr float kHitFull     = 0.1f;    // a hit of this share of max health shakes fully
constexpr float kHitLeast    = 0.25f;   // the smallest hit that counts shakes this much

inline float hit_threshold(float maxHealth) { return std::fmax(1.0f, 0.005f * maxHealth); }

inline float hit_scale(float damage, float maxHealth)
{
   if (!(maxHealth > 0.0f)) return 1.0f;
   return clampf(damage / (kHitFull * maxHealth), kHitLeast, 1.0f);
}

struct HitSense {
   float prev     = -1.0f;
   float pending  = 0.0f;
   float cooldown = 0.0f;

   void reset() { *this = HitSense{}; }

   // The damage to shake for now, or 0.
   float update(float health, float threshold, float dt)
   {
      if (!std::isfinite(health)) return 0.0f;
      if (prev < 0.0f) {
         prev = health;
         return 0.0f;
      }
      const float drop = prev - health;
      prev = health;
      pending = std::fmax(0.0f, pending - threshold * dt / kHitLeak);
      if (drop > 0.0f) pending += drop;
      if (cooldown > 0.0f) cooldown -= dt;
      if (cooldown <= 0.0f && pending > 0.0f && pending >= threshold) {
         const float hit = pending;
         pending = 0.0f;
         cooldown = kHitCooldown;
         return hit;
      }
      return 0.0f;
   }
};

// Landings: time spent airborne, and a landing once the unit is on the ground
// again after at least kMinAirtime. Anything that is neither (a knockdown, a
// death) ends the jump without a landing.
constexpr float kMinAirtime = 0.25f;

struct AirTime {
   float air      = 0.0f;
   bool  airborne = false;

   void reset() { *this = AirTime{}; }

   bool update(bool isAirborne, bool isGrounded, float dt)
   {
      if (isAirborne) {
         if (!airborne) air = 0.0f;
         airborne = true;
         air += dt;
         return false;
      }
      const bool landed = airborne && isGrounded && air >= kMinAirtime;
      airborne = false;
      return landed;
   }
};

// -----------------------------------------------------------------------------
// Applying it
// -----------------------------------------------------------------------------

// The engine's angles: rotations about the camera's X, Y and Z.
struct Angles {
   float pitch = 0.0f;
   float yaw   = 0.0f;
   float roll  = 0.0f;
};

// Turn a camera matrix by `a` in its own space: the rows right, up and back
// (x, y, z of each) are rotated, the translation row is left alone. Same order
// as the stock shake: RotationX(pitch) * RotationY(yaw) * RotationZ(roll), then
// that times the matrix, in the row-vector convention of D3DX and PblMatrix.
// The camera looks down -Z, so a positive X turn looks up, a positive Y turn
// looks left and a positive Z turn tilts the view anticlockwise.
inline void rotate_rows(float right[3], float up[3], float back[3], const Angles& a)
{
   const float cx = std::cos(a.pitch), sx = std::sin(a.pitch);
   const float cy = std::cos(a.yaw),   sy = std::sin(a.yaw);
   const float cz = std::cos(a.roll),  sz = std::sin(a.roll);

   // X = [1 0 0; 0 cx sx; 0 -sx cx], Y = [cy 0 -sy; 0 1 0; sy 0 cy],
   // Z = [cz sz 0; -sz cz 0; 0 0 1], R = X * Y * Z.
   const float xy[3][3] = {
      { cy,        0.0f, -sy      },
      { sx * sy,   cx,    sx * cy },
      { cx * sy,  -sx,    cx * cy },
   };
   float r[3][3];
   for (int i = 0; i < 3; ++i) {
      r[i][0] = xy[i][0] * cz - xy[i][1] * sz;
      r[i][1] = xy[i][0] * sz + xy[i][1] * cz;
      r[i][2] = xy[i][2];
   }

   float out[3][3];
   const float* rows[3] = { right, up, back };
   for (int i = 0; i < 3; ++i)
      for (int c = 0; c < 3; ++c)
         out[i][c] = r[i][0] * rows[0][c] + r[i][1] * rows[1][c] + r[i][2] * rows[2][c];
   for (int c = 0; c < 3; ++c) {
      right[c] = out[0][c];
      up[c]    = out[1][c];
      back[c]  = out[2][c];
   }
}

// Moves, then turns, a camera matrix (rows right, up, back, position) by `o`.
// The move is along the unturned axes.
inline void apply(float m[16], const Offset& o)
{
   for (int c = 0; c < 3; ++c)
      m[12 + c] += m[0 + c] * o.right + m[4 + c] * o.up + m[8 + c] * o.back;
   Angles a;
   a.pitch = o.pitch;
   a.yaw   = -o.yaw;
   a.roll  = -o.roll;
   rotate_rows(m + 0, m + 4, m + 8, a);
}

// The shaken matrix left in the chase camera at the end of a frame, and the
// one BF2 made before it was shaken. BF2 builds each frame's camera from the
// last frame's matrix, easing it a share of the way to where it should be
// (CameraTrackSetting::SetupCameraTrackMatrix, by the class's MoveTension and
// the frame time), so a shake left there piles up: each frame's push lands on
// the last one's, by more the higher the frame rate. Handing BF2 its own
// matrix back before it runs again keeps a shake an offset on top of its
// camera, the same at any frame rate.
struct LeftShake {
   const void* camera = nullptr;   // the chase camera it was left in, or none
   float shaken[16]   = {};
   float unshaken[16] = {};

   void leave(const void* cam, const float before[16], const float after[16])
   {
      camera = cam;
      std::memcpy(unshaken, before, sizeof unshaken);
      std::memcpy(shaken, after, sizeof shaken);
   }

   // Puts BF2's own matrix back in `m`, once, while `m` still holds the
   // shaken one: anything that has set it since is left alone. True when it
   // did.
   bool put_back(const void* cam, float m[16])
   {
      if (!camera || cam != camera) return false;
      camera = nullptr;
      if (std::memcmp(m, shaken, sizeof shaken) != 0) return false;
      std::memcpy(m, unshaken, sizeof unshaken);
      return true;
   }
};

} // namespace camera_shake
