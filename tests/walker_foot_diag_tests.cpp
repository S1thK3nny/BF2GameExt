// Standalone tests (not a DLL build) for the walker foot diagnostic's step
// tracking, against a model of DoFootImpactEffects' two stomp tests, and for
// the walker stomp fix's type 1 test.
#include "../PatcherDLL/src/entity/walker_foot_diag_core.hpp"
#include "../PatcherDLL/src/entity/walker_stomp_fix.hpp"
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace walker_foot_diag;

namespace {

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

FootUpdate at(float before, float after, float seconds = 1.0f / 60.0f, bool counted = false)
{
   FootUpdate u;
   u.before = before;
   u.after = after;
   u.seconds = seconds;
   u.counted = counted;
   return u;
}

// DoFootImpactEffects' test for one foot, as decompiled from the Phantom PDB
// build (0x00595470) and read on modtools and retail: what it sets in
// mFootState and mMinFootHeight for the foot's new height. `drop` is what type
// 1 compares with: BF2's 0.1, or the stomp fix's value for the update.
struct Bf2Foot {
   float last     = 0.0f;
   float lowest   = FLT_MAX;
   bool  disarmed = false;   // type 1: bit 24 + i

   void update(float now, int type, float threshold, float drop, bool& counted, bool& rearmed)
   {
      counted = rearmed = false;
      if (type == 0) {
         const float line = lowest + threshold;
         counted = now < last && now < line && line < last;
      } else if (!disarmed) {
         if (last - now < drop) disarmed = counted = true;
      } else if (last - now > drop) {
         disarmed = false;
         rearmed = true;
      }
      if (now < lowest) lowest = now;
      last = now;
   }
};

// A foot's height over a second of walking: lifted to 0.6 over 0.4 s, brought
// down at `down` m/s, planted for the rest.
float gait(double t, double down)
{
   const double p = t - std::floor(t);
   if (p < 0.4) return static_cast<float>(1.5 * p);
   const double h = 0.6 - down * (p - 0.4);
   return h > 0.0 ? static_cast<float>(h) : 0.0f;
}

struct Run {
   std::vector<Step> steps;
   int strays = 0;
};

// Five seconds of the gait at `rate` updates a second, through the model and
// the tracker together, with BF2's type 1 test or the stomp fix's.
Run walk(int rate, int type, float threshold, double down = 4.0, bool fixed = false)
{
   const float dt = 1.0f / static_cast<float>(rate);
   Bf2Foot bf2;
   bf2.last = gait(0.0, down);
   bf2.lowest = bf2.last;
   FootTracker tracker;
   Run run;
   float drop = 0.1f;
   for (int n = 1; n <= 5 * rate; ++n) {
      if (fixed) drop = walker_stomp_drop(dt, drop);
      FootUpdate u;
      u.before = bf2.last;
      u.lowest = bf2.lowest;
      u.after = gait(n * static_cast<double>(dt), down);
      u.seconds = dt;
      bf2.update(u.after, type, threshold, drop, u.counted, u.rearmed);
      switch (tracker.update(u, threshold)) {
      case Event::kStep:       run.steps.push_back(tracker.step()); break;
      case Event::kStrayCount: ++run.strays;                        break;
      case Event::kNone:                                            break;
      }
   }
   return run;
}

} // namespace

int main()
{
   // One clean step: ten updates of 0.05 down, then two standing still end it.
   {
      FootTracker t;
      float h = 1.0f;
      for (int i = 0; i < 10; ++i, h -= 0.05f) assert(t.update(at(h, h - 0.05f), 0.15f) == Event::kNone);
      assert(t.descending());
      assert(t.update(at(h, h), 0.15f) == Event::kNone);
      assert(t.update(at(h, h), 0.15f) == Event::kStep);
      const Step& s = t.step();
      assert(near(s.top, 1.0f) && near(s.bottom, 0.5f) && near(s.drop(), 0.5f));
      assert(s.updates == 10 && near(s.seconds, 10.0f / 60.0f) && near(s.biggest, 0.05f));
      assert(!s.counted && !s.rearmed && !t.descending());
   }

   // The biggest drop keeps its own update's length.
   {
      FootTracker t;
      t.update(at(1.0f, 0.9f, 0.010f), 0.0f);
      t.update(at(0.9f, 0.7f, 0.025f), 0.0f);
      t.update(at(0.7f, 0.6f, 0.010f), 0.0f);
      t.update(at(0.6f, 0.6f), 0.0f);
      assert(t.update(at(0.6f, 0.6f), 0.0f) == Event::kStep);
      assert(near(t.step().biggest, 0.2f) && near(t.step().biggestSeconds, 0.025f));
      assert(near(t.step().seconds, 0.045f));
   }

   // A landing counted in the first update standing still, where type 1
   // counts it, belongs to the step.
   {
      FootTracker t;
      t.update(at(1.0f, 0.8f), 0.0f);
      FootUpdate u = at(0.8f, 0.8f, 1.0f / 60.0f, true);
      assert(t.update(u, 0.0f) == Event::kNone);
      assert(t.update(at(0.8f, 0.8f), 0.0f) == Event::kStep && t.step().counted);
   }

   // A one-update pause on the way down is still one step.
   {
      FootTracker t;
      t.update(at(1.0f, 0.8f), 0.0f);
      assert(t.update(at(0.8f, 0.8f), 0.0f) == Event::kNone);
      t.update(at(0.8f, 0.5f), 0.0f);
      t.update(at(0.5f, 0.5f), 0.0f);
      assert(t.update(at(0.5f, 0.5f), 0.0f) == Event::kStep);
      assert(near(t.step().top, 1.0f) && near(t.step().bottom, 0.5f) && t.step().updates == 2);
   }

   // Rising, or moving no more than kMoving, starts nothing.
   {
      FootTracker t;
      assert(t.update(at(0.5f, 0.6f), 0.0f) == Event::kNone && !t.descending());
      assert(t.update(at(0.6f, 0.6f - kMoving * 0.5f), 0.0f) == Event::kNone && !t.descending());
   }

   // The body bobbing a planted foot is not a step, unless BF2 counted it.
   {
      FootTracker t;
      t.update(at(0.5f, 0.49f), 0.0f);
      t.update(at(0.49f, 0.49f), 0.0f);
      assert(t.update(at(0.49f, 0.49f), 0.0f) == Event::kNone);
      t.update(at(0.49f, 0.48f, 1.0f / 60.0f, true), 0.0f);
      t.update(at(0.48f, 0.48f), 0.0f);
      assert(t.update(at(0.48f, 0.48f), 0.0f) == Event::kStep && t.step().counted);
   }

   // A count while the foot is not coming down is reported on its own.
   {
      FootTracker t;
      assert(t.update(at(0.3f, 0.3f, 1.0f / 60.0f, true), 0.0f) == Event::kStrayCount);
   }

   // Type 0's line is the foot's lowest plus StompThreshold as the step began.
   {
      FootTracker t;
      FootUpdate u = at(1.0f, 0.9f);
      u.lowest = 0.25f;
      t.update(u, 0.15f);
      t.update(at(0.9f, 0.9f), 0.15f);
      t.update(at(0.9f, 0.9f), 0.15f);
      assert(near(t.step().line, 0.4f));
   }

   // The same gait under BF2's own tests. Brought down at 4 m/s, a foot drops
   // 0.067 an update at 60 updates a second: type 1 never re-arms it after the
   // landing it starts armed for, so no step counts. At 30 a second it drops
   // 0.133 an update, and every step counts. Type 0 counts every step at both.
   {
      const Run fast = walk(60, 1, 0.15f);
      assert(fast.steps.size() == 5 && fast.strays == 1);
      for (const Step& s : fast.steps) {
         assert(near(s.drop(), 0.6f, 0.01f) && near(s.biggest, 4.0f / 60.0f, 0.002f));
         assert(!s.counted && !s.rearmed);
      }
      const Run slow = walk(30, 1, 0.15f);
      assert(slow.steps.size() == 5 && slow.strays == 1);
      for (const Step& s : slow.steps) {
         assert(near(s.biggest, 4.0f / 30.0f, 0.004f));
         assert(s.counted && s.rearmed);
      }
      for (int rate : { 30, 60, 144 }) {
         const Run run = walk(rate, 0, 0.15f);
         assert(run.steps.size() == 5 && run.strays == 0);
         for (const Step& s : run.steps) assert(s.counted && near(s.line, 0.15f));
      }
   }

   // The stomp fix's test: 3 m/s times the update's length, BF2's 0.1 at 30
   // updates a second. A paused or broken update keeps the last value.
   {
      assert(near(walker_stomp_drop(1.0f / 30.0f, 0.5f), 0.1f, 1e-6f));
      assert(near(walker_stomp_drop(1.0f / 60.0f, 0.5f), 0.05f, 1e-6f));
      assert(walker_stomp_drop(0.0f, 0.07f) == 0.07f);
      assert(walker_stomp_drop(-0.01f, 0.07f) == 0.07f);
      assert(walker_stomp_drop(std::nanf(""), 0.07f) == 0.07f);
      assert(walker_stomp_drop(INFINITY, 0.07f) == 0.07f);
   }

   // With the fix, the 4 m/s gait lands every step at any rate, as BF2 itself
   // does at 30. A foot brought down at 2.5 m/s, slower than the test, lands at
   // none: the same as BF2 at 30, where it drops 0.083 an update.
   {
      for (int rate : { 30, 60, 144 }) {
         const Run run = walk(rate, 1, 0.15f, 4.0, true);
         assert(run.steps.size() == 5 && run.strays == 1);
         for (const Step& s : run.steps) assert(s.counted && s.rearmed);
      }
      for (int rate : { 30, 60, 144 }) {
         const Run run = walk(rate, 1, 0.15f, 2.5, true);
         assert(run.steps.size() == 5);
         for (const Step& s : run.steps) assert(!s.counted && !s.rearmed);
      }
      const Run stock = walk(30, 1, 0.15f, 2.5);
      assert(stock.steps.size() == 5);
      for (const Step& s : stock.steps) assert(!s.counted);
   }

   std::puts("Walker foot diagnostic tests passed (steps, biggest drop and its update, landings counted "
             "as the foot stops, one-update pauses, body bob, stray counts, type 0's line, one gait "
             "under BF2's two stomp tests at 30, 60 and 144 updates a second, and the stomp fix's "
             "3 m/s test at each).");
}
