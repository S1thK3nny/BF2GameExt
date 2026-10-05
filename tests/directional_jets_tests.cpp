// Standalone tests (not a DLL build) for the directional jets: the four
// animations' names, the unit's top jet speeds, the lean a move asks for, how
// the legs' lean follows it, how much of each animation that shows, and that
// the blends laid one after another give each animation exactly its share.
#include "../PatcherDLL/src/entity/directional_jets_core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

using namespace directional_jets;

namespace {

bool near(float a, float b, float eps = 1e-5f)
{
   return std::fabs(a - b) <= eps;
}

void test_names()
{
   assert(std::strcmp(anim_name(Forward), "jetpack_hover_forward") == 0);
   assert(std::strcmp(anim_name(Backward), "jetpack_hover_backward") == 0);
   assert(std::strcmp(anim_name(Left), "jetpack_hover_left") == 0);
   assert(std::strcmp(anim_name(Right), "jetpack_hover_right") == 0);
}

void test_top_speeds()
{
   // EntitySoldierClass's defaults for "jet": thrust 0.3, strafe 0.3.
   TopSpeeds t = top_speeds(7.0f, 5.0f, 0.3f, 0.3f);
   assert(near(t.forward, 2.1f) && near(t.backward, 1.5f) && near(t.side, 1.5f));
   t = top_speeds(7.0f, 5.0f, 1.5f, 1.25f);
   assert(near(t.forward, 10.5f) && near(t.backward, 7.5f) && near(t.side, 6.25f));
   // No push that way in a hover: the run speed stands in.
   t = top_speeds(7.0f, 5.0f, 0.0f, 0.0f);
   assert(near(t.forward, 7.0f) && near(t.backward, 5.0f) && near(t.side, 5.0f));
   t = top_speeds(7.0f, 5.0f, 0.3f, -1.0f);
   assert(near(t.forward, 2.1f) && near(t.side, 5.0f));
   t = top_speeds(0.0f, 0.0f, 0.0f, 0.0f);
   assert(t.forward == 0.0f && t.backward == 0.0f && t.side == 0.0f);
}

void test_lean()
{
   const TopSpeeds top = top_speeds(7.0f, 5.0f, 0.3f, 0.3f);   // 2.1, 1.5, 1.5
   Lean l = lean_of(2.1f, 0.0f, top);
   assert(near(l.f, 1.0f) && near(l.s, 0.0f));
   l = lean_of(1.05f, 0.0f, top);
   assert(near(l.f, 0.5f));
   l = lean_of(20.0f, 0.0f, top);   // a jet jump past the hover's top speed
   assert(near(l.f, 1.0f) && near(l.s, 0.0f));
   l = lean_of(-0.75f, 0.0f, top);   // backward goes over the backward top speed
   assert(near(l.f, -0.5f));
   l = lean_of(0.0f, 1.5f, top);    // + along the right row is moving left
   assert(near(l.s, 1.0f));
   l = lean_of(0.0f, -0.75f, top);
   assert(near(l.s, -0.5f));
   // Both at their top: kept on the unit circle, the way between the two.
   l = lean_of(2.1f, 1.5f, top);
   assert(near(l.f, std::sqrt(0.5f)) && near(l.s, std::sqrt(0.5f)));
   // A unit that cannot move at all leans nowhere; nonsense leans nowhere.
   l = lean_of(3.0f, 3.0f, top_speeds(0.0f, 0.0f, 0.0f, 0.0f));
   assert(l.f == 0.0f && l.s == 0.0f);
   l = lean_of(NAN, 1.0f, top);
   assert(l.f == 0.0f && l.s == 0.0f);
   l = lean_of(INFINITY, 0.0f, top);
   assert(l.f == 0.0f && l.s == 0.0f);
   // Never outside the unit disc.
   std::mt19937 rng(1234);
   std::uniform_real_distribution<float> speed(-30.0f, 30.0f);
   for (int i = 0; i < 2000; ++i) {
      l = lean_of(speed(rng), speed(rng), top);
      assert(l.f * l.f + l.s * l.s <= 1.0f + 1e-4f);
   }
}

void test_follow()
{
   const Lean target{ 1.0f, -0.5f };
   Lean l = follow(Lean{}, target, 0.0f);   // paused: no change
   assert(l.f == 0.0f && l.s == 0.0f);
   l = follow(Lean{}, target, NAN);
   assert(l.f == 0.0f && l.s == 0.0f);
   l = follow(Lean{}, target, 1.0f);        // a long frame lands on it
   assert(near(l.f, 1.0f) && near(l.s, -0.5f));
   // UpdateActionAnimation's leg rate: min(dt x 7.5, 1) of the way a frame.
   l = follow(Lean{}, target, 1.0f / 60.0f);
   assert(near(l.f, 0.125f) && near(l.s, -0.0625f));
   // At 60 fps it is past 95% of the way in 0.4 s, and never past it.
   l = Lean{};
   for (int frame = 0; frame < 24; ++frame) {
      l = follow(l, target, 1.0f / 60.0f);
      assert(l.f <= 1.0f && l.s >= -0.5f);
   }
   assert(l.f > 0.95f && l.s < -0.475f);
}

// A body at its top speed straight ahead turns half a turn in one frame: its
// lean swings through the middle, never more than a frame's share at a time.
void test_swing()
{
   const TopSpeeds top = top_speeds(7.0f, 5.0f, 1.0f, 1.0f);
   Lean shown{};
   for (int frame = 0; frame < 120; ++frame) shown = follow(shown, lean_of(7.0f, 0.0f, top), 1.0f / 60.0f);
   assert(near(shown.f, 1.0f, 1e-3f));
   float last = shown.f;
   bool crossed = false;
   for (int frame = 0; frame < 120; ++frame) {
      shown = follow(shown, lean_of(-5.0f, 0.0f, top), 1.0f / 60.0f);
      assert(last - shown.f <= 2.0f * 0.125f + 1e-5f);
      crossed |= std::fabs(shown.f) < 0.15f;
      last = shown.f;
   }
   assert(crossed && near(shown.f, -1.0f, 1e-3f));
}

void test_weights()
{
   Weights w = weights_of(Lean{});
   for (float v : w.w) assert(v == 0.0f);
   w = weights_of(Lean{ 1.0f, 0.0f });
   assert(near(w.w[Forward], 1.0f) && w.w[Backward] == 0.0f && w.w[Left] == 0.0f && w.w[Right] == 0.0f);
   w = weights_of(Lean{ -0.5f, 0.0f });
   assert(near(w.w[Backward], 0.5f) && w.w[Forward] == 0.0f);
   w = weights_of(Lean{ 0.0f, 0.5f });
   assert(near(w.w[Left], 0.5f) && w.w[Right] == 0.0f);
   w = weights_of(Lean{ 0.0f, -1.0f });
   assert(near(w.w[Right], 1.0f) && w.w[Left] == 0.0f);
   // Diagonally at the top speed: half each of the two ways.
   w = weights_of(Lean{ std::sqrt(0.5f), std::sqrt(0.5f) });
   assert(near(w.w[Forward], 0.5f) && near(w.w[Left], 0.5f));
   // Halfway there, diagonally: a quarter each, the hover the other half.
   w = weights_of(Lean{ 0.5f * std::sqrt(0.5f), -0.5f * std::sqrt(0.5f) });
   assert(near(w.w[Forward], 0.25f) && near(w.w[Right], 0.25f));
   // Too little to show is nothing.
   w = weights_of(Lean{ 0.001f, 0.0f });
   assert(w.w[Forward] == 0.0f);
   // The total is the lean's length, never more than 1.
   std::mt19937 rng(99);
   std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
   for (int i = 0; i < 2000; ++i) {
      Lean l{ unit(rng), unit(rng) };
      const float m = std::sqrt(l.f * l.f + l.s * l.s);
      if (m > 1.0f) {
         l.f /= m;
         l.s /= m;
      }
      w = weights_of(l);
      float sum = 0.0f;
      for (float v : w.w) {
         assert(v >= 0.0f);
         sum += v;
      }
      assert(sum <= 1.0f + 1e-5f);
      if (std::sqrt(l.f * l.f + l.s * l.s) > kMinWeight) assert(near(sum, std::fmin(1.0f, m), 1e-4f));
   }
}

// The blends laid one after another, each moving the pose `t` of the way to
// its animation, done on one-hot vectors (the hover, then the four ways):
// what each ends up holding.
void mixture(const Weights& w, const bool have[kDirs], float out[1 + kDirs])
{
   Step steps[kDirs];
   const int n = blend_steps(w, have, steps);
   for (int i = 0; i <= kDirs; ++i) out[i] = i == 0 ? 1.0f : 0.0f;
   for (int i = 0; i < n; ++i) {
      assert(steps[i].t >= 0.0f && steps[i].t <= 1.0f);
      for (int k = 0; k <= kDirs; ++k) out[k] *= 1.0f - steps[i].t;
      out[1 + steps[i].dir] += steps[i].t;
   }
}

void test_steps()
{
   const bool all[kDirs] = { true, true, true, true };
   float mix[1 + kDirs];

   Weights w = weights_of(Lean{ std::sqrt(0.5f), std::sqrt(0.5f) });
   Step steps[kDirs];
   int n = blend_steps(w, all, steps);
   assert(n == 2 && steps[0].dir == Forward && near(steps[0].t, 1.0f) && steps[1].dir == Left &&
          near(steps[1].t, 0.5f));

   n = blend_steps(weights_of(Lean{}), all, steps);
   assert(n == 0);

   // A way without its animation keeps the hover for its share.
   const bool noLeft[kDirs] = { true, true, false, true };
   w = weights_of(Lean{ 0.3f, 0.4f });   // 0.5 long: forward 0.214, left 0.286
   mixture(w, noLeft, mix);
   assert(near(mix[1 + Forward], w.w[Forward]) && mix[1 + Left] == 0.0f &&
          near(mix[0], 1.0f - w.w[Forward]));

   // Every lean, every set of animations: each holds exactly its weight.
   std::mt19937 rng(7);
   std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
   for (int i = 0; i < 4000; ++i) {
      Lean l{ unit(rng), unit(rng) };
      const float m = std::sqrt(l.f * l.f + l.s * l.s);
      if (m > 1.0f) {
         l.f /= m;
         l.s /= m;
      }
      bool have[kDirs];
      for (bool& h : have) h = (rng() & 1) != 0;
      w = weights_of(l);
      mixture(w, have, mix);
      float rest = 1.0f;
      for (int d = 0; d < kDirs; ++d) {
         const float want = have[d] && w.w[d] > kMinWeight ? w.w[d] : 0.0f;
         assert(near(mix[1 + d], want, 1e-4f));
         rest -= want;
      }
      assert(near(mix[0], rest, 1e-4f));
   }
}

} // namespace

int main()
{
   test_names();
   test_top_speeds();
   test_lean();
   test_follow();
   test_swing();
   test_weights();
   test_steps();
   std::printf("directional_jets_tests: all passed\n");
   return 0;
}
