// Standalone tests, not a DLL build. Run without NDEBUG.
// cl /std:c++17 /EHsc /W4 /WX tests\hud_horizon_tests.cpp
#include "../PatcherDLL/src/render/hud_horizon_math.hpp"

#include <cassert>
#include <cstdio>
#include <limits>
#include <random>

constexpr double kPi = 3.14159265358979323846;
static bool near(float a, float b) { return std::fabs(a - b) < 0.0001f; }

static void pose(float m[16], double roll, double pitch = 0, double yaw = 0)
{
   const double r = roll * kPi / 180, p = pitch * kPi / 180, y = yaw * kPi / 180;
   const double c = std::cos(r), s = std::sin(r), cp = std::cos(p), sp = std::sin(p);
   const double basis[3][3] = { {c, s*cp, s*sp}, {-s, c*cp, c*sp}, {0, -sp, cp} };
   for (int i = 0; i < 16; ++i) m[i] = 0;
   for (int i = 0; i < 3; ++i) {
      m[i*4] = static_cast<float>(basis[i][0]*std::cos(y) + basis[i][2]*std::sin(y));
      m[i*4+1] = static_cast<float>(basis[i][1]);
      m[i*4+2] = static_cast<float>(-basis[i][0]*std::sin(y) + basis[i][2]*std::cos(y));
   }
   m[15] = 1;
}

static void project(const float m[16], const double point[3], double tw, double th,
                    double width, double height, double out[2])
{
   double cam[3] = {};
   for (int axis = 0; axis < 3; ++axis)
      for (int i = 0; i < 3; ++i) cam[axis] += (point[i] - m[12+i]) * m[axis*4+i];
   out[0] = width * (0.5 + cam[0] / (-cam[2] * tw) * 0.5);
   out[1] = height * (0.5 - cam[1] / (-cam[2] * th) * 0.5);
}

int main()
{
   hud_horizon::State state;
   float m[16];
   for (float roll : {0.0f, 45.0f, 90.0f, -90.0f, 179.0f, -179.0f}) {
      for (float pitch : {-80.0f, 0.0f, 80.0f}) {
         for (float yaw : {-123.0f, 0.0f, 151.0f}) {
            pose(m, roll, pitch, yaw);
            assert(near(state.update(m, 2, 1.125f, 1920, 1080), roll));
            // Zoom and resolution changes do not change the bank angle.
            assert(near(state.update(m, 0.5f, 0.28125f, 3840, 2160), roll));
            assert(near(state.update(m, 1, 0.75f, 640, 480), roll));
         }
      }
   }
   pose(m, 180);
   assert(near(std::fabs(state.update(m, 1, 1, 1000, 1000)), 180));
   pose(m, -180);
   assert(near(std::fabs(state.update(m, 1, 1, 1000, 1000)), 180));

   // Nonstandard projection aspect: actual projected direction, not a guessed
   // resolution correction. 45-degree bank becomes atan(2) in pixel space.
   pose(m, 45);
   assert(near(state.update(m, 1, 1, 2000, 1000), 63.434949f));

   pose(m, 37);
   state.update(m, 1, 1, 1000, 1000);
   for (double pitch : {89.9, 90.0, -90.0, 89.2}) {
      pose(m, -80, pitch);
      assert(near(state.update(m, 1, 1, 1000, 1000), 37));
      assert(state.atPole);
   }
   pose(m, -80, 88.0);
   assert(near(state.update(m, 1, 1, 1000, 1000), -80));
   assert(!state.atPole);
   state.reset(); // new mission, new camera, or removed listener
   pose(m, 90, 90);
   assert(near(state.update(m, 1, 1, 1000, 1000), 0));

   // Invalid/missing camera or projection must clear a previously banked angle.
   const float nan = std::numeric_limits<float>::quiet_NaN();
   const float inf = std::numeric_limits<float>::infinity();
   pose(m, 90);
   state.update(m, 1, 1, 1000, 1000);
   assert(near(state.update(nullptr, 1, 1, 1000, 1000), 0));
   for (float bad : {0.0f, -1.0f, nan, inf}) {
      assert(near(state.update(m, bad, 1, 1000, 1000), 0));
      assert(near(state.update(m, 1, bad, 1000, 1000), 0));
   }
   assert(near(state.update(m, 1, 1, 0, 1000), 0));
   assert(near(state.update(m, 1, 1, 1000, 0), 0));
   for (int axis : {0, 1, 2, 4, 5, 6, 8, 9, 10}) {
      pose(m, 90);
      m[axis] = nan;
      assert(near(state.update(m, 1, 1, 1000, 1000), 0));
   }
   for (float& value : m) value = 0;
   assert(near(state.update(m, 1, 1, 1000, 1000), 0));
   pose(m, 90);
   for (int axis : {3, 7, 11, 12, 13, 14, 15}) m[axis] = nan;
   assert(near(state.update(m, 1, 1, 1000, 1000), 90));

   // Independent end-to-end direction oracle: project a small world-up segment
   // at the centre ray; apply the native D3DX Z rotation to HUD up (0,-1).
   std::mt19937 rng(0x484F5249);
   std::uniform_real_distribution<double> angle(-180, 180), pitch(-88, 88), fov(0.2, 3);
   for (int sample = 0; sample < 20000; ++sample) {
      pose(m, angle(rng), pitch(rng), angle(rng));
      m[12] = 123; m[13] = -456; m[14] = 789;
      const float tw = static_cast<float>(fov(rng)), th = static_cast<float>(fov(rng));
      const uint32_t width = 640 + rng()%3200, height = 480 + rng()%1600;
      double base[3], top[3], screenBase[2], screenTop[2];
      for (int i = 0; i < 3; ++i) base[i] = top[i] = m[12+i] - 10.0*m[8+i];
      top[1] += 0.25;
      project(m, base, tw, th, width, height, screenBase);
      project(m, top, tw, th, width, height, screenTop);
      double dx = screenTop[0] - screenBase[0], dy = screenTop[1] - screenBase[1];
      const double length = std::hypot(dx, dy);
      dx /= length; dy /= length;
      const double radians = state.update(m, tw, th, width, height) * kPi / 180;
      assert(std::fabs(std::sin(radians) - dx) < 1e-5);
      assert(std::fabs(-std::cos(radians) - dy) < 1e-5);
   }
   std::puts("Horizon tests passed (including 20,000 projected-direction cases).");
}
