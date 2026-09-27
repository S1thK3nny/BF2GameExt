#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>

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
//  - A held shake, while a state lasts (sprinting, boosting, braking, the
//    stock blast queue). It swings at its rate, scaled by how strongly the
//    state holds at the moment.
//
// Everything is sampled by time, so the result is the same at any frame rate.
// The shapes follow the BFIII-derived camera spec: fire, hit and landing are
// smoothstep kicks, sprinting a railed judder, a blast three slow sines on roll
// and position.
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

// Past these a shake stops growing: an ODF typo must not spin the view round or
// throw the camera through a wall.
constexpr float kMaxAngle = 0.35f;   // radians per axis, about 20 degrees
constexpr float kMaxMove  = 0.5f;    // metres per axis

inline Offset clamp_offset(const Offset& o)
{
   Offset c;
   c.pitch = clampf(o.pitch, -kMaxAngle, kMaxAngle);
   c.yaw   = clampf(o.yaw,   -kMaxAngle, kMaxAngle);
   c.roll  = clampf(o.roll,  -kMaxAngle, kMaxAngle);
   c.right = clampf(o.right, -kMaxMove, kMaxMove);
   c.up    = clampf(o.up,    -kMaxMove, kMaxMove);
   c.back  = clampf(o.back,  -kMaxMove, kMaxMove);
   return c;
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

// One shake's settings: angles in degrees and movement in metres, as an ODF
// gives them. Length and rise only mean something to a one-off shake.
struct Shape {
   Range pitch, yaw, roll, push;
   float length = 0.0f;   // seconds
   float rise   = 0.0f;   // share of the length spent rising, 0..1
   float rate   = 0.0f;   // swings per second; 0 makes a one-off shake a single push
   // One-off: how many shakes' worth repeats can pile up to (see KickChannel).
   // Blast: the amount of stock Shake it levels off at (see blast_amount).
   float limit  = 0.0f;
};

// The defaults, used for whatever a class leaves unset. Fire, hit, landing,
// sprint and blast are the spec's tune; the rest are first guesses.
namespace defaults {
// pitch, yaw, roll, push, length, rise, rate, limit
constexpr Shape kFire         = { { 0.25f, 0.4f }, { -0.12f, 0.12f }, { 0.0f, 0.0f }, { 0.015f, 0.015f }, 0.22f, 0.25f, 0.0f, 1.0f };
constexpr Shape kHit          = { { 2.0f, 4.0f }, { -2.0f, 2.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, 0.25f, 0.3f, 0.0f, 1.0f };
constexpr Shape kLandSoldier  = { { -2.25f, -2.25f }, { 0.92f, 0.92f }, { 0.44f, 0.44f }, { 0.0f, 0.0f }, 0.67f, 0.15f, 0.0f, 1.0f };
constexpr Shape kLandFlyer    = { { -1.5f, -1.5f }, { 0.0f, 0.0f }, { -0.5f, 0.5f }, { 0.0f, 0.0f }, 0.5f, 0.12f, 0.0f, 1.0f };
constexpr Shape kRollSoldier  = { { 1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.5f, 1.5f }, { 0.0f, 0.0f }, 0.4f, 0.1f, 5.0f, 1.0f };
constexpr Shape kRollFlyer    = { { 1.0f, 1.0f }, { 1.0f, 1.0f }, { 2.0f, 2.0f }, { 0.0f, 0.0f }, 0.6f, 0.1f, 5.0f, 1.0f };
constexpr Shape kSprintSoldier = { { 0.16f, 0.16f }, { 0.06f, 0.06f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, 0.0f, 0.0f, 4.0f, 0.0f };
constexpr Shape kSprintFlyer  = { { 0.3f, 0.3f }, { 0.3f, 0.3f }, { 0.2f, 0.2f }, { 0.0f, 0.0f }, 0.0f, 0.0f, 16.0f, 0.0f };
constexpr Shape kBrake        = { { 0.4f, 0.4f }, { 0.4f, 0.4f }, { 0.2f, 0.2f }, { 0.0f, 0.0f }, 0.0f, 0.0f, 11.0f, 0.0f };
// Per unit of the stock `Shake` amount.
constexpr Shape kBlast        = { { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 6.0f, 6.0f }, { 0.03f, 0.03f }, 0.0f, 0.0f, 3.0f, 2.5f };
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

   bool alive() const { return age < length; }

   Offset value(double t) const
   {
      if (!alive()) return {};
      Offset v = peak;
      if (rate > 0.0f) {
         v.pitch *= noise(t, rate, kPhasePitch + phase);
         v.yaw   *= noise(t, rate, kPhaseYaw + phase);
         v.roll  *= noise(t, rate, kPhaseRoll + phase);
         v.right *= noise(t, rate, kPhaseRight + phase);
         v.up    *= noise(t, rate, kPhaseUp + phase);
         v.back  *= noise(t, rate, kPhaseBack + phase);
      }
      v = v.scaled(envelope(age, length, rise));
      const float up = clamp01(rise) * length;
      if (age < up) v += from.scaled(1.0f - smoothstep(age / up));
      return v;
   }
};

// A kick drawn from a shape: each angle and the push picked in its range, then
// scaled. The push goes back along the view.
inline Kick make_kick(const Shape& s, float scale, Rng& rng)
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
   return k;
}

// How much of where the view stands a repeat carries into its own peak, for a
// limit of that many shakes' worth. Carrying (1 - 1/limit) each time settles at
// the limit when repeats come faster than a shake rises: x = p + x(1 - 1/L)
// gives x = L * p. A limit of 1 carries nothing, so a repeat just restarts.
inline float carry_for(float limit) { return limit > 1.0f ? 1.0f - 1.0f / limit : 0.0f; }

// One shake at a time, for every one-off shake. A repeat restarts the channel
// from wherever it stands, so the view never jumps, and carries part of it into
// its peak (carry_for), so rapid repeats pile up only as far as the limit.
class KickChannel {
public:
   void trigger(Kick k, double t, float limit = 1.0f)
   {
      const Offset now = m_active ? m_kick.value(t) : Offset{};
      k.from = now;
      k.peak += now.scaled(carry_for(limit));
      k.age  = 0.0f;
      m_kick = k;
      m_active = k.length > 0.0f;
   }

   void advance(float dt)
   {
      if (!m_active) return;
      m_kick.age += dt;
      if (!m_kick.alive()) m_active = false;
   }

   Offset value(double t) const { return m_active ? m_kick.value(t) : Offset{}; }
   bool active() const { return m_active; }
   void clear() { m_active = false; }

private:
   Kick m_kick;
   bool m_active = false;
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
   o.right = m * noise(t, s.rate, kPhaseRight);
   o.up    = m * noise(t, s.rate, kPhaseUp);
   o.back  = m * noise(t, s.rate, kPhaseBack);
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
   o.right = m * sy;
   o.up    = m * sx;
   o.back  = m * sz;
   return o;
}

// How hard a flyer is braking, 0..1, from its deceleration in units per second
// squared: nothing at kBrakeFloor, full at kBrakeFull.
constexpr float kBrakeFloor = 4.0f;
constexpr float kBrakeFull  = 30.0f;

inline float brake_intensity(float deceleration)
{
   return clamp01((deceleration - kBrakeFloor) / (kBrakeFull - kBrakeFloor));
}

// -----------------------------------------------------------------------------
// Triggers
// -----------------------------------------------------------------------------

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

} // namespace camera_shake
