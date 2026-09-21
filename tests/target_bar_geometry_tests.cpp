// Standalone positioning tests (not the DLL), in an x86 VS Developer prompt:
// cl /std:c++17 /EHsc /W4 /WX tests\target_bar_geometry_tests.cpp
// Run target_bar_geometry_tests.exe. Do not define NDEBUG.
#include "../PatcherDLL/src/render/target_bar_geometry.hpp"

#include <cassert>
#include <cstdio>
#include <limits>
#include <random>

using namespace target_bar_geometry;

static void identity(float m[16])
{
   for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
}
static bool near(float a, float b) { return std::fabs(a-b) < 1.0e-5f; }

int main()
{
   float model[16], camera[16], out[3], anchor[3];
   identity(model);
   identity(camera);
   model[14] = -10;
   const Box vehicle = { {-4, -0.5f, -2}, {4, 0.5f, 2} };
   Box world;
   assert(world_bounds(vehicle, model, world));
   top_centre(world, anchor);
   assert(near(anchor[0], 0) && near(anchor[1], 0.5f) && near(anchor[2], -10));
   assert(project_anchor(world, camera, 1, 1, out));
   assert(near(out[0], 0.5f) && near(out[1], 0.475f)); // NOT projected-box top .46875

   // Unused W lanes in engine matrices need not be initialized.
   model[3] = camera[15] = std::numeric_limits<float>::quiet_NaN();
   assert(world_bounds(vehicle, model, world));
   assert(project_anchor(world, camera, 1, 1, out));
   model[3] = 0;
   camera[15] = 1;

   // The anchor is a single world point, unaffected by camera position or FOV.
   camera[12] = 3;
   camera[13] = 2;
   assert(project_anchor(world, camera, 2, 0.5f, out));
   assert(near(out[0], 0.425f) && near(out[1], 0.65f));
   top_centre(world, out);
   for (int a=0; a<3; ++a) assert(near(anchor[a], out[a]));

   // Model rotation changes WORLD bounds, not the camera-selected silhouette.
   model[0] = model[5] = 0; model[1] = 1; model[4] = -1;
   assert(world_bounds(vehicle, model, world));
   top_centre(world, anchor);
   assert(near(anchor[1], 4));
   identity(model); identity(camera);
   model[14] = -10;

   // A jumping unit retains bbox size but follows its current collision centre.
   const Box standing = { {-0.4f, 0, -0.4f}, {0.4f, 2, 0.4f} };
   const float jumpingCentre[3] = { 7, 5, -10 };
   assert(recenter_bounds(standing, jumpingCentre, world));
   top_centre(world, anchor);
   assert(near(anchor[0], 7) && near(anchor[1], 6) && near(anchor[2], -10));
   assert(near(world.max[1]-world.min[1], 2));

   // Pinning is independent of geometry, FOV and distance. Insets belong to the
   // authored HUD footprint, not a perspective-resized rectangle.
   const Insets insets = { 0.10f, 0.10f, 0.107f, 0 };
   out[0] = -50; out[1] = -100;
   assert(pin_to_screen(out, 1920, 1080, insets));
   assert(out[0] >= insets.left && out[1] >= insets.top);
   assert(out[0] < 0.101f && out[1] < 0.101f);
   out[0] = 50; out[1] = 100;
   assert(pin_to_screen(out, 1920, 1080, insets));
   assert(out[0] <= 1-insets.right && near(out[1], 1));
   const Insets rightUp = { 0, 0.025f, 0.1f, 0 };
   out[0] = -100; out[1] = -100;
   assert(pin_to_screen(out, 1366, 768, rightUp));
   assert(out[0] == 0 && out[1] >= 0.025f);

   // Up close the top-centre can cross the eye plane while the vehicle is still
   // in front. Preserve its above/below direction, then pin it without flipping.
   world = { {-2, 2, -1}, {2, 6, 1} };
   assert(project_anchor(world, camera, 1, 1, out));
   assert(out[1] < 0);
   assert(pin_to_screen(out, 1920, 1080, insets));
   assert(out[1] < 0.101f);
   world = { {-2, 2, 1}, {2, 6, 3} };
   assert(!project_anchor(world, camera, 1, 1, out));

   Box bad = vehicle;
   bad.max[0] = bad.min[0]-1;
   assert(!world_bounds(bad, model, world));
   bad = {};
   assert(!world_bounds(bad, model, world));
   bad = vehicle;
   bad.min[0] = std::numeric_limits<float>::quiet_NaN();
   assert(!world_bounds(bad, model, world));
   assert(world_bounds(vehicle, model, world));
   assert(!project_anchor(world, camera, 0, 1, out));
   assert(!project_anchor(world, camera, 1, std::numeric_limits<float>::infinity(), out));
   model[0] = std::numeric_limits<float>::infinity();
   assert(!world_bounds(vehicle, model, world));
   out[0]=out[1]=0.5f;
   assert(!pin_to_screen(out, 0, 1080, insets));
   assert(!pin_to_screen(out, 1920, 1080, {0.6f, 0, 0.4f, 0}));
   assert(!pin_to_screen(out, 1920, 1080, {-0.1f, 0, 0, 0}));

   std::mt19937 rng(20260921);
   std::uniform_real_distribution<float> coordinate(-100, 100);
   for (int trial=0; trial<20000; ++trial) {
      const unsigned width=640+rng()%3000, height=480+rng()%1600;
      out[0]=coordinate(rng); out[1]=coordinate(rng);
      assert(pin_to_screen(out, width, height, insets));
      assert(out[0]>=insets.left && out[0]<=1-insets.right);
      assert(out[1]>=insets.top && out[1]<=1-insets.bottom);
      const float x=out[0]*width, y=out[1]*height;
      assert(std::fabs(x-std::round(x))<0.0005f);
      assert(std::fabs(y-std::round(y))<0.0005f);
   }
   std::puts("Target-bar positioning tests passed (20,000 randomized pin/snap cases).");
}
