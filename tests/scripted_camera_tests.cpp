// Standalone test of the scripted camera's maths (render/scripted_camera_core.hpp):
// the engine's look-at frame, rotations and blends, object facing, the FOV
// widening the game uses, the follow trail and the path curve. No game needed.
#include "render/scripted_camera_core.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>

using namespace scripted_camera;

namespace {

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
bool near(V3 a, V3 b, float eps = 1e-4f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }

bool orthonormal(const View& v)
{
   return near(length(v.right), 1.0f, 1e-4f) && near(length(v.up), 1.0f, 1e-4f) &&
          near(length(v.back), 1.0f, 1e-4f) && near(dot(v.right, v.up), 0.0f, 1e-4f) &&
          near(dot(v.right, v.back), 0.0f, 1e-4f) && near(dot(v.up, v.back), 0.0f, 1e-4f) &&
          near(cross(v.right, v.up), v.back, 1e-4f);   // right x up = back, as the engine builds it
}

float angle_between(const View& a, const View& b)
{
   // The rotation from a to b, from the quaternions.
   const Quat qa = to_quat(a), qb = to_quat(b);
   float d = std::fabs(qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w);
   if (d > 1.0f) d = 1.0f;
   return 2.0f * std::acos(d);
}

V3 random_dir(std::mt19937& rng)
{
   std::uniform_real_distribution<float> u(-1.0f, 1.0f);
   for (;;) {
      V3 d{ u(rng), u(rng), u(rng) };
      const float l = length(d);
      if (l > 0.1f && l <= 1.0f) return d * (1.0f / l);
   }
}

// A frame with roll: look_along, then turned about its view axis.
View rolled(V3 pos, V3 dir, float roll)
{
   View v = look_along(pos, dir);
   const float c = std::cos(roll), s = std::sin(roll);
   const V3 r = v.right * c + v.up * s;
   const V3 u = v.up * c - v.right * s;
   v.right = r;
   v.up = u;
   return v;
}

void test_look_at()
{
   // The free camera's construction: a camera at +Z looking at the origin.
   const View v = look_at({ 0.0f, 0.0f, 10.0f }, {});
   assert(near(v.back, { 0.0f, 0.0f, 1.0f }));
   assert(near(v.right, { 1.0f, 0.0f, 0.0f }));
   assert(near(v.up, { 0.0f, 1.0f, 0.0f }));
   assert(orthonormal(v));

   // It looks along minus back at whatever it was aimed at, level (no roll).
   std::mt19937 rng(7);
   for (int i = 0; i < 1000; ++i) {
      const V3 pos = random_dir(rng) * 50.0f;
      const V3 target = random_dir(rng) * 20.0f;
      const View w = look_at(pos, target);
      assert(orthonormal(w));
      assert(near(-w.back, normalize(target - pos, {}), 1e-4f));
      assert(near(w.right.y, 0.0f, 1e-4f));
      assert(w.up.y >= -1e-4f);
   }

   // Straight down: no level right exists, the hint is kept.
   const View down = look_at({ 0.0f, 10.0f, 0.0f }, {}, { 0.0f, 0.0f, 1.0f });
   assert(orthonormal(down));
   assert(near(down.back, { 0.0f, 1.0f, 0.0f }));
   assert(near(down.right, { 0.0f, 0.0f, 1.0f }));
}

void test_rotations()
{
   std::mt19937 rng(11);
   std::uniform_real_distribution<float> roll(-3.1f, 3.1f);
   for (int i = 0; i < 2000; ++i) {
      const View a = rolled(random_dir(rng) * 30.0f, random_dir(rng), roll(rng));
      View b;
      to_rows(to_quat(a), b);
      assert(near(a.right, b.right, 2e-4f) && near(a.up, b.up, 2e-4f) && near(a.back, b.back, 2e-4f));
   }

   // A blend's ends are its views, and its middle is half way in position and
   // in turn, for any pair.
   for (int i = 0; i < 2000; ++i) {
      const View a = rolled(random_dir(rng) * 30.0f, random_dir(rng), roll(rng));
      const View b = rolled(random_dir(rng) * 30.0f, random_dir(rng), roll(rng));
      const View s = blend(a, b, 0.0f), e = blend(a, b, 1.0f);
      assert(near(s.pos, a.pos) && near(s.back, a.back) && near(e.pos, b.pos) && near(e.back, b.back));
      const View m = blend(a, b, 0.5f);
      assert(orthonormal(m));
      assert(near(m.pos, (a.pos + b.pos) * 0.5f, 1e-3f));
      const float whole = angle_between(a, b);
      assert(near(angle_between(a, m), whole * 0.5f, 2e-3f));
      assert(near(angle_between(m, b), whole * 0.5f, 2e-3f));
   }

   // Matrices round trip, w column and all.
   const View v = rolled({ 1.0f, 2.0f, 3.0f }, { 0.3f, -0.2f, -1.0f }, 0.4f);
   float m[16];
   to_matrix(v, m);
   assert(m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f && m[15] == 1.0f);
   const View back = from_matrix(m);
   assert(near(back.pos, v.pos) && near(back.right, v.right) && near(back.back, v.back));
}

void test_facing()
{
   // An object with the identity frame faces +Z: the view looks along +Z with
   // its up, and is a proper camera frame (right x up = back).
   float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1 };
   View v = facing_view(m);
   assert(orthonormal(v));
   assert(near(-v.back, { 0.0f, 0.0f, 1.0f }));
   assert(near(v.up, { 0.0f, 1.0f, 0.0f }));
   assert(near(v.pos, { 5.0f, 6.0f, 7.0f }));

   // Roll is kept: an object rolled 30 degrees gives a view rolled the same.
   const float c = std::cos(0.5236f), s = std::sin(0.5236f);
   float r[16] = { c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
   v = facing_view(r);
   assert(orthonormal(v));
   assert(near(v.up, { -s, c, 0.0f }));
   assert(near(-v.back, { 0.0f, 0.0f, 1.0f }));

   // A point in an object's frame: {right, up, forward} from its position.
   float o[16] = { 0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 10, 0, 0, 1 };   // facing +X
   assert(near(from_object(o, { 0.0f, 2.0f, 5.0f }), { 15.0f, 2.0f, 0.0f }));
   assert(near(from_object(o, { 3.0f, 0.0f, 0.0f }), { 10.0f, 0.0f, -3.0f }));
}

void test_fov()
{
   // The game's 48 degrees on 16:9 (aspect 9/16) is 61.4 degrees across, the
   // vertical held at 36.9.
   const float w = widened_fov(48.0f, 9.0f / 16.0f);
   assert(near(degrees(w), 61.4f, 0.1f));
   assert(near(degrees(widened_fov(48.0f, 0.75f)), 48.0f, 1e-3f));   // 4:3 is unchanged
   // 21:9 widens further.
   assert(degrees(widened_fov(48.0f, 9.0f / 21.0f)) > 75.0f);
   // The inverse gives the game's measure back.
   for (float f = 20.0f; f <= 100.0f; f += 7.0f)
      for (float a : { 0.75f, 0.625f, 0.5625f, 0.4286f })
         assert(near(game_fov(widened_fov(f, a), a), f, 1e-2f));
}

void test_timing()
{
   assert(ease(Ease::InOut, 0.0f) == 0.0f && ease(Ease::InOut, 1.0f) == 1.0f);
   assert(near(ease(Ease::InOut, 0.5f), 0.5f));
   for (Ease e : { Ease::InOut, Ease::In, Ease::Out, Ease::Linear }) {
      float prev = -1.0f;
      for (int i = 0; i <= 100; ++i) {
         const float v = ease(e, i / 100.0f);
         assert(v >= prev);
         prev = v;
      }
      assert(ease(e, -1.0f) == 0.0f && ease(e, 2.0f) == 1.0f);
   }
   assert(blend_weight(0.0f, 0.0f) == 1.0f);           // a cut
   assert(blend_weight(0.25f, 0.5f) == 0.5f);

   // The trail covers the same ground in a second at any frame rate.
   for (float lag : { 0.1f, 0.3f, 1.0f }) {
      V3 p30{}, p144{};
      const V3 goal{ 10.0f, 0.0f, 0.0f };
      for (int i = 0; i < 30; ++i) p30 = trail(p30, goal, 1.0f / 30.0f, lag);
      for (int i = 0; i < 144; ++i) p144 = trail(p144, goal, 1.0f / 144.0f, lag);
      assert(near(p30.x, p144.x, 1e-3f));
      assert(near(p30.x, 10.0f * (1.0f - std::exp(-1.0f / lag)), 1e-3f));
   }
   assert(near(trail({}, { 1.0f, 2.0f, 3.0f }, 0.016f, 0.0f), { 1.0f, 2.0f, 3.0f }));   // no lag
}

void test_curve()
{
   // Two nodes: a straight line, flown at an even speed.
   Curve line;
   assert(line.build({ { 0, 0, 0 }, { 0, 0, 100 } }, false));
   assert(near(line.total(), 100.0f, 0.05f));
   for (float d = 0.0f; d <= 100.0f; d += 12.5f) assert(near(line.at(d), { 0.0f, 0.0f, d }, 0.05f));

   // It passes through every node, in order, and its ends are the path's.
   const std::vector<V3> nodes = { { 0, 10, 0 }, { 40, 12, 5 }, { 60, 20, 60 }, { 20, 15, 90 }, { -30, 10, 80 } };
   Curve c;
   assert(c.build(nodes, false));
   assert(near(c.at(0.0f), nodes.front(), 1e-3f));
   assert(near(c.at(c.total()), nodes.back(), 1e-3f));
   size_t seen = 0;
   for (float d = 0.0f; d <= c.total(); d += 0.05f)
      if (seen < nodes.size() && length(c.at(d) - nodes[seen]) < 0.2f) ++seen;
   assert(seen == nodes.size());

   // Even speed: equal steps along it are equal steps in space.
   const float step = c.total() / 200.0f;
   for (int i = 0; i < 200; ++i) {
      const float gap = length(c.at((i + 1) * step) - c.at(i * step));
      assert(near(gap, step, step * 0.02f));
   }

   // Which segment and how far through it.
   int seg = -1;
   float through = -1.0f;
   c.at(0.0f, &seg, &through);
   assert(seg == 0 && near(through, 0.0f));
   c.at(c.total(), &seg, &through);
   assert(seg == 3 && near(through, 1.0f));

   // Close nodes do not make it overshoot into a loop (centripetal).
   Curve tight;
   assert(tight.build({ { 0, 0, 0 }, { 100, 0, 0 }, { 100.5f, 0, 0.5f }, { 101, 0, 100 } }, false));
   for (float d = 0.0f; d <= tight.total(); d += 0.25f) {
      const V3 p = tight.at(d);
      assert(p.x < 102.0f && p.z > -1.5f);
   }

   // Closed: it wraps and the start is its end.
   Curve ring;
   std::vector<V3> square = { { 0, 0, 0 }, { 50, 0, 0 }, { 50, 0, 50 }, { 0, 0, 50 } };
   assert(ring.build(square, true) && ring.closed());
   assert(near(ring.at(ring.total()), square[0], 1e-2f));
   assert(near(ring.at(ring.total() + 10.0f), ring.at(10.0f), 1e-3f));
   assert(near(ring.at(-10.0f), ring.at(ring.total() - 10.0f), 1e-3f));

   // Looking ahead runs on past the end along the last stretch.
   const V3 end = c.at(c.total());
   const V3 past = ahead_on(c, c.total(), 5.0f);
   assert(near(length(past - end), 5.0f, 1e-2f));
   assert(dot(past - end, nodes.back() - nodes[nodes.size() - 2]) > 0.0f);

   // Fewer than two nodes is no curve.
   Curve none;
   assert(!none.build({ { 1, 2, 3 } }, false));
   assert(near(none.at(5.0f), { 1.0f, 2.0f, 3.0f }));
}

} // namespace

int main()
{
   test_look_at();
   test_rotations();
   test_facing();
   test_fov();
   test_timing();
   test_curve();
   std::printf("Scripted camera tests passed (look-at frames, 2000 rotation round trips and blends, object "
               "facing with roll, FOV widening 48 -> 61.4 at 16:9 and back, eases, the trail at 30 and 144 "
               "updates a second, the path curve through every node at an even speed, closed and tight).\n");
   return 0;
}
