// Standalone tests (not a DLL build) for real flight on paths: how far ahead
// of an AI flyer its carrot runs, how much of each update the carrot is
// stepped, and the speed the flyer is asked for - the logic in
// flyer_real_flight_core.hpp, fed what flyer_real_flight.cpp reads. A small
// kinematic flyer (a turn rate, an acceleration) chases a carrot round a
// straight path, a wide turn and a hairpin.
#include "../PatcherDLL/src/ai/flyer_real_flight_core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace flyer_real_flight;
using flyer_wingmen::length;
using flyer_wingmen::normalise;

namespace {

const float kNan = std::numeric_limits<float>::quiet_NaN();
const float kInf = std::numeric_limits<float>::infinity();

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// About a second of the flyer's speed, within a turning circle and kMaxLead.
void leads()
{
   assert(near(lead_for(70.0f), 70.0f));
   assert(near(lead_for(10.0f), kMinLead) && near(lead_for(0.0f), kMinLead) && near(lead_for(-5.0f), kMinLead));
   assert(near(lead_for(500.0f), kMaxLead));
   assert(near(lead_for(kNan), kMinLead) && near(lead_for(kInf), kMinLead));
}

// All of the update at the lead, twice as much on top of the flyer, none from
// twice the lead out.
void pacing()
{
   assert(near(pace(70.0f, 70.0f), 1.0f));
   assert(near(pace(0.0f, 70.0f), kMaxPace));
   assert(near(pace(35.0f, 70.0f), 1.5f));
   assert(near(pace(140.0f, 70.0f), 0.0f) && near(pace(500.0f, 70.0f), 0.0f));
   assert(near(pace(kNan, 70.0f), 1.0f) && near(pace(10.0f, 0.0f), 1.0f) && near(pace(10.0f, kNan), 1.0f));
   float last = kMaxPace;
   for (float d = 0.0f; d <= 200.0f; d += 5.0f) {
      const float p = pace(d, 70.0f);
      assert(p <= last && p >= 0.0f);
      last = p;
   }
}

// Lost past four leads, never nearer than kMinLost; NaN is lost.
void losing()
{
   assert(!lost(299.0f, 40.0f) && lost(301.0f, 40.0f));
   assert(!lost(599.0f, 150.0f) && lost(601.0f, 150.0f));
   assert(lost(kNan, 70.0f) && lost(kInf, 70.0f));
   assert(!lost(0.0f, 70.0f));
}

// The path's speed from how far the carrot went over the part of the update
// it was stepped, smoothed.
void carrot_speeds()
{
   assert(near(carrot_speed(0.0f, 10.0f, 0.1f, 0.1f), 100.0f));     // the first, whole
   assert(near(carrot_speed(100.0f, 5.0f, 0.1f, 0.1f), 80.0f));     // 50 now, 0.4 of the way
   assert(near(carrot_speed(100.0f, 5.0f, 0.1f, 1.0f), 50.0f));     // a long update: all the way
   assert(near(carrot_speed(100.0f, 10.0f, 0.05f, 0.1f), 140.0f));  // stepped half the update: 200 m/s now
   assert(near(carrot_speed(70.0f, 10.0f, 0.0f, 0.1f), 70.0f));     // not stepped: as it was
   assert(near(carrot_speed(70.0f, kNan, 0.1f, 0.1f), 70.0f));
   assert(near(carrot_speed(kNan, 7.0f, 0.1f, 0.1f), 70.0f));
}

// The path's speed, plus kCatchUp a metre past the lead, within the flyer's.
void flyer_speeds()
{
   assert(near(flyer_speed(70.0f, 70.0f, 70.0f, 50.0f, 100.0f), 70.0f));
   assert(near(flyer_speed(70.0f, 90.0f, 70.0f, 50.0f, 100.0f), 80.0f));
   assert(near(flyer_speed(70.0f, 30.0f, 70.0f, 50.0f, 100.0f), 50.0f));
   assert(near(flyer_speed(70.0f, 10.0f, 70.0f, 50.0f, 100.0f), 50.0f));    // its slowest
   assert(near(flyer_speed(70.0f, 200.0f, 70.0f, 50.0f, 100.0f), 100.0f));  // its fastest
   assert(near(flyer_speed(70.0f, 10.0f, 70.0f, 120.0f, 100.0f), 40.0f));   // a slowest above the fastest: none
   assert(near(flyer_speed(kNan, 70.0f, 70.0f, 50.0f, 100.0f), 100.0f));    // no carrot speed yet: its fastest
   assert(near(flyer_speed(70.0f, kNan, 70.0f, 50.0f, 100.0f), 70.0f));
   assert(near(flyer_speed(70.0f, 70.0f, 70.0f, 50.0f, 0.0f), 0.0f));
   assert(near(flyer_speed(70.0f, 70.0f, 70.0f, 50.0f, kNan), 0.0f));     // no top speed to read
}

void switches()
{
   bool on = false;
   assert(read_switch("1", on) && on);
   assert(read_switch(" \"0\" ", on) && !on);
   assert(read_switch("2", on) && on);
   on = true;
   assert(!read_switch("yes", on) && on && !read_switch("", on) && !read_switch(nullptr, on) && on);
}

void stats()
{
   Stats s;
   s.add(60.0f, 70.0f, 68.0f, 70.0f);
   s.add(90.0f, 70.0f, 72.0f, 70.0f);
   s.add(kNan, 70.0f, 70.0f, 70.0f);
   assert(s.samples == 2 && near((float)s.distance, 150.0f) && near(s.worst, 90.0f) && near((float)s.speed, 140.0f));
}

// ---- A carrot chased round three paths ------------------------------------------------
// A path by arc length u, flat (y 0), starting at the origin heading +z.
using Path = V3 (*)(float u);

V3 straight(float u) { return {0.0f, 0.0f, u}; }

constexpr float kWide = 150.0f;   // a wide turn's radius, m
V3 wide_turn(float u) { return {kWide - kWide * std::cos(u / kWide), 0.0f, kWide * std::sin(u / kWide)}; }

// 300 m on, a 10 m hairpin, then back: the authored worst (docs/RE/FlyerAI.md).
constexpr float kLeg = 300.0f, kPin = 10.0f, kPi = 3.14159265f;
V3 hairpin(float u)
{
   if (u <= kLeg) return {0.0f, 0.0f, u};
   const float a = (u - kLeg) / kPin;
   if (a <= kPi) return {kPin - kPin * std::cos(a), 0.0f, kLeg + kPin * std::sin(a)};
   return {2.0f * kPin, 0.0f, kLeg - (u - kLeg - kPi * kPin)};
}

struct Run {
   float worstLost = 0.0f;   // the largest distance over its lost limit seen (0: never lost)
   float settledLo = 1e9f, settledHi = 0.0f;   // distance over the lead, after settling
   V3    pos, heading;
   float u = 0.0f;
};

// A flyer that turns at most kTurn and changes speed at most kAccel chases the
// carrot as flyer_real_flight.cpp steps it: lead, pace, the carrot's speed and
// the speed it is asked for, every 1/60 s for `seconds`.
Run chase(Path path, float pathSpeed, float seconds, float settleAfter)
{
   constexpr float kDt = 1.0f / 60.0f, kTurn = 1.5708f, kAccel = 30.0f, kMin = 40.0f, kMax = 100.0f;
   Run r;
   r.pos = path(0.0f);
   r.heading = {0.0f, 0.0f, 1.0f};
   float speed = pathSpeed, carrotSpeed = pathSpeed;
   for (float t = 0.0f; t < seconds; t += kDt) {
      // The carrot, as step() moves it.
      const V3 carrot = path(r.u);
      const float lead = lead_for(speed > carrotSpeed ? speed : carrotSpeed);
      const float d = length(carrot - r.pos);
      if (lost(d, lead)) r.worstLost = d > r.worstLost ? d : r.worstLost;
      if (t > settleAfter) {
         r.settledLo = d / lead < r.settledLo ? d / lead : r.settledLo;
         r.settledHi = d / lead > r.settledHi ? d / lead : r.settledHi;
      }
      const float stepped = kDt * pace(d, lead);
      r.u += pathSpeed * stepped;
      carrotSpeed = carrot_speed(carrotSpeed, length(path(r.u) - carrot), stepped, kDt);
      // The flyer: toward the carrot at its turn rate, toward the asked speed at its acceleration.
      V3 want = carrot - r.pos;
      if (normalise(want)) {
         const float c = flyer_wingmen::clampf(flyer_wingmen::dot(r.heading, want), -1.0f, 1.0f);
         const float angle = std::acos(c);
         const float turn = angle < kTurn * kDt ? angle : kTurn * kDt;
         if (angle > 1e-5f) {
            const float side = r.heading.z * want.x - r.heading.x * want.z > 0.0f ? 1.0f : -1.0f;   // turn which way, flat
            const float s = std::sin(turn) * side, k = std::cos(turn);
            r.heading = {r.heading.x * k + r.heading.z * s, 0.0f, r.heading.z * k - r.heading.x * s};
            normalise(r.heading);
         }
      }
      const float asked = flyer_speed(carrotSpeed, d, lead, kMin, kMax);
      speed += flyer_wingmen::clampf(asked - speed, -kAccel * kDt, kAccel * kDt);
      r.pos = r.pos + r.heading * (speed * kDt);
   }
   return r;
}

void chasing()
{
   // A straight path: the carrot settles at the lead.
   const Run a = chase(straight, 70.0f, 20.0f, 6.0f);
   assert(a.worstLost == 0.0f);
   assert(a.settledLo > 0.95f && a.settledHi < 1.05f);
   assert(std::fabs(a.pos.x) < 1.0f);

   // A wide turn: it cuts inside, the carrot near the lead, the flyer near the path.
   const Run b = chase(wide_turn, 70.0f, 30.0f, 6.0f);
   assert(b.worstLost == 0.0f);
   assert(b.settledLo > 0.6f && b.settledHi < 1.4f);
   const V3 centre = {kWide, 0.0f, 0.0f};
   assert(std::fabs(length(b.pos - centre) - kWide) < 20.0f);

   // A hairpin it cannot turn on: it swings wide but never loses the carrot, and is
   // back on the far leg, heading down it, within 15 s.
   const Run c = chase(hairpin, 70.0f, 20.0f, 1e9f);
   assert(c.worstLost == 0.0f);
   assert(std::fabs(c.pos.x - 2.0f * kPin) < 25.0f && c.heading.z < -0.9f);
   assert(c.u > kLeg + kPi * kPin + 200.0f);   // the carrot got on with it

   std::printf("  straight: carrot %.2f-%.2f leads ahead; wide turn: %.2f-%.2f leads, %.1f m inside the path; "
               "hairpin: %.1f m off the far leg after 20 s\n",
               (double)a.settledLo, (double)a.settledHi, (double)b.settledLo, (double)b.settledHi,
               (double)(kWide - length(b.pos - centre)), (double)std::fabs(c.pos.x - 2.0f * kPin));
}

// ---- Pursuit ---------------------------------------------------------------------------
// Where it steers: the target led by up to kPursuitLeadMax of its flight.
void pursuit_points()
{
   const V3 me{0, 0, 0}, flank{60, 0, 0};
   assert(near(pursuit_point(me, 80.0f, {0, 0, 40}, flank).x, 30.0f));                      // 0.5 s out
   assert(near(pursuit_point(me, 80.0f, {0, 0, 400}, flank).x, 60.0f * kPursuitLeadMax));   // capped
   assert(near(pursuit_point(me, 5.0f, {0, 0, 20}, flank).x, 60.0f));                       // closing at least kPursuitMinSpeed
   const V3 still = pursuit_point(me, 80.0f, {0, 0, 400}, {kNan, 0, 0});                    // no velocity: the target
   assert(near(still.x, 0.0f) && near(still.z, 400.0f));
   assert(near(pursuit_point(me, kNan, {0, 0, 20}, flank).x, 60.0f));
}

// Its speed: flat out far off or off its nose; the target's, give or take, close behind it.
void pursuit_speeds()
{
   assert(near(pursuit_speed(kPursuitGap, 1.0f, 70.0f, 50.0f, 100.0f), 70.0f));
   assert(near(pursuit_speed(kPursuitGap + 20.0f, 1.0f, 70.0f, 50.0f, 100.0f), 80.0f));
   assert(near(pursuit_speed(kPursuitGap - 20.0f, 1.0f, 70.0f, 50.0f, 100.0f), 60.0f));
   assert(near(pursuit_speed(10.0f, 1.0f, 70.0f, 50.0f, 100.0f), 50.0f));         // its slowest
   assert(near(pursuit_speed(800.0f, 1.0f, 70.0f, 50.0f, 100.0f), 100.0f));       // closing
   assert(near(pursuit_speed(kPursuitGap, 0.5f, 70.0f, 50.0f, 100.0f), 100.0f));  // turning onto it
   assert(near(pursuit_speed(kNan, 1.0f, 70.0f, 50.0f, 100.0f), 100.0f) && near(pursuit_speed(100.0f, 1.0f, 70.0f, 50.0f, 0.0f), 0.0f));
   assert(near(pursuit_speed(100.0f, 1.0f, 70.0f, 50.0f, kNan), 0.0f));   // no top speed to read
   // A wingman's own gap (from its weapon): at its speed there, faster further back.
   assert(near(pursuit_speed(60.0f, 1.0f, 70.0f, 50.0f, 100.0f, 60.0f), 70.0f));
   assert(near(pursuit_speed(80.0f, 1.0f, 70.0f, 50.0f, 100.0f, 60.0f), 80.0f));
   assert(near(pursuit_speed(kPursuitGap, 1.0f, 70.0f, 50.0f, 100.0f, kNan), 70.0f));   // no gap: the default
   assert(near(pursuit_speed(kPursuitGap, 1.0f, 70.0f, 50.0f, 100.0f, -5.0f), 70.0f));
}

// The chase's clock: kPursuitSeconds from each provocation, at most kPursuitMaxSeconds.
void pursuit_clock()
{
   Pursuit p;
   assert(pursuit_over(p, 0.0));
   Pursuit off;
   off.until = 1e9;
   assert(pursuit_over(off, 0.0));   // not begun: over, whatever its clock says
   provoke(p, 100.0);
   assert(p.on && !pursuit_over(p, 114.9) && pursuit_over(p, 115.0));
   provoke(p, 110.0);
   assert(!pursuit_over(p, 124.9) && pursuit_over(p, 125.0));
   provoke(p, 135.0);
   assert(std::fabs(p.until - (100.0 + kPursuitMaxSeconds)) < 1e-9 && pursuit_over(p, 140.0));
}

// A target flying a steady circle (20 degrees a second at 70 m/s) chased by a
// flyer that turns 90 degrees a second: it comes round onto the target's tail
// and stays near the gap, the target ahead of its nose.
void pursuing()
{
   constexpr float kDt = 1.0f / 60.0f, kTurn = 1.5708f, kAccel = 30.0f, kMin = 40.0f, kMax = 100.0f;
   constexpr float kCircle = 0.349f, kTargetSpeed = 70.0f, kRadius = kTargetSpeed / kCircle;
   V3 me{0, 0, -600}, heading{1, 0, 0};   // 600 m off, heading across its circle
   float speed = 70.0f;
   float settledLo = 1e9f, settledHi = 0.0f, worstCos = 1.0f;
   for (float t = 0.0f; t < 40.0f; t += kDt) {
      const float a = t * kCircle;
      const V3 target{kRadius * std::sin(a), 0.0f, kRadius - kRadius * std::cos(a)};
      const V3 targetVel{kTargetSpeed * std::cos(a), 0.0f, kTargetSpeed * std::sin(a)};
      const V3 point = pursuit_point(me, speed, target, targetVel);
      V3 to = target - me;
      const float d = length(to);
      normalise(to);
      const float ahead = flyer_wingmen::dot(heading, to);
      const float asked = pursuit_speed(d, ahead, kTargetSpeed, kMin, kMax);
      if (t > 20.0f) {
         settledLo = d < settledLo ? d : settledLo;
         settledHi = d > settledHi ? d : settledHi;
         worstCos = ahead < worstCos ? ahead : worstCos;
      }
      V3 want = point - me;
      if (normalise(want)) {
         const float c = flyer_wingmen::clampf(flyer_wingmen::dot(heading, want), -1.0f, 1.0f);
         const float angle = std::acos(c);
         const float turn = angle < kTurn * kDt ? angle : kTurn * kDt;
         if (angle > 1e-5f) {
            const float side = heading.z * want.x - heading.x * want.z > 0.0f ? 1.0f : -1.0f;
            const float s = std::sin(turn) * side, k = std::cos(turn);
            heading = {heading.x * k + heading.z * s, 0.0f, heading.z * k - heading.x * s};
            normalise(heading);
         }
      }
      speed += flyer_wingmen::clampf(asked - speed, -kAccel * kDt, kAccel * kDt);
      me = me + heading * (speed * kDt);
   }
   assert(settledLo > 40.0f && settledHi < 200.0f && worstCos > 0.8f);
   std::printf("  pursuit of a circling target: %.0f-%.0f m behind it after 20 s, never more than %.0f degrees off "
               "its nose\n",
               (double)settledLo, (double)settledHi, (double)(std::acos(worstCos) * 57.29578f));
}

} // namespace

int main()
{
   leads();
   pacing();
   losing();
   carrot_speeds();
   flyer_speeds();
   switches();
   stats();
   chasing();
   pursuit_points();
   pursuit_speeds();
   pursuit_clock();
   pursuing();
   std::printf("Flyer real flight tests passed (leads, pacing, losing, carrot speeds, flyer speeds, switches, stats, "
               "chasing, pursuit points, pursuit speeds, the pursuit clock, pursuing).\n");
   return 0;
}
