// Standalone tests (not a DLL build). Compile with /std:c++17 /W4 /WX, no NDEBUG.
#include "../PatcherDLL/src/render/camera_shake_core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>

using namespace camera_shake;

namespace {

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

bool near_offset(const Offset& a, const Offset& b, float eps = 1e-4f)
{
   return near(a.pitch, b.pitch, eps) && near(a.yaw, b.yaw, eps) && near(a.roll, b.roll, eps) &&
          near(a.right, b.right, eps) && near(a.up, b.up, eps) && near(a.back, b.back, eps);
}

// D3DX-style row-major 3x3 multiply, the reference rotate_rows must match.
void mul(const float a[3][3], const float b[3][3], float out[3][3])
{
   for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
         out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
}

void rot_x(float a, float m[3][3])
{
   const float c = std::cos(a), s = std::sin(a);
   const float r[3][3] = { { 1, 0, 0 }, { 0, c, s }, { 0, -s, c } };
   for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) m[i][j] = r[i][j];
}

void rot_y(float a, float m[3][3])
{
   const float c = std::cos(a), s = std::sin(a);
   const float r[3][3] = { { c, 0, -s }, { 0, 1, 0 }, { s, 0, c } };
   for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) m[i][j] = r[i][j];
}

void rot_z(float a, float m[3][3])
{
   const float c = std::cos(a), s = std::sin(a);
   const float r[3][3] = { { c, s, 0 }, { -s, c, 0 }, { 0, 0, 1 } };
   for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) m[i][j] = r[i][j];
}

float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// A small deterministic generator, so the test needs no <random> seeding.
unsigned s_seed = 12345u;
float rnd(float lo, float hi)
{
   s_seed = s_seed * 1664525u + 1013904223u;
   return lo + (hi - lo) * static_cast<float>(s_seed >> 8) / static_cast<float>(1u << 24);
}

// An identity camera: right +X, up +Y, back +Z, at the origin.
void identity(float m[16])
{
   for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
}

} // namespace

int main()
{
   // The noise stays in -1..1 and moves smoothly: its slope is bounded by the
   // sum of its components' slopes, so consecutive samples never jump.
   for (float hz : { 2.2f, 3.0f, 4.0f, 5.0f, 9.0f, 11.0f, 16.0f }) {
      const float maxSlope = 2.0f * kPi * hz * (1.0f + 0.5f * 1.93f + 0.25f * 3.07f) / 1.75f;
      float prev = noise(0.0, hz, kPhaseYaw);
      for (int i = 1; i <= 20000; ++i) {
         const double t = i * 0.0005;
         const float n = noise(t, hz, kPhaseYaw);
         assert(n >= -1.0f && n <= 1.0f);
         assert(std::fabs(n - prev) <= maxSlope * 0.0005f + 1e-4f);
         prev = n;
      }
   }

   // The envelope: 0 at the start, 1 at the peak, 0 at the end, rising and then
   // falling without a step. A rise of 0 starts at full strength.
   {
      assert(envelope(0.0f, 1.0f, 0.25f) == 0.0f);
      assert(near(envelope(0.25f, 1.0f, 0.25f), 1.0f));
      assert(envelope(1.0f, 1.0f, 0.25f) == 0.0f);
      assert(envelope(0.5f, 0.0f, 0.25f) == 0.0f);
      assert(near(envelope(0.0f, 1.0f, 0.0f), 1.0f));
      float prev = 0.0f;
      for (int i = 1; i <= 25; ++i) {
         const float e = envelope(i * 0.01f, 1.0f, 0.25f);
         assert(e >= prev - 1e-6f);
         prev = e;
      }
      for (int i = 26; i < 100; ++i) {
         const float e = envelope(i * 0.01f, 1.0f, 0.25f);
         assert(e <= prev + 1e-6f && std::fabs(e - prev) < 0.05f);
         prev = e;
      }
   }

   // ODF values: one number or two in either order; nothing unreadable is kept.
   {
      Range r;
      assert(parse_range("0.25 0.4", r) && r.lo == 0.25f && r.hi == 0.4f);
      assert(parse_range("0.4 0.25", r) && r.lo == 0.25f && r.hi == 0.4f);
      assert(parse_range("-2.25", r) && r.lo == -2.25f && r.hi == -2.25f);
      assert(parse_range("  -2 2", r) && r.lo == -2.0f && r.hi == 2.0f);
      assert(parse_range("1 x", r) && r.lo == 1.0f && r.hi == 1.0f);
      Range keep = { 7.0f, 8.0f };
      assert(!parse_range("abc", keep) && keep.lo == 7.0f && keep.hi == 8.0f);
      assert(!parse_range("", keep) && !parse_range(nullptr, keep));
      assert(!parse_range("nan", keep) && !parse_range("inf 1", keep));
      assert(near(Range{ -3.0f, 2.0f }.largest(), 3.0f));

      float v = 5.0f;
      assert(parse_amount("1.5", v) && v == 1.5f);
      assert(parse_amount("0", v) && v == 0.0f);
      v = 5.0f;
      assert(!parse_amount("-1", v) && v == 5.0f);
      assert(!parse_amount("x", v) && !parse_amount(nullptr, v) && !parse_amount("nan", v));
   }

   // The generator stays in range and repeats for a seed.
   {
      Rng a(99u), b(99u);
      for (int i = 0; i < 10000; ++i) {
         const float u = a.unit();
         assert(u >= 0.0f && u < 1.0f && u == b.unit());
         const float x = a.in(Range{ -2.0f, 3.0f });
         assert(x >= -2.0f && x <= 3.0f);
         b.in(Range{ -2.0f, 3.0f });
      }
      Rng zero(0u);
      assert(zero.next() != 0u);   // a zero seed must not stick at zero
   }

   // A kick drawn from a shape: angles in their ranges, in radians, scaled; the
   // push goes back along the view; the timing is copied.
   {
      Rng rng(7u);
      for (int i = 0; i < 1000; ++i) {
         const Kick k = make_kick(defaults::kFire, 2.0f, rng);
         assert(k.peak.pitch >= 0.25f * kDegToRad * 2.0f - 1e-6f && k.peak.pitch <= 0.4f * kDegToRad * 2.0f + 1e-6f);
         assert(std::fabs(k.peak.yaw) <= 0.12f * kDegToRad * 2.0f + 1e-6f);
         assert(k.peak.roll == 0.0f && near(k.peak.back, 0.12f) && k.peak.right == 0.0f && k.peak.up == 0.0f);
         assert(k.length == defaults::kFire.length && k.rise == defaults::kFire.rise && k.rate == 0.0f);
      }
   }

   // A kick at rate 0 is its peak at the top of the envelope, nothing at its
   // length, and ages by time alone: 30 or 240 frames a second end the same.
   {
      Rng rng(3u);
      Kick k = make_kick(defaults::kHit, 1.0f, rng);
      k.age = defaults::kHit.length * defaults::kHit.rise;
      assert(near_offset(k.value(1.0), k.peak));
      k.age = defaults::kHit.length;
      assert(k.value(1.0).negligible() && !k.alive());

      KickChannel slow, fast;
      Rng r1(5u), r2(5u);
      slow.trigger(make_kick(defaults::kLandSoldier, 1.0f, r1), 0.0);
      fast.trigger(make_kick(defaults::kLandSoldier, 1.0f, r2), 0.0);
      for (int i = 0; i < 9; ++i) slow.advance(1.0f / 30.0f);
      for (int i = 0; i < 72; ++i) fast.advance(1.0f / 240.0f);
      assert(near_offset(slow.value(0.3), fast.value(0.3), 1e-4f));
   }

   // A swinging kick stays inside its peak and actually swings.
   {
      Rng rng(11u);
      Kick k = make_kick(defaults::kTrickRoll, 1.0f, rng);
      bool positive = false, negative = false;
      for (int i = 0; i < 400; ++i) {
         k.age = i * 0.001f;
         const Offset v = k.value(i * 0.001);
         assert(std::fabs(v.roll) <= std::fabs(k.peak.roll) + 1e-6f);
         positive |= v.roll > 1e-4f;
         negative |= v.roll < -1e-4f;
      }
      assert(positive && negative);
   }

   // The limit is how many run at once: a whole number from 1 to 8.
   assert(kick_cap(1.0f) == 1 && kick_cap(0.0f) == 1 && kick_cap(1.9f) == 1);
   assert(kick_cap(std::numeric_limits<float>::quiet_NaN()) == 1);
   assert(kick_cap(2.0f) == 2 && kick_cap(3.7f) == 3 && kick_cap(8.0f) == 8 && kick_cap(100.0f) == 8);

   // The reference model's fire kick, frame by frame: advance first, then
   // sample, the first sample a frame in. Its own sheet's numbers at 60 and 30
   // frames a second, and gone on the frame after its last.
   {
      Kick k;
      k.peak.pitch = 1.0f;
      k.length = defaults::kFire.length;
      k.rise = defaults::kFire.rise;
      const float at60[] = { 0.2198f, 0.6567f, 0.9767f, 0.9857f, 0.9217f, 0.8174f, 0.6854f,
                             0.5378f, 0.3872f, 0.2459f, 0.1262f, 0.0405f, 0.0012f };
      const float at30[] = { 0.6567f, 0.9857f, 0.8174f, 0.5378f, 0.2459f, 0.0405f };
      KickChannel ch60, ch30;
      ch60.trigger(k, 0.0, defaults::kFire.limit);
      ch30.trigger(k, 0.0, defaults::kFire.limit);
      for (float e : at60) {
         ch60.advance(1.0f / 60.0f);
         assert(near(ch60.value(0.0).pitch, e, 1e-3f));
      }
      for (float e : at30) {
         ch30.advance(1.0f / 30.0f);
         assert(near(ch30.value(0.0).pitch, e, 1e-3f));
      }
      ch60.advance(1.0f / 60.0f);
      ch30.advance(1.0f / 30.0f);
      assert(!ch60.active() && !ch30.active());
      // A shot 0.2 s on finds the last one at about 4%: next to no stacking.
      assert(near(envelope(0.2f, defaults::kFire.length, defaults::kFire.rise), 0.0405f, 1e-3f));
   }

   // Shots add together: the channel is every running kick's envelope summed,
   // as the reference model sums its kicks, for a chaingun firing every 0.08 s.
   {
      Kick k;
      k.peak.pitch = 1.0f;
      k.length = defaults::kFire.length;
      k.rise = defaults::kFire.rise;
      KickChannel ch;
      const float dt = 1.0f / 60.0f, every = 0.08f;
      float shots[64];
      int fired = 0;
      float t = 0.0f, nextShot = 0.0f, highest = 0.0f;
      for (int frame = 0; frame < 120; ++frame) {
         while (nextShot <= t && fired < 64) {
            ch.trigger(k, t, defaults::kFire.limit);
            shots[fired++] = t;                      // it starts on this frame
            nextShot += every;
         }
         ch.advance(dt);
         t += dt;
         float sum = 0.0f;
         for (int i = 0; i < fired; ++i) sum += envelope(t - shots[i], k.length, k.rise);
         assert(near(ch.value(t).pitch, sum, 2e-3f));
         highest = std::fmax(highest, sum);
      }
      assert(highest > 1.5f && ch.count() <= 3);   // they pile up past one kick
   }

   // At a limit, the oldest makes room by fading out: never more than that
   // many kicks' worth, and never a jump from one frame to the next.
   {
      for (float limit : { 1.0f, 2.0f, 3.0f }) {
         Kick k;
         k.peak.pitch = 0.01f;
         k.length = 0.6f;
         k.rise = 0.25f;
         KickChannel ch;
         double t = 0.0;
         float highest = 0.0f;
         for (int shot = 0; shot < 200; ++shot) {   // a shot every 0.05 s; a kick lasts 0.6 s
            const float before = ch.value(t).pitch;
            ch.trigger(k, t, limit);
            assert(ch.count() <= kick_cap(limit));
            assert(near(ch.value(t).pitch, before, 1e-6f));   // a shot never jumps the view
            for (int f = 0; f < 5; ++f) {
               ch.advance(0.01f);
               t += 0.01;
               highest = std::fmax(highest, ch.value(t).pitch);
            }
         }
         assert(highest <= 0.01f * limit * 1.001f);
         assert(highest >= 0.01f * limit * 0.9f);
      }
      // Slower than a kick lasts, nothing stacks whatever the limit.
      Kick k;
      k.peak.pitch = 0.01f;
      k.length = 0.22f;
      k.rise = 0.25f;
      KickChannel slowFire;
      double t = 0.0;
      float highest = 0.0f;
      for (int shot = 0; shot < 50; ++shot) {       // every 0.3 s: each has faded first
         slowFire.trigger(k, t, 8.0f);
         for (int f = 0; f < 30; ++f) {
            slowFire.advance(0.01f);
            t += 0.01;
            highest = std::fmax(highest, slowFire.value(t).pitch);
         }
      }
      assert(highest <= 0.01f * 1.001f);
      KickChannel none;
      Kick empty = k;
      empty.length = 0.0f;
      none.trigger(empty, 0.0);
      assert(!none.active());
   }

   // A restart starts from wherever the channel stands, so the view never jumps.
   {
      Rng rng(21u);
      KickChannel ch;
      ch.trigger(make_kick(defaults::kHit, 1.0f, rng), 0.0);
      ch.advance(0.1f);
      const Offset before = ch.value(0.1);
      assert(!before.negligible());
      ch.trigger(make_kick(defaults::kHit, 1.0f, rng), 0.1);
      assert(near_offset(ch.value(0.1), before, 1e-6f));
      ch.advance(0.001f);
      assert(near_offset(ch.value(0.101), before, 0.002f));
      ch.advance(1.0f);
      assert(!ch.active() && ch.value(2.0).negligible());
   }

   // approach moves by at most rate * dt and stops on the target.
   assert(near(approach(0.0f, 1.0f, 3.0f, 0.1f), 0.3f));
   assert(approach(0.9f, 1.0f, 3.0f, 0.1f) == 1.0f);
   assert(near(approach(1.0f, 0.0f, 3.0f, 0.1f), 0.7f));

   // A hold eases toward its target without overshooting, faster up than down,
   // and the frame rate does not change where it ends up.
   {
      Hold up, slowUp;
      for (int i = 0; i < 100; ++i) up.advance(2.0f, 0.001f, 0.1f, 0.3f);
      for (int i = 0; i < 10; ++i) slowUp.advance(2.0f, 0.01f, 0.1f, 0.3f);
      assert(near(up.level, slowUp.level, 1e-4f));
      assert(up.level > 0.0f && up.level < 2.0f);
      Hold riseTest, fallTest;
      fallTest.level = 1.0f;
      riseTest.advance(1.0f, 0.1f, 0.1f, 0.3f);
      fallTest.advance(0.0f, 0.1f, 0.1f, 0.3f);
      assert(riseTest.level > 1.0f - fallTest.level);
      Hold instant;
      instant.advance(3.0f, 0.016f, 0.0f, 0.1f);
      assert(instant.level == 3.0f);
   }

   // A held sway: nothing at level 0, linear in its level, inside its amplitudes.
   {
      assert(sway(defaults::kBrake, 1.0, 0.0f).negligible());
      const Offset one = sway(defaults::kBrake, 0.77, 1.0f);
      const Offset two = sway(defaults::kBrake, 0.77, 2.0f);
      assert(near(two.pitch, 2.0f * one.pitch) && near(two.roll, 2.0f * one.roll));
      for (int i = 0; i < 5000; ++i) {
         const Offset o = sway(defaults::kBrake, i * 0.003, 1.0f);
         assert(std::fabs(o.pitch) <= 0.4f * kDegToRad + 1e-6f && std::fabs(o.roll) <= 0.2f * kDegToRad + 1e-6f);
      }
   }

   // The sprint judder is railed: it spends most of its time at its limits, and
   // never goes past them.
   {
      const Shape& s = defaults::kSprintSoldier;
      int atRail = 0;
      const int n = 20000;
      for (int i = 0; i < n; ++i) {
         const Offset o = judder(s, i * 0.0005, 1.0f);
         assert(std::fabs(o.pitch) <= s.pitch.largest() * kDegToRad + 1e-6f);
         assert(std::fabs(o.yaw) <= s.yaw.largest() * kDegToRad + 1e-6f);
         if (std::fabs(std::fabs(o.pitch) - s.pitch.largest() * kDegToRad) < 1e-6f) ++atRail;
      }
      assert(atRail > n / 2);
      Shape still = s;
      still.rate = 0.0f;
      assert(judder(still, 1.0, 1.0f).negligible());
   }

   // The blast: roll and movement only by default, inside amount * amplitude,
   // and its amount levels off toward the cap.
   {
      const float cap = defaults::kBlast.limit;
      float prev = 0.0f;
      for (int i = 1; i <= 400; ++i) {
         const float x = i * 0.025f;
         const float a = blast_amount(x, cap);
         assert(a > prev && a <= x + 1e-6f && a < cap);
         prev = a;
      }
      assert(blast_amount(0.0f, cap) == 0.0f && blast_amount(-1.0f, cap) == 0.0f);
      assert(blast_amount(1.0f, 0.0f) == 0.0f);         // a limit of 0 turns it off
      assert(near(blast_amount(0.1f, cap), 0.1f, 1e-3f));   // small shakes are unchanged
      assert(blast_amount(3.0f, 5.0f) > blast_amount(3.0f, cap));
      for (int i = 0; i < 5000; ++i) {
         const Offset o = blast(defaults::kBlast, i * 0.004, 1.0f);
         assert(o.pitch == 0.0f && o.yaw == 0.0f);
         assert(std::fabs(o.roll) <= 6.0f * kDegToRad + 1e-6f);
         assert(std::fabs(o.right) <= 0.08f + 1e-6f && std::fabs(o.back) <= 0.08f + 1e-6f);
      }
      assert(blast(defaults::kBlast, 1.0, 0.0f).negligible());
   }

   // Thresholds read like the angles: a number or a pair, and here also the
   // class's speeds by name, in any case, kept in the order written.
   {
      Threshold th;
      assert(parse_threshold("20", th) && !th.pair && th.from.value == 20.0f && th.to.value == 20.0f);
      assert(parse_threshold("4 30", th) && th.pair && th.from.value == 4.0f && th.to.value == 30.0f);
      assert(parse_threshold("30 4", th) && th.pair && th.from.value == 30.0f && th.to.value == 4.0f);
      assert(parse_threshold("MidSpeed MaxSpeed", th) && th.pair && th.from.ref == SpeedRef::Mid &&
             th.to.ref == SpeedRef::Max);
      assert(parse_threshold("maxspeed 150", th) && th.from.ref == SpeedRef::Max &&
             th.to.ref == SpeedRef::None && th.to.value == 150.0f);
      assert(parse_threshold("  BOOSTSPEED", th) && !th.pair && th.from.ref == SpeedRef::Boost);
      assert(parse_threshold("MinSpeed\t-2.5", th) && th.from.ref == SpeedRef::Min && th.to.value == -2.5f);
      assert(parse_threshold("2 junk", th) && !th.pair && th.from.value == 2.0f);
      const Threshold kept = th;
      assert(!parse_threshold("", th) && !parse_threshold("MaxSpeedy", th) && !parse_threshold("fast", th) &&
             !parse_threshold("inf", th) && !parse_threshold(nullptr, th));
      assert(th.from.value == kept.from.value && th.from.ref == kept.from.ref && th.pair == kept.pair);
   }

   // Where a value sits in a threshold: a step for one value, a ramp for a pair
   // either way round, and nothing where a speed name does not resolve.
   {
      FlyerSpeeds xwing;
      xwing.min = 35.0f;
      xwing.mid = 70.0f;
      xwing.max = 95.0f;
      xwing.boost = 150.0f;
      xwing.known = true;
      FlyerSpeeds laat = xwing;
      laat.boost = 0.0f;   // a class that cannot boost
      const FlyerSpeeds unknown;

      const Threshold step = { value_bound(20.0f), value_bound(20.0f), false };
      assert(threshold_level(step, xwing, 19.9f) == 0.0f && threshold_level(step, xwing, 20.0f) == 1.0f);
      assert(threshold_level(step, xwing, 500.0f) == 1.0f);

      const Threshold& boost = defaults::kBoost.threshold;
      assert(threshold_level(boost, xwing, 95.0f) == 0.0f && threshold_level(boost, xwing, 150.0f) == 1.0f);
      assert(near(threshold_level(boost, xwing, 122.5f), 0.5f) && threshold_level(boost, xwing, 200.0f) == 1.0f);
      assert(threshold_level(boost, laat, 200.0f) == 0.0f);      // no BoostSpeed: never
      assert(threshold_level(boost, unknown, 200.0f) == 0.0f);   // speeds not read: never
      assert(threshold_rising(boost, xwing));

      const Threshold stall = between(speed_bound(SpeedRef::Mid), speed_bound(SpeedRef::Min));
      assert(!threshold_rising(stall, xwing));
      assert(threshold_level(stall, xwing, 80.0f) == 0.0f && threshold_level(stall, xwing, 35.0f) == 1.0f);
      assert(near(threshold_level(stall, xwing, 52.5f), 0.5f));
      assert(threshold_reached(stall, xwing, 70.0f) && !threshold_reached(stall, xwing, 71.0f));

      const Threshold ramp = between(value_bound(4.0f), value_bound(30.0f));
      assert(threshold_level(ramp, unknown, 4.0f) == 0.0f && threshold_level(ramp, unknown, 30.0f) == 1.0f);
      assert(near(threshold_level(ramp, unknown, 17.0f), 0.5f));
      assert(threshold_level(ramp, unknown, std::numeric_limits<float>::quiet_NaN()) == 0.0f);

      // The brake's default runs from MinSpeed to MaxSpeed: full braking at top
      // speed or faster, fading out toward the slowest. One value counts down,
      // at or below it; a pair ramps from its first end to its second.
      const Threshold& brake = defaults::kBrake.threshold;
      assert(brake.pair && brake.from.ref == SpeedRef::Min && brake.to.ref == SpeedRef::Max);
      assert(brake_position(brake, xwing, 95.0f) == 1.0f && brake_position(brake, xwing, 150.0f) == 1.0f);
      assert(brake_position(brake, xwing, 35.0f) == 0.0f && near(brake_position(brake, xwing, 65.0f), 0.5f));
      assert(brake_position(brake, unknown, 90.0f) == 0.0f);
      const Threshold belowCruise = { speed_bound(SpeedRef::Mid), speed_bound(SpeedRef::Mid), false };
      assert(brake_position(belowCruise, xwing, 70.0f) == 1.0f && brake_position(belowCruise, xwing, 40.0f) == 1.0f);
      assert(brake_position(belowCruise, xwing, 70.5f) == 0.0f);
      const Threshold down = between(speed_bound(SpeedRef::Mid), speed_bound(SpeedRef::Min));
      assert(brake_position(down, xwing, 70.0f) == 0.0f && brake_position(down, xwing, 35.0f) == 1.0f);
      assert(near(brake_position(down, xwing, 52.5f), 0.5f));
      assert(brake_position(brake, xwing, std::numeric_limits<float>::quiet_NaN()) == 0.0f);
   }

   // The throttle's target: MidSpeed at rest, MaxSpeed full forward, MinSpeed
   // full back, BoostSpeed while boosting with one; and the margin that keeps
   // steering from counting as speeding up.
   {
      FlyerSpeeds xwing;
      xwing.min = 35.0f;
      xwing.mid = 70.0f;
      xwing.max = 95.0f;
      xwing.boost = 150.0f;
      xwing.known = true;
      assert(throttle_target(xwing, 0.0f, false) == 70.0f);
      assert(throttle_target(xwing, 1.0f, false) == 95.0f && throttle_target(xwing, -1.0f, false) == 35.0f);
      assert(near(throttle_target(xwing, 0.5f, false), 82.5f) && near(throttle_target(xwing, -0.5f, false), 52.5f));
      assert(throttle_target(xwing, 3.0f, false) == 95.0f && throttle_target(xwing, -3.0f, false) == 35.0f);
      assert(throttle_target(xwing, std::numeric_limits<float>::quiet_NaN(), false) == 70.0f);
      assert(throttle_target(xwing, 0.0f, true) == 150.0f);
      FlyerSpeeds interceptor;   // no BoostSpeed: boosting keeps the throttle's target
      interceptor.min = 27.5f;
      interceptor.mid = 55.0f;
      interceptor.max = 137.5f;
      interceptor.known = true;
      assert(throttle_target(interceptor, 1.0f, true) == 137.5f);
      // Inside a landing region BF2 caps MaxSpeed at 60, MidSpeed at 20 and
      // MinSpeed at a fifth of itself, at most 10; BoostSpeed stays.
      assert(throttle_target(interceptor, 0.0f, false, true) == 20.0f);
      assert(throttle_target(interceptor, 1.0f, false, true) == 60.0f);
      assert(near(throttle_target(interceptor, -1.0f, false, true), 5.5f));
      assert(near(throttle_target(xwing, -1.0f, false, true), 7.0f));
      assert(throttle_target(xwing, 0.0f, true, true) == 150.0f);
      FlyerSpeeds slow = interceptor;   // below every cap but MinSpeed's fifth
      slow.min = 60.0f;
      slow.mid = 15.0f;
      slow.max = 40.0f;
      assert(throttle_target(slow, 1.0f, false, true) == 40.0f && throttle_target(slow, 0.0f, false, true) == 15.0f);
      assert(throttle_target(slow, -1.0f, false, true) == 10.0f);
      assert(near(heading_margin(interceptor), 5.5f) && near(heading_margin(xwing), 5.75f));
      FlyerSpeeds tiny = interceptor;
      tiny.min = 0.0f;
      tiny.max = 10.0f;
      assert(heading_margin(tiny) == 1.0f);
   }

   // BoostShake: full while the speed heads for the far end, the steady share
   // once it settles, nothing outside the threshold.
   {
      assert(boost_level(1.0f, true, 0.25f) == 1.0f);
      assert(boost_level(1.0f, false, 0.25f) == 0.25f);
      assert(near(boost_level(0.5f, true, 0.25f), 0.5f) && near(boost_level(0.5f, false, 0.25f), 0.125f));
      assert(boost_level(0.0f, true, 1.0f) == 0.0f && boost_level(1.0f, false, 0.0f) == 0.0f);
      assert(boost_level(2.0f, true, 3.0f) == 1.0f && boost_level(1.0f, false, 3.0f) == 1.0f);
   }

   // The throttle as held: BF2 scales throttle and roll down together to its
   // stick's rim, so a held key reads 0.71 through a roll; put back, it is 1.
   // A pair inside the rim stays as it is.
   {
      const float k = 1.0f / std::sqrt(2.0f);
      assert(throttle_intent(k, k) == 1.0f && throttle_intent(k, -k) == 1.0f && throttle_intent(-k, k) == -1.0f);
      assert(throttle_intent(1.0f, 0.0f) == 1.0f && throttle_intent(0.0f, 1.0f) == 0.0f);
      assert(throttle_intent(0.0f, 0.0f) == 0.0f && throttle_intent(0.5f, 0.5f) == 0.5f);
      assert(near(throttle_intent(0.8f, 0.6f), 1.0f));   // on the rim, mostly throttle
      assert(near(throttle_intent(0.6f, 0.8f), 0.75f));
   }

   // A speed-up the pilot asked for: only a rise of the speed the throttle as
   // held asks for starts one. The climb to cruise after taking off, and speed
   // a roll or turn costs and BF2 gives back, do not count.
   {
      const float margin = 5.5f;   // the interceptor's
      const float k = 1.0f / std::sqrt(2.0f);
      FlyerSpeeds eta;
      eta.min = 27.5f;
      eta.mid = 55.0f;
      eta.max = 137.5f;
      eta.known = true;
      const auto asked = [&](float move, float roll) { return throttle_target(eta, throttle_intent(move, roll), false); };
      const auto bf2 = [&](float move) { return throttle_target(eta, move, false); };
      SpeedUp s;
      for (float v = 0.0f; v <= 55.0f; v += 5.0f) assert(!s.update(asked(0, 0), bf2(0), v, margin));   // to cruise
      assert(s.update(asked(1, 0), bf2(1), 55.0f, margin));                                            // throttle forward
      assert(s.update(asked(1, 0), bf2(1), 120.0f, margin));
      assert(!s.update(asked(1, 0), bf2(1), 133.0f, margin));                                          // within the margin: there
      // A roll with the throttle held: BF2 reads 0.71 and slows toward 113 m/s, then back.
      for (float v = 137.5f; v >= 114.0f; v -= 3.0f) assert(!s.update(asked(k, k), bf2(k), v, margin));
      for (float v = 114.0f; v <= 137.5f; v += 3.0f) assert(!s.update(asked(1, 0), bf2(1), v, margin));
      for (float v = 137.5f; v >= 125.0f; v -= 2.5f) assert(!s.update(asked(1, 0), bf2(1), v, margin));   // a turn's dip
      assert(!s.update(asked(0, 0), bf2(0), 137.5f, margin) && !s.update(asked(0, 0), bf2(0), 90.0f, margin));   // let go
      assert(s.update(asked(1, 0), bf2(1), 90.0f, margin));                                            // forward again
      assert(!s.update(asked(1, 0), bf2(1), 137.5f, margin));
      assert(s.update(275.0f, 275.0f, 137.5f, margin));                                               // a boost
      // The throttle opened during a roll: up to BF2's lower target, and the
      // rest once the roll ends is not a second one.
      SpeedUp r;
      assert(!r.update(asked(0, 0), bf2(0), 55.0f, margin));
      assert(r.update(asked(k, k), bf2(k), 60.0f, margin));
      assert(!r.update(asked(k, k), bf2(k), 110.0f, margin));
      assert(!r.update(asked(1, 0), bf2(1), 115.0f, margin));
      // A throttle eased forward counts once its rise passes the margin.
      SpeedUp a;
      assert(!a.update(55.0f, 55.0f, 55.0f, margin) && !a.update(60.0f, 60.0f, 55.0f, margin));
      assert(a.update(61.0f, 61.0f, 55.0f, margin));
      // Bad input changes nothing; reset forgets.
      assert(!a.update(std::numeric_limits<float>::quiet_NaN(), 61.0f, 55.0f, margin) && a.on);
      a.reset();
      assert(!a.on && !a.seen);
   }

   // The nose's turn rate: the angle between two forward axes a frame apart,
   // whatever their length; still, rolling or bad input reads as nothing.
   {
      const float dt = 1.0f / 60.0f;
      const float a = 1.45f * dt;              // one frame of a 1.45 rad/s turn
      const float ahead[3] = { 0.0f, 0.0f, 1.0f };
      const float yawed[3] = { std::sin(a), 0.0f, std::cos(a) };
      const float pitched[3] = { 0.0f, 2.0f * std::sin(a), 2.0f * std::cos(a) };   // not unit length
      assert(near(nose_turn_rate(ahead, yawed, dt), 1.45f, 1e-3f));
      assert(near(nose_turn_rate(ahead, pitched, dt), 1.45f, 1e-3f));
      assert(nose_turn_rate(ahead, ahead, dt) == 0.0f && nose_turn_rate(ahead, yawed, 0.0f) == 0.0f);
      const float zero[3] = { 0.0f, 0.0f, 0.0f };
      const float nan3[3] = { std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f };
      assert(nose_turn_rate(zero, yawed, dt) == 0.0f && nose_turn_rate(nan3, yawed, dt) == 0.0f);
      const float back[3] = { 0.0f, 0.0f, -1.0f };   // half a turn in one frame: pi radians
      assert(near(nose_turn_rate(ahead, back, 1.0f), 3.14159265f, 1e-4f));

      // Hard: 60% of the faster of the class's PitchRate and TurnRate.
      assert(near(hard_turn_rate(1.45f, 1.2f), 0.87f) && near(hard_turn_rate(0.0f, 2.0f), 1.2f));
      assert(near(hard_turn_rate(0.0f, 0.0f), kHardTurnFallback));
      assert(near(hard_turn_rate(std::numeric_limits<float>::quiet_NaN(), 0.0f), kHardTurnFallback));
   }

   // Seconds of hard turning: counts while hard, holds through a lull of up to
   // a quarter of a second (a mouse between pushes), starts again after longer.
   {
      const float dt = 1.0f / 30.0f;
      TurnCount c;
      for (int i = 0; i < 90; ++i) c.update(true, dt);   // three seconds
      assert(near(c.time, 3.0f, 1e-3f));
      for (int i = 0; i < 6; ++i) c.update(false, dt);   // 0.2 s lull: held
      assert(near(c.time, 3.0f, 1e-3f));
      c.update(true, dt);
      assert(near(c.time, 3.0f + dt, 1e-3f) && c.lull == 0.0f);
      for (int i = 0; i < 9; ++i) c.update(false, dt);   // 0.3 s lull: over
      assert(c.time == 0.0f);
      c.update(true, 0.0f);                              // a paused frame changes nothing
      assert(c.time == 0.0f);
      // A mouse pushing two frames in three still adds up.
      TurnCount m;
      for (int i = 0; i < 90; ++i) m.update(i % 3 != 2, dt);
      assert(near(m.time, 2.0f, 0.05f));
      const Threshold& turn = defaults::kTurn.threshold;   // 2 to 3 seconds
      assert(threshold_level(turn, FlyerSpeeds{}, 1.9f) == 0.0f && threshold_level(turn, FlyerSpeeds{}, 3.0f) == 1.0f);
   }

   // Bumps: nothing short of the threshold, 30% at its start growing to full,
   // full past a single value; BF2's own figure stops at 40 m/s.
   {
      const FlyerSpeeds none;
      const Threshold& any = defaults::kCollision.threshold;   // 0 to 40 m/s
      assert(near(bump_scale(any, none, 0.0f), kBumpLeast) && bump_scale(any, none, 40.0f) == 1.0f);
      assert(near(bump_scale(any, none, 20.0f), 0.65f) && bump_scale(any, none, 90.0f) == 1.0f);
      const Threshold hard = between(value_bound(10.0f), value_bound(50.0f));
      assert(bump_scale(hard, none, 9.0f) == 0.0f && near(bump_scale(hard, none, 10.0f), kBumpLeast));
      const Threshold single = { value_bound(15.0f), value_bound(15.0f), false };
      assert(bump_scale(single, none, 14.0f) == 0.0f && bump_scale(single, none, 15.0f) == 1.0f);
      assert(near(stock_bump_speed(0.8f * (20.0f / 40.0f + 0.01f)), 20.0f, 1e-3f));
      assert(near(stock_bump_speed(0.8f), 39.6f, 1e-3f));
      assert(stock_bump_speed(5.0f) == 40.0f && stock_bump_speed(0.0f) == 0.0f);
      assert(stock_bump_speed(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
   }

   // Walkers. Steps: each foot's bit counts once, when it comes on; the roll
   // goes toward the side that landed, left feet even and right feet odd.
   {
      assert(feet_mask(0) == 0 && feet_mask(-1) == 0 && feet_mask(2) == 0x3 && feet_mask(4) == 0xF);
      assert(feet_mask(6) == 0x3F && feet_mask(9) == 0x3F);
      assert(feet_landed(0x1, 0x3) == 0x2 && feet_landed(0x3, 0x3) == 0 && feet_landed(0x2, 0x1) == 0x1);
      assert(step_side(0x1) == -1 && step_side(0x2) == 1 && step_side(0x3) == 0 && step_side(0) == 0);
      assert(step_side(0x4) == -1 && step_side(0x8) == 1 && step_side(0x5) == -1 && step_side(0x30) == 0);
      // A step with no Threshold is full size at any speed; with one it grades
      // like a bump, from 30% at the first end.
      const FlyerSpeeds atst = walker_speeds(5.0f, 10.0f);
      assert(bump_scale(defaults::kStep.threshold, atst, 0.0f) == 1.0f);
      assert(bump_scale(defaults::kStep.threshold, atst, 8.0f) == 1.0f);
      const Threshold faster = between(value_bound(0.0f), speed_bound(SpeedRef::Max));
      assert(near(bump_scale(faster, atst, 0.0f), kBumpLeast) && bump_scale(faster, atst, 5.0f) == 1.0f);
      assert(defaults::kStep.roll.lo > 0.0f && defaults::kStep.pitch.hi < 0.0f);
   }

   // A walker's speeds: MaxSpeed and BoostSpeed its own, no MinSpeed (0) and
   // no cruising MidSpeed (MaxSpeed); a class without a boost never reaches
   // BoostSpeed.
   {
      const FlyerSpeeds atst = walker_speeds(5.0f, 10.0f);
      assert(atst.known && atst.min == 0.0f && atst.mid == 5.0f && atst.max == 5.0f && atst.boost == 10.0f);
      const Threshold& boost = defaults::kBoostWalker.threshold;
      assert(threshold_level(boost, atst, 5.0f) == 0.0f && near(threshold_level(boost, atst, 7.5f), 0.5f));
      assert(threshold_level(boost, atst, 10.0f) == 1.0f && defaults::kBoostWalker.steady == 1.0f);
      const FlyerSpeeds atat = walker_speeds(2.0f, 0.0f);
      assert(threshold_level(boost, atat, 30.0f) == 0.0f);
      assert(!walker_speeds(std::numeric_limits<float>::quiet_NaN(), 1.0f).known);
   }

   // Walker landings: from BF2's own time off the ground, at least half a
   // second, sized by the fastest drop on the way down. Striding never counts.
   {
      WalkerAir a;
      for (int i = 0; i < 60; ++i) assert(a.update(0.0f, 0.0f) < 0.0f);           // striding
      for (int i = 1; i <= 10; ++i) assert(a.update(0.03f * i, 0.0f) < 0.0f);    // brief gaps
      assert(a.update(0.0f, 0.0f) < 0.0f);
      for (int i = 1; i <= 40; ++i) assert(a.update(i / 60.0f, -0.25f * i) < 0.0f);   // a fall
      assert(near(a.update(0.0f, 0.0f), 10.0f));                                  // down at 10 m/s
      assert(a.update(0.0f, 0.0f) < 0.0f);                                        // once
      for (int i = 1; i <= 20; ++i) a.update(i / 60.0f, -1.0f);                   // a third of a second
      assert(a.update(0.0f, 0.0f) < 0.0f);
      assert(a.update(std::numeric_limits<float>::quiet_NaN(), 0.0f) < 0.0f);
      const Threshold& land = defaults::kLandWalker.threshold;   // 2 to 10 m/s
      assert(bump_scale(land, FlyerSpeeds{}, 1.0f) == 0.0f && near(bump_scale(land, FlyerSpeeds{}, 2.0f), kBumpLeast));
      assert(bump_scale(land, FlyerSpeeds{}, 10.0f) == 1.0f);
   }

   // The turbulence: pitch at the rate, yaw at 1.3x and roll at 0.7x, inside the
   // shape's angles, linear in its level, still at level 0.
   {
      const Shape& s = defaults::kBoost;
      assert(turbulence(s, 1.0, 0.0f).negligible());
      const Offset one = turbulence(s, 0.123, 1.0f);
      const Offset two = turbulence(s, 0.123, 2.0f);
      assert(near(two.pitch, 2.0f * one.pitch) && near(two.yaw, 2.0f * one.yaw) && near(two.roll, 2.0f * one.roll));
      int pitchFlips = 0, yawFlips = 0, rollFlips = 0;
      Offset last = turbulence(s, 0.0, 1.0f);
      const int n = 100000;   // ten seconds
      for (int i = 1; i <= n; ++i) {
         const Offset o = turbulence(s, i * 1e-4, 1.0f);
         assert(std::fabs(o.pitch) <= 0.5f * kDegToRad + 1e-6f && std::fabs(o.yaw) <= 0.5f * kDegToRad + 1e-6f);
         assert(std::fabs(o.roll) <= 0.5f * kDegToRad + 1e-6f && o.back == 0.0f && o.right == 0.0f);
         pitchFlips += (o.pitch > 0.0f) != (last.pitch > 0.0f);
         yawFlips += (o.yaw > 0.0f) != (last.yaw > 0.0f);
         rollFlips += (o.roll > 0.0f) != (last.roll > 0.0f);
         last = o;
      }
      // Two sign changes a cycle: 11, 14.3 and 7.7 Hz over ten seconds.
      assert(std::abs(pitchFlips - 220) <= 2 && std::abs(yawFlips - 286) <= 2 && std::abs(rollFlips - 154) <= 2);
   }

   // A held shake's fade: Rise x Length in, the rest out. The defaults keep the
   // fades the shakes had before they took Length and Rise: boost and turn eased
   // at 6 per second each way, the brake in at about 0.08 s and out at 0.25 s,
   // the sprint ramped at 3 per second.
   {
      const Fade boost = held_fade(defaults::kBoost);
      const Fade turn = held_fade(defaults::kTurn);
      const Fade brake = held_fade(defaults::kBrake);
      const Fade sprint = held_fade(defaults::kSprintSoldier);
      assert(near(boost.in, 0.5f) && near(boost.out, 0.5f) && near(turn.in, 0.5f) && near(turn.out, 0.5f));
      assert(near(brake.in, 0.25f) && near(brake.out, 0.75f));
      assert(near(sprint.in, 1.0f / 3.0f) && near(sprint.out, 1.0f / 3.0f));

      // Exactly the old boost and turn ease.
      Hold now, before;
      for (int i = 0; i < 120; ++i) {
         const float target = i < 60 ? 1.0f : 0.0f;
         fade_toward(now, target, 1.0f / 60.0f, boost);
         before.advance(target, 1.0f / 60.0f, 1.0f / 6.0f, 1.0f / 6.0f);
         assert(near(now.level, before.level, 1e-5f));
      }
      // The brake within a hair of its old 0.08 s / 0.25 s.
      Hold b1, b2;
      for (int i = 0; i < 120; ++i) {
         const float target = i < 30 ? 1.0f : 0.0f;
         fade_toward(b1, target, 1.0f / 60.0f, brake);
         b2.advance(target, 1.0f / 60.0f, 0.08f, 0.25f);
         assert(near(b1.level, b2.level, 0.02f));
      }
      // Exactly the old sprint ramp.
      float ramped = 0.0f, old = 0.0f;
      for (int i = 0; i < 90; ++i) {
         const float target = i < 45 ? 1.0f : 0.0f;
         ramped = ramp_toward(ramped, target, 1.0f / 60.0f, sprint);
         old = approach(old, target, 3.0f, 1.0f / 60.0f);
         assert(near(ramped, old, 1e-5f));
      }

      // "Over its time": an exponential fade is 95% there, a ramp all the way.
      Shape slow = defaults::kBoost;
      slow.length = 2.0f;
      slow.rise = 0.25f;                 // half a second in, a second and a half out
      const Fade f = held_fade(slow);
      assert(near(f.in, 0.5f) && near(f.out, 1.5f));
      Hold h;
      for (int i = 0; i < 30; ++i) fade_toward(h, 1.0f, 1.0f / 60.0f, f);
      assert(h.level >= 0.94f && h.level < 1.0f);
      float r = 0.0f;
      for (int i = 0; i < 30; ++i) r = ramp_toward(r, 1.0f, 1.0f / 60.0f, f);
      assert(near(r, 1.0f));
      // No length, or all of it on one side: that side is instant.
      Shape instant = slow;
      instant.length = 0.0f;
      Hold i0;
      fade_toward(i0, 1.0f, 1.0f / 60.0f, held_fade(instant));
      assert(i0.level == 1.0f && ramp_toward(0.0f, 1.0f, 0.016f, held_fade(instant)) == 1.0f);
      Shape upAtOnce = slow;
      upAtOnce.rise = 0.0f;
      assert(held_fade(upAtOnce).in == 0.0f && near(held_fade(upAtOnce).out, 2.0f));
      Shape downAtOnce = slow;
      downAtOnce.rise = 1.0f;
      assert(near(held_fade(downAtOnce).in, 2.0f) && held_fade(downAtOnce).out == 0.0f);
      Shape broken = slow;
      broken.length = std::numeric_limits<float>::quiet_NaN();
      assert(held_fade(broken).in == 0.0f && held_fade(broken).out == 0.0f);
      assert(ramp_toward(0.3f, 1.0f, 0.0f, f) == 0.3f);   // a paused frame changes nothing
   }

   // PushOnce: with a rate, the push still goes out and back once (a one-off)
   // or holds steady (a held shake) while the angles swing; off, it swings with
   // them. Every default has it on but the blast, whose position sways.
   {
      assert(defaults::kFire.pushOnce && defaults::kTakeoff.pushOnce && defaults::kBoost.pushOnce);
      assert(!defaults::kBlast.pushOnce);

      Shape rumble = defaults::kTakeoff;   // rate 8
      rumble.push = { 0.05f, 0.05f };
      Rng r1(9u), r2(9u);
      Kick once = make_kick(rumble, 1.0f, r1);
      Shape swinging = rumble;
      swinging.pushOnce = false;
      Kick swings = make_kick(swinging, 1.0f, r2);
      assert(once.pushOnce && !swings.pushOnce && once.rate == 8.0f);
      bool forward = false;
      for (int i = 0; i < 1000; ++i) {
         once.age = swings.age = i * 0.001f;
         const Offset a = once.value(i * 0.001);
         const Offset b = swings.value(i * 0.001);
         assert(a.back >= 0.0f);   // never pulled forward past rest
         assert(near(a.back, 0.05f * envelope(once.age, once.length, once.rise), 1e-6f));
         forward |= b.back < -1e-4f;
      }
      assert(forward);   // swinging, it does

      Shape boost = defaults::kBoost;
      boost.push = { 0.04f, 0.04f };
      for (int i = 0; i < 200; ++i) {
         const Offset o = turbulence(boost, i * 0.01, 0.5f);
         assert(near(o.back, 0.02f) && o.right == 0.0f && o.up == 0.0f);
         const Offset w = sway(boost, i * 0.01, 0.5f);
         assert(near(w.back, 0.02f) && w.right == 0.0f && w.up == 0.0f);
      }
      boost.pushOnce = false;
      bool varies = false;
      for (int i = 1; i < 200; ++i)
         varies |= !near(turbulence(boost, i * 0.01, 0.5f).back, turbulence(boost, (i - 1) * 0.01, 0.5f).back);
      assert(varies);
   }

   // A bump's kick swings on the turbulence, inside its peak, and fades out.
   {
      Rng rng(3u);
      Kick k = make_kick(defaults::kCollision, 0.5f, rng, true);
      assert(k.turbulent && near(k.peak.pitch, 0.75f * kDegToRad) && k.rise == 0.0f && k.length == 0.3f);
      bool positive = false, negative = false;
      for (int i = 0; i < 300; ++i) {
         k.age = i * 0.001f;
         const Offset v = k.value(i * 0.001);
         assert(std::fabs(v.pitch) <= std::fabs(k.peak.pitch) + 1e-6f);
         positive |= v.pitch > 1e-5f;
         negative |= v.pitch < -1e-5f;
      }
      assert(positive && negative);
      k.age = 0.3f;
      assert(k.value(0.3).negligible());
   }

   // Hits: the first reading only sets the baseline; a drop past the threshold
   // hits at once; drops during the cooldown gather into one hit after it; a
   // slow drain never adds up to one; healing and NaN do nothing.
   {
      HitSense h;
      const float th = hit_threshold(300.0f);
      assert(near(th, 1.5f));
      assert(h.update(300.0f, th, 0.016f) == 0.0f);
      assert(near(h.update(280.0f, th, 0.016f), 20.0f));
      assert(h.update(270.0f, th, 0.016f) == 0.0f);          // in the cooldown: gathered
      assert(h.update(265.0f, th, 0.016f) == 0.0f);
      float hit = 0.0f;
      for (int i = 0; i < 20 && hit == 0.0f; ++i) hit = h.update(265.0f, th, 0.016f);
      assert(hit > 14.0f && hit <= 15.0f);                   // less what leaked meanwhile
      assert(h.update(300.0f, th, 0.016f) == 0.0f);          // healed
      assert(h.update(std::numeric_limits<float>::quiet_NaN(), th, 0.016f) == 0.0f);

      HitSense drain;
      float health = 1000.0f;
      drain.update(health, hit_threshold(1000.0f), 0.1f);
      for (int i = 0; i < 100; ++i) {
         health -= 0.3f;   // 3 a second, like a hero's drain, read every 0.1 s
         assert(drain.update(health, hit_threshold(1000.0f), 0.1f) == 0.0f);
      }

      assert(hit_scale(1.0f, 300.0f) == kHitLeast);
      assert(hit_scale(30.0f, 300.0f) == 1.0f && hit_scale(300.0f, 300.0f) == 1.0f);
      assert(near(hit_scale(15.0f, 300.0f), 0.5f));
      assert(hit_scale(5.0f, 0.0f) == 1.0f);
   }

   // Landings: only after at least kMinAirtime in the air, and only onto the
   // ground; a knockdown in between ends the jump without one.
   {
      AirTime a;
      for (int i = 0; i < 30; ++i) assert(!a.update(true, false, 0.02f));
      assert(a.update(false, true, 0.02f));
      assert(!a.update(false, true, 0.02f));                 // standing: no second landing

      AirTime hop;
      for (int i = 0; i < 5; ++i) hop.update(true, false, 0.02f);
      assert(!hop.update(false, true, 0.02f));               // 0.1 s: too short

      AirTime knocked;
      for (int i = 0; i < 30; ++i) knocked.update(true, false, 0.02f);
      assert(!knocked.update(false, false, 0.02f));          // tumbling
      assert(!knocked.update(false, true, 0.02f));
   }

   // rotate_rows matches RotationX * RotationY * RotationZ times the matrix,
   // keeps the rows orthonormal, and zero angles change nothing.
   for (int n = 0; n < 2000; ++n) {
      float basis[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
      float rx[3][3], ry[3][3], rz[3][3], t1[3][3], t2[3][3];
      rot_x(rnd(-3.0f, 3.0f), rx);
      rot_y(rnd(-3.0f, 3.0f), ry);
      rot_z(rnd(-3.0f, 3.0f), rz);
      mul(rx, ry, t1);
      mul(t1, rz, t2);
      float turned[3][3];
      mul(t2, basis, turned);
      for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) basis[i][j] = turned[i][j];

      Angles a;
      a.pitch = rnd(-0.35f, 0.35f);
      a.yaw   = rnd(-0.35f, 0.35f);
      a.roll  = rnd(-0.35f, 0.35f);

      rot_x(a.pitch, rx);
      rot_y(a.yaw, ry);
      rot_z(a.roll, rz);
      float r[3][3], want[3][3];
      mul(rx, ry, t1);
      mul(t1, rz, r);
      mul(r, basis, want);

      float right[3] = { basis[0][0], basis[0][1], basis[0][2] };
      float up[3]    = { basis[1][0], basis[1][1], basis[1][2] };
      float back[3]  = { basis[2][0], basis[2][1], basis[2][2] };
      rotate_rows(right, up, back, a);
      for (int c = 0; c < 3; ++c) {
         assert(near(right[c], want[0][c]));
         assert(near(up[c], want[1][c]));
         assert(near(back[c], want[2][c]));
      }
      assert(near(dot(right, right), 1.0f) && near(dot(up, up), 1.0f) && near(dot(back, back), 1.0f));
      assert(near(dot(right, up), 0.0f) && near(dot(up, back), 0.0f) && near(dot(right, back), 0.0f));
   }

   // The ODF's signs: pitch up, yaw right and roll clockwise are positive, and a
   // push moves the camera back. The camera looks down -Z of its matrix.
   {
      float m[16];
      Offset o;
      o.pitch = 0.1f;
      identity(m);
      apply(m, o);
      assert(-m[9] > 0.0f);                     // the view direction (-back row) points up
      o = Offset{};
      o.yaw = 0.1f;
      identity(m);
      apply(m, o);
      assert(-m[8] > 0.0f);                     // ... points right
      o = Offset{};
      o.roll = 0.1f;
      identity(m);
      apply(m, o);
      assert(m[1] < 0.0f);                      // the right row dips: a clockwise tilt
      o = Offset{};
      o.back = 0.5f;
      o.right = 0.25f;
      identity(m);
      apply(m, o);
      assert(near(m[14], 0.5f) && near(m[12], 0.25f) && near(m[13], 0.0f));
      identity(m);
      apply(m, Offset{});
      float id[16];
      identity(id);
      for (int i = 0; i < 16; ++i) assert(m[i] == id[i]);
   }

   // No caps: any size passes through untouched; only an axis that is not a
   // number is dropped.
   {
      Offset big;
      big.pitch = 5.0f;
      big.roll = -5.0f;
      big.up = 9.0f;
      big.back = 50.0f;
      const Offset c = finite_offset(big);
      assert(c.pitch == 5.0f && c.roll == -5.0f && c.up == 9.0f && c.back == 50.0f && c.yaw == 0.0f);
      Offset bad;
      bad.yaw = std::numeric_limits<float>::quiet_NaN();
      bad.right = std::numeric_limits<float>::infinity();
      bad.pitch = 0.25f;
      const Offset d = finite_offset(bad);
      assert(d.yaw == 0.0f && d.right == 0.0f && d.pitch == 0.25f);
   }

   // One shot per weapon per frame: a shotgun's eight pellets kick once; the
   // next frame's shot kicks again; another weapon at the same time kicks too.
   {
      int shotgun, pistol;
      ShotGate gate;
      int kicks = 0;
      for (int pellet = 0; pellet < 8; ++pellet) kicks += gate.first(&shotgun, 1.0) ? 1 : 0;
      assert(kicks == 1);
      assert(gate.first(&pistol, 1.0) && !gate.first(&pistol, 1.0) && !gate.first(&shotgun, 1.0));
      assert(gate.first(&shotgun, 1.0 + 1.0 / 60.0));          // a salvo's next shot, a frame on
      ShotGate busy;                                            // more weapons than it keeps
      int w[ShotGate::kWeapons + 2];
      for (int i = 0; i < ShotGate::kWeapons + 2; ++i) assert(busy.first(&w[i], 2.0));
   }

   // Melee: teams from GameObject::mTeam's 4-bit signed field; teammates share
   // a team above 0.
   {
      assert(team_from_bits(0u) == 0 && team_from_bits(1u) == 1 && team_from_bits(7u) == 7);
      assert(team_from_bits(0xFu) == -1 && team_from_bits(0x8u) == -8);
      assert(team_from_bits(0xABCD0012u) == 2);              // only the low four bits
      assert(same_team(1, 1) && same_team(2, 2) && !same_team(1, 2));
      assert(!same_team(0, 0) && !same_team(-1, -1));         // neutral: no one's teammate
   }

   // What a swing struck during an update: an attack's objects past its count
   // before; all of an attack that is new since.
   {
      int a, b, c;
      MeleeTally tally;
      tally.add(&a, 2);
      tally.add(&b, 0);
      assert(tally.before(&a) == 2 && tally.before(&b) == 0 && tally.before(&c) == 0);
      MeleeTally full;
      int many[kMeleeAttacks + 4];
      for (int i = 0; i < kMeleeAttacks + 4; ++i) full.add(&many[i], i);
      assert(full.attacks == kMeleeAttacks && full.before(&many[kMeleeAttacks - 1]) == kMeleeAttacks - 1);
      assert(full.before(&many[kMeleeAttacks]) == 0);         // past the cap: never tallied
   }

   // The melee defaults: opt-in one-off shakes, teammates counted unless an ODF says not.
   {
      const Shape* melee[] = { &defaults::kSwing, &defaults::kStrike, &defaults::kBlock,
                               &defaults::kDeflect, &defaults::kSwingBlocked };
      for (const Shape* s : melee) {
         assert(s->length > 0.0f && s->rate == 0.0f && s->limit == 1.0f);
         assert(s->pushOnce && s->teammates);
      }
      assert(defaults::kStrike.pitch.hi < 0.0f);                // a strike jars the view down
      assert(defaults::kSwingBlocked.pitch.lo > defaults::kBlock.pitch.lo);   // knocked back hardest
      assert(defaults::kFire.teammates && defaults::kHit.teammates);
   }

   // First person keeps the turn and drops the move.
   {
      Offset o;
      o.pitch = 0.1f;
      o.yaw = -0.2f;
      o.roll = 0.3f;
      o.right = 1.0f;
      o.up = -2.0f;
      o.back = 13.0f;
      const Offset t = turn_only(o);
      assert(t.pitch == 0.1f && t.yaw == -0.2f && t.roll == 0.3f);
      assert(t.right == 0.0f && t.up == 0.0f && t.back == 0.0f);
   }

   // Handing BF2 its own camera back: once, to the camera it was left in, and
   // only while the shaken matrix is still there.
   {
      int ours, other;
      float before[16], after[16], m[16];
      identity(before);
      std::memcpy(after, before, sizeof after);
      after[14] = 0.5f;                                   // pushed back half a metre
      LeftShake left;
      assert(!left.put_back(&ours, after));               // nothing left yet
      std::memcpy(m, after, sizeof m);
      left.leave(&ours, before, after);
      assert(!left.put_back(&other, m) && m[14] == 0.5f); // another camera
      assert(left.put_back(&ours, m) && m[14] == 0.0f);   // BF2's own matrix again
      std::memcpy(m, after, sizeof m);
      assert(!left.put_back(&ours, m) && m[14] == 0.5f);  // once only
      left.leave(&ours, before, after);
      m[12] = 3.0f;                                       // set by something else since
      assert(!left.put_back(&ours, m) && m[12] == 3.0f && m[14] == 0.5f);
   }

   // Why: BF2 eases its camera a share of the way toward where it should be
   // each frame, from wherever its matrix was last frame. A push left in that
   // matrix piles up, more the higher the frame rate; handed back, the camera
   // shows the push and nothing more, at any frame rate.
   {
      const auto shown = [](float fps, bool handBack) {
         const float dt = 1.0f / fps;
         const float ease = 1.0f - std::exp(-8.0f * dt);   // a soldier's camera, pulling in at 8
         int camera;
         LeftShake left;
         float m[16];
         identity(m);                                       // where BF2 wants it: the origin
         for (int frame = 0; frame < static_cast<int>(fps); ++frame) {   // a second
            if (handBack) left.put_back(&camera, m);
            for (int c = 12; c < 15; ++c) m[c] -= m[c] * ease;
            float before[16];
            std::memcpy(before, m, sizeof before);
            Offset push;
            push.back = 0.1f;
            apply(m, push);
            left.leave(&camera, before, m);
         }
         return m[14];
      };
      for (float fps : { 30.0f, 60.0f, 144.0f }) assert(near(shown(fps, true), 0.1f, 1e-5f));
      assert(shown(60.0f, false) > 0.75f && shown(144.0f, false) > 1.8f);
   }

   // The defaults carry the spec's numbers.
   assert(defaults::kFire.pitch.lo == 0.25f && defaults::kFire.pitch.hi == 0.4f);
   assert(defaults::kFire.push.hi == 0.06f && defaults::kFire.length == 0.22f && defaults::kFire.rise == 0.25f);
   assert(defaults::kHit.pitch.lo == 2.0f && defaults::kHit.length == 0.25f && defaults::kHit.rise == 0.3f);
   assert(defaults::kLandSoldier.pitch.lo == -2.25f && defaults::kLandSoldier.length == 0.67f);
   assert(defaults::kSprintSoldier.pitch.hi == 0.16f && defaults::kSprintSoldier.yaw.hi == 0.06f);
   assert(defaults::kBlast.pitch.hi == 0.0f && defaults::kBlast.yaw.hi == 0.0f && defaults::kBlast.rate == 3.0f);
   assert(defaults::kFire.limit == 8.0f && defaults::kHit.limit == 1.0f && defaults::kBlast.limit == 2.5f);

   // A soldier's roll is one move over a second: back and looking down, then settling.
   {
      Rng rng(1u);
      Kick k = make_kick(defaults::kRollSoldier, 1.0f, rng);
      assert(k.rate == 0.0f && k.length == 1.0f && k.peak.pitch < 0.0f && k.peak.back > 0.0f);
      assert(k.peak.yaw == 0.0f && k.peak.roll == 0.0f);
      k.age = k.length * k.rise;
      const Offset top = k.value(0.0);
      assert(near(top.pitch, -5.0f * kDegToRad) && near(top.back, 0.7f));
      k.age = 0.999f;
      assert(std::fabs(k.value(0.0).back) < 1e-4f);
   }

   std::puts("Camera shake tests passed (noise, envelope, ODF values, kicks, restarts and limits, "
             "holds, sway, railed sprint judder, blast cap, thresholds, boost, speed-ups the throttle "
             "asked for, hard turns, bumps, walker steps, speeds and landings, "
             "turbulence, hits, landings, 2,000 rotation cases, ODF signs, no caps, one kick per "
             "shotgun blast, melee teams and tallies, first person turns only, BF2's camera handed "
             "back, defaults).");
}
