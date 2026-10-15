// Standalone tests (not a DLL build) for flyer-to-flyer avoidance: which
// flyers are threats, which way and how hard a flyer steers away, and its speed
// behind a slower one - the logic in flyer_avoidance_core.hpp, fed what
// flyer_avoidance.cpp reads. Small kinematic flyers (a turn rate, an
// acceleration) meet head on, cross, and overtake, steered as the game's
// CalculateDest stand-in steers them.
#include "../PatcherDLL/src/ai/flyer_avoidance_core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace flyer_avoidance;

namespace {

const float kNan = std::numeric_limits<float>::quiet_NaN();

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

const V3 kFwd{0, 0, 1}, kRight{1, 0, 0};

// Threats: within the two radii plus the margin inside the horizon, nothing else.
void threats()
{
   Threat t;
   // Dead ahead, closing head on at 140 m/s from 200 m: a threat, out to the right.
   assert(assess({0, 0, 0}, {0, 0, 70}, 6.0f, kFwd, kRight, {0, 0, 200}, {0, 0, -70}, 6.0f, t));
   assert(near(t.away.x, 1.0f) && near(t.pass, 0.0f) && t.urgency > 0.0f && t.brake == 0.0f);
   // The same, 600 m off: past the horizon (closest in 4.3 s at 140 m/s).
   assert(!assess({0, 0, 0}, {0, 0, 70}, 6.0f, kFwd, kRight, {0, 0, 600}, {0, 0, -70}, 6.0f, t));
   // Passing 30 m to the side: clear of 6 + 6 + kMargin.
   assert(!assess({0, 0, 0}, {0, 0, 70}, 6.0f, kFwd, kRight, {30, 0, 200}, {0, 0, -70}, 6.0f, t));
   // Passing 10 m to its left: away is to the right, square to its heading.
   assert(assess({0, 0, 0}, {0, 0, 70}, 6.0f, kFwd, kRight, {-10, 0, 200}, {0, 0, -70}, 6.0f, t));
   assert(t.away.x > 0.99f && near(t.away.z, 0.0f) && near(t.pass, 10.0f, 1e-3f));
   // ... and 10 m above it: away is down.
   assert(assess({0, 0, 0}, {0, 0, 70}, 6.0f, kFwd, kRight, {0, 10, 200}, {0, 0, -70}, 6.0f, t));
   assert(t.away.y < -0.99f);
   // Sooner is more urgent than later, nearer than further.
   Threat soon, late, wide;
   assess({0, 0, 0}, {0, 0, 70}, 6.0f, kFwd, kRight, {-5, 0, 50}, {0, 0, -70}, 6.0f, soon);
   assess({0, 0, 0}, {0, 0, 70}, 6.0f, kFwd, kRight, {-5, 0, 250}, {0, 0, -70}, 6.0f, late);
   assess({0, 0, 0}, {0, 0, 70}, 6.0f, kFwd, kRight, {-15, 0, 50}, {0, 0, -70}, 6.0f, wide);
   assert(soon.urgency > late.urgency && soon.urgency > wide.urgency);
   // Side by side and parallel (a formation): 15 m apart is inside 6 + 6 + kMargin, so they
   // ease apart; 25 m apart they are left alone.
   assert(assess({0, 0, 0}, {0, 0, 70}, 6.0f, kFwd, kRight, {15, 0, 0}, {0, 0, 70}, 6.0f, t));
   assert(near(t.pass, 15.0f) && t.away.x < -0.99f);
   assert(!assess({0, 0, 0}, {0, 0, 70}, 6.0f, kFwd, kRight, {25, 0, 0}, {0, 0, 70}, 6.0f, t));
   // Its way out is square to its heading, even when the pass is still ahead of it.
   assert(assess({0, 0, 0}, {0, 0, 90}, 6.0f, kFwd, kRight, {5, 0, 100}, {0, 0, 45}, 6.0f, t));
   assert(near(t.pass, std::sqrt(125.0f), 1e-3f) && near(dot(t.away, kFwd), 0.0f) && t.away.x < -0.99f);
   // A slower one just behind going its way: step aside, but no braking (it is falling back).
   assert(assess({0, 0, 0}, {0, 0, 90}, 6.0f, kFwd, kRight, {0, 0, -10}, {0, 0, 60}, 6.0f, t));
   assert(t.brake == 0.0f);
   // No heading to go by: no threat.
   assert(!assess({0, 0, 0}, {0, 0, 70}, 6.0f, {kNan, 0, 1}, kRight, {0, 0, 200}, {0, 0, -70}, 6.0f, t));
   // On its line of flight: right with the other ahead, left with it behind.
   assert(assess({0, 0, 0}, {0, 0, 90}, 6.0f, kFwd, kRight, {0, 0, 40}, {0, 0, 60}, 6.0f, t) && t.away.x > 0.99f);
   assert(assess({0, 0, 0}, {0, 0, 60}, 6.0f, kFwd, kRight, {0, 0, -40}, {0, 0, 90}, 6.0f, t) && t.away.x < -0.99f);
   // Already moving apart: only while still inside the limit.
   assert(!assess({0, 0, 0}, {0, 0, 70}, 6.0f, kFwd, kRight, {0, 0, -50}, {0, 0, -70}, 6.0f, t));
   // A slower one ahead going its way: ease off as well as stepping aside.
   assert(assess({0, 0, 0}, {0, 0, 90}, 6.0f, kFwd, kRight, {2, 0, 60}, {0, 0, 60}, 6.0f, t));
   assert(t.brake > 0.0f && t.brake <= kBrake && t.away.x < -0.99f);
   // A faster one closing from behind: step aside, no braking.
   assert(assess({0, 0, 0}, {0, 0, 60}, 6.0f, kFwd, kRight, {2, 0, -60}, {0, 0, 90}, 6.0f, t));
   assert(t.brake == 0.0f);
   // Single file (a wingman on the trail): one ahead going its way is kept
   // behind, slowed by the threat's urgency, not passed round; one behind it or
   // meeting it is stepped round as before.
   assert(assess({0, 0, 0}, {0, 0, 60}, 6.0f, kFwd, kRight, {2, 0, 18}, {0, 0, 60}, 6.0f, t) && t.aheadSameWay);
   const float urgent = t.urgency;
   single_file(t);
   assert(t.away.x == 0.0f && t.away.y == 0.0f && t.away.z == 0.0f && urgent > 0.0f && near(t.brake, kBrake * urgent));
   assert(assess({0, 0, 0}, {0, 0, 60}, 6.0f, kFwd, kRight, {2, 0, -18}, {0, 0, 60}, 6.0f, t) && !t.aheadSameWay);
   single_file(t);
   assert(t.away.x < -0.99f);   // behind it: unchanged
   assert(assess({0, 0, 0}, {0, 0, 70}, 6.0f, kFwd, kRight, {-10, 0, 200}, {0, 0, -70}, 6.0f, t) && !t.aheadSameWay);
   single_file(t);
   assert(t.away.x > 0.99f);    // meeting it: stepped round
   Threat hard;
   hard.aheadSameWay = true;
   hard.urgency = 0.1f;
   hard.brake = 0.3f;
   single_file(hard);
   assert(near(hard.brake, 0.3f));   // a harder brake already: kept
   // Radii are kept within bounds; bad input is no threat.
   assert(near(clamp_radius(100.0f), kMaxRadius) && near(clamp_radius(0.0f), kMinRadius) && near(clamp_radius(kNan), kMinRadius));
   assert(!assess({0, 0, 0}, {0, 0, 70}, 6.0f, kFwd, kRight, {kNan, 0, 50}, {0, 0, -70}, 6.0f, t));
   assert(!assess({0, 0, 0}, {kNan, 0, 70}, 6.0f, kFwd, kRight, {0, 0, 50}, {0, 0, -70}, 6.0f, t));
}

// The steering point bent away, the speed eased.
void bending()
{
   Avoid none;
   const V3 me{0, 100, 0}, point{0, 100, 500};
   assert(near(length(bend(me, point, kFwd, none, true) - point), 0.0f));   // no threats: untouched
   Avoid a;
   Threat t;
   t.urgency = 1.0f;
   t.away = {1, 0, 0};
   add(a, t);
   const V3 b = bend(me, point, kFwd, a, true);
   V3 dir = b - me;
   assert(near(length(dir), 500.0f, 1e-2f));                                // as far out as the point was
   normalise(dir);
   assert(dir.x > 0.7f && dir.z > 0.5f);                                    // bent hard right, still ahead
   // A near point is pushed out to kMinAhead.
   assert(near(length(bend(me, {0, 100, 10}, kFwd, a, true) - me), kMinAhead, 1e-2f));
   // Lifted over terrain: bent to the side, never lower.
   Avoid down;
   t.away = {0, -1, 0};
   add(down, t);
   V3 kept = bend(me, {0, 130, 400}, kFwd, down, false) - me;
   normalise(kept);
   V3 was = V3{0, 30, 400};
   normalise(was);
   assert(kept.y >= was.y - 1e-4f);
   V3 free = bend(me, {0, 130, 400}, kFwd, down, true) - me;
   normalise(free);
   assert(free.y < was.y);
   // Many threats add up to at most kMaxPush.
   Avoid many;
   for (int i = 0; i < 10; ++i) add(many, t);
   assert(many.threats == 10 && length(many.push) > kMaxPush);
   V3 capped = bend(me, point, kFwd, many, true) - me;
   normalise(capped);
   const float most = 1.0f / std::sqrt(1.0f + kMaxPush * kGain * kMaxPush * kGain);   // the bend it is held to
   assert(near(capped.z, most, 1e-3f));
   // Each threat pushes as hard as it is urgent; the nearest pass is kept.
   Avoid weighted;
   Threat light;
   light.urgency = 0.25f;
   light.away = {1, 0, 0};
   light.pass = 14.0f;
   add(weighted, light);
   t.pass = 9.0f;
   add(weighted, t);
   assert(near(length(V3{weighted.push.x, 0, 0}), 0.25f) && near(weighted.pass, 9.0f) && near(weighted.urgency, 1.0f));
   // Braking: a share of the target, at most kBrake.
   Avoid slow;
   Threat s;
   s.urgency = 1.0f;
   s.away = {1, 0, 0};
   s.brake = kBrake;
   add(slow, s);
   assert(near(braked(0.8f, slow), 0.8f * (1.0f - kBrake)) && near(braked(0.8f, a), 0.8f));
   assert(std::isnan(braked(kNan, slow)));
   // Stats: the nearest pass and the worst urgency.
   Stats st;
   st.add(none);
   st.add(slow);
   st.add(weighted);
   assert(st.bent == 2 && st.braked == 1 && near(st.worst, 1.0f) && near(st.nearest, 0.0f));
   Stats far;
   far.add(weighted);
   assert(near(far.nearest, 9.0f));
}

// ---- Flyers meeting -----------------------------------------------------------------
struct Flyer {
   V3    pos, heading;
   float speed;
   float cruise;   // the speed it wants
   V3    goal;     // where it is going
};

// Two flyers fly at their goals for `seconds`, turning at most kTurn and
// changing speed at most kAccel, each steered as the stand-in steers: its goal,
// bent away from the other when it is a threat. The nearest they came.
float meet(Flyer a, Flyer b, bool avoid, float seconds)
{
   constexpr float kDt = 1.0f / 60.0f, kTurn = 1.5708f, kAccel = 30.0f, kRadius = 6.0f;
   float nearest = 1e9f;
   for (float time = 0.0f; time < seconds; time += kDt) {
      Flyer* f[2] = {&a, &b};
      V3 aim[2];
      float want[2];
      for (int i = 0; i < 2; ++i) {
         Flyer& me = *f[i];
         const Flyer& other = *f[1 - i];
         aim[i] = me.goal;
         want[i] = me.cruise;
         if (avoid) {
            const V3 right{me.heading.z, 0.0f, -me.heading.x};
            Threat t;
            Avoid av;
            if (assess(me.pos, me.heading * me.speed, kRadius, me.heading, right, other.pos,
                       other.heading * other.speed, kRadius, t))
               add(av, t);
            aim[i] = bend(me.pos, me.goal, me.heading, av, true);
            want[i] = me.cruise * (1.0f - av.brake);
         }
      }
      for (int i = 0; i < 2; ++i) {
         Flyer& me = *f[i];
         V3 to = aim[i] - me.pos;
         if (normalise(to)) {
            const float c = clampf(dot(me.heading, to), -1.0f, 1.0f);
            const float angle = std::acos(c);
            if (angle > 1e-5f) {
               const float step = angle < kTurn * kDt ? angle : kTurn * kDt;
               // Turn toward `to` by `step` in the plane the two span.
               V3 side = to - me.heading * c;
               if (normalise(side)) {
                  me.heading = me.heading * std::cos(step) + side * std::sin(step);
                  normalise(me.heading);
               }
            }
         }
         me.speed += clampf(want[i] - me.speed, -kAccel * kDt, kAccel * kDt);
         me.pos = me.pos + me.heading * (me.speed * kDt);
      }
      const float d = length(a.pos - b.pos);
      if (d < nearest) nearest = d;
   }
   return nearest;
}

void meeting()
{
   const float clear = 2.0f * 6.0f;   // the spheres touching
   // Head on, each flying at the other's start.
   Flyer a{{0, 100, 0}, {0, 0, 1}, 70, 70, {0, 100, 2000}};
   Flyer b{{0, 100, 1000}, {0, 0, -1}, 70, 70, {0, 100, -1000}};
   const float headOnBlind = meet(a, b, false, 15.0f), headOn = meet(a, b, true, 15.0f);
   assert(headOnBlind < 3.0f && headOn > clear);   // blind: through each other (a frame is 2.3 m of closing)
   // Nearly head on, 5 m apart: they widen it rather than meeting.
   Flyer c = b;
   c.pos.x = 5.0f;
   c.goal.x = 5.0f;
   const float offsetBlind = meet(a, c, false, 15.0f), offset = meet(a, c, true, 15.0f);
   assert(offsetBlind < clear && offset > clear);
   // Crossing at right angles, timed to meet in the middle.
   Flyer d{{-700, 100, 700}, {1, 0, 0}, 70, 70, {2000, 100, 700}};
   const float crossBlind = meet(a, d, false, 20.0f), cross = meet(a, d, true, 20.0f);
   assert(crossBlind < 3.0f && cross > clear);
   // Overtaking a slower flyer on the same line.
   Flyer fast{{0, 100, 0}, {0, 0, 1}, 95, 95, {0, 100, 5000}};
   Flyer slow{{0, 100, 300}, {0, 0, 1}, 60, 60, {0, 100, 5000}};
   const float overtakeBlind = meet(fast, slow, false, 20.0f), overtake = meet(fast, slow, true, 20.0f);
   assert(overtakeBlind < 3.0f && overtake > clear);
   std::printf("  nearest passes, blind / avoiding: head on %.1f / %.1f m, 5 m offset %.1f / %.1f m, crossing "
               "%.1f / %.1f m, overtaking %.1f / %.1f m\n",
               (double)headOnBlind, (double)headOn, (double)offsetBlind, (double)offset, (double)crossBlind,
               (double)cross, (double)overtakeBlind, (double)overtake);
}

} // namespace

int main()
{
   threats();
   bending();
   meeting();
   std::printf("Flyer avoidance tests passed (threats, bending, meeting).\n");
   return 0;
}
