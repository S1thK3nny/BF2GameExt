// Standalone tests (not a DLL build). Compile with /std:c++17 /W4 /WX, no NDEBUG.
#include "../PatcherDLL/src/render/camera_shake_core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
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
         assert(k.peak.roll == 0.0f && near(k.peak.back, 0.03f) && k.peak.right == 0.0f && k.peak.up == 0.0f);
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
      Kick k = make_kick(defaults::kRollSoldier, 1.0f, rng);
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

   // The limit: repeats faster than a shake rises settle at about `limit`
   // shakes' worth and never pass it; slower repeats do not pile up at all.
   {
      assert(carry_for(1.0f) == 0.0f && carry_for(0.5f) == 0.0f);
      assert(near(carry_for(2.0f), 0.5f) && near(carry_for(4.0f), 0.75f));
      for (float limit : { 1.0f, 2.0f, 3.0f }) {
         Kick k;
         k.peak.pitch = 0.01f;
         k.length = 0.6f;
         k.rise = 0.25f;
         KickChannel ch;
         double t = 0.0;
         float highest = 0.0f;
         for (int shot = 0; shot < 200; ++shot) {   // a shot every 0.05 s; the rise is 0.15 s
            ch.trigger(k, t, limit);
            for (int f = 0; f < 5; ++f) {
               ch.advance(0.01f);
               t += 0.01;
               highest = std::fmax(highest, ch.value(t).pitch);
            }
         }
         assert(highest <= 0.01f * limit * 1.001f);
         assert(highest >= 0.01f * limit * 0.95f);
      }
      Kick k;
      k.peak.pitch = 0.01f;
      k.length = 0.22f;
      k.rise = 0.25f;
      KickChannel slowFire;
      double t = 0.0;
      float highest = 0.0f;
      for (int shot = 0; shot < 50; ++shot) {       // every 0.3 s: each has faded first
         slowFire.trigger(k, t, 3.0f);
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
      assert(sway(defaults::kSprintFlyer, 1.0, 0.0f).negligible());
      const Offset one = sway(defaults::kSprintFlyer, 0.77, 1.0f);
      const Offset two = sway(defaults::kSprintFlyer, 0.77, 2.0f);
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
         assert(std::fabs(o.right) <= 0.03f + 1e-6f && std::fabs(o.back) <= 0.03f + 1e-6f);
      }
      assert(blast(defaults::kBlast, 1.0, 0.0f).negligible());
   }

   // Brake intensity: nothing below the floor, full from kBrakeFull, linear between.
   assert(brake_intensity(0.0f) == 0.0f);
   assert(brake_intensity(kBrakeFloor) == 0.0f);
   assert(brake_intensity(kBrakeFull) == 1.0f);
   assert(near(brake_intensity((kBrakeFloor + kBrakeFull) * 0.5f), 0.5f));

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

   // clamp_offset holds every axis to its limit.
   {
      Offset big;
      big.pitch = 5.0f;
      big.roll = -5.0f;
      big.up = 9.0f;
      const Offset c = clamp_offset(big);
      assert(c.pitch == kMaxAngle && c.roll == -kMaxAngle && c.up == kMaxMove && c.yaw == 0.0f);
   }

   // The defaults carry the spec's numbers.
   assert(defaults::kFire.pitch.lo == 0.25f && defaults::kFire.pitch.hi == 0.4f);
   assert(defaults::kFire.push.hi == 0.015f && defaults::kFire.length == 0.22f && defaults::kFire.rise == 0.25f);
   assert(defaults::kHit.pitch.lo == 2.0f && defaults::kHit.length == 0.25f && defaults::kHit.rise == 0.3f);
   assert(defaults::kLandSoldier.pitch.lo == -2.25f && defaults::kLandSoldier.length == 0.67f);
   assert(defaults::kSprintSoldier.pitch.hi == 0.16f && defaults::kSprintSoldier.yaw.hi == 0.06f);
   assert(defaults::kBlast.pitch.hi == 0.0f && defaults::kBlast.yaw.hi == 0.0f && defaults::kBlast.rate == 3.0f);
   assert(defaults::kFire.limit == 1.0f && defaults::kHit.limit == 1.0f && defaults::kBlast.limit == 2.5f);

   std::puts("Camera shake tests passed (noise, envelope, ODF values, kicks, restarts and limits, "
             "holds, sway, railed sprint judder, blast cap, hits, landings, 2,000 rotation cases, "
             "ODF signs, clamps, defaults).");
}
