// Standalone tests, not a DLL build. Run without NDEBUG.
// cl /std:c++17 /EHsc /W4 /WX tests\target_bar_fade_tests.cpp
#include "../PatcherDLL/src/render/target_bar_fade.hpp"

#include <cassert>
#include <cstdio>
#include <limits>

using target_bar_geometry::Box;

static float camera[16];
static float tanW = 1, tanH = 1;
static unsigned width = 1000, height = 1000;
static bool cameraAvailable = true;
static int projectionCalls = 0;
static Box lastProjectedBounds;
static constexpr target_bar_geometry::Insets insets = {0.10f, 0.10f, 0.107f, 0};

static void identity_camera()
{
   for (int i = 0; i < 16; ++i) camera[i] = i % 5 == 0 ? 1.0f : 0.0f;
}

static bool project(const Box& world, float* out)
{
   ++projectionCalls;
   lastProjectedBounds = world;
   return cameraAvailable && target_bar_geometry::project_anchor(world, camera, tanW, tanH, out)
      && target_bar_geometry::pin_to_screen(out, width, height, insets);
}

static bool near(float a, float b) { return std::fabs(a - b) < 1.0e-5f; }

static void at(const target_bar_fade::Position& position, float x, float y)
{
   assert(near(position.value[0], x) && near(position.value[1], y) && position.value[2] == 0);
}

static void anchored_at(float x, float y, float z)
{
   float anchor[3];
   target_bar_geometry::top_centre(lastProjectedBounds, anchor);
   assert(near(anchor[0], x) && near(anchor[1], y) && near(anchor[2], z));
}

int main()
{
   identity_camera();
   target_bar_fade::Position first, second;
   const Box standing = {{-1, 0, -11}, {1, 2, -9}};
   Box moved = {{1, 0, -11}, {3, 3, -9}};
   const Box corpse = {{1, 0, -11}, {3, 0.5f, -9}};
   const Box next = {{-5, 0, -21}, {-3, 4, -19}};
   at(first, -2, -2); // no previous target: never publish an uninitialised origin
   first.update(false, true, &standing, project);
   at(first, 0.5f, 0.4f);
   first.update(true, true, &moved, project); // living fade still follows movement
   at(first, 0.6f, 0.35f);
   moved = corpse; // the cached snapshot owns its data; not a pointer into the unit
   first.update(true, false, &corpse, project); // dead bounds must be ignored
   at(first, 0.6f, 0.35f);
   anchored_at(2, 3, -10);

   // Camera translation after death changes screen position, not the world anchor.
   camera[12] = 2;
   first.update(true, false, nullptr, project);
   at(first, 0.5f, 0.35f);
   camera[13] = 1;
   first.update(true, false, nullptr, project);
   at(first, 0.5f, 0.4f);
   camera[14] = 10;
   first.update(true, false, nullptr, project);
   at(first, 0.5f, 0.45f);
   anchored_at(2, 3, -10);

   // Rotate the view while the unit is gone; the fading marker must not follow it.
   identity_camera();
   camera[0] = camera[10] = std::cos(0.2f);
   camera[2] = -std::sin(0.2f); camera[8] = std::sin(0.2f);
   first.update(true, false, nullptr, project);
   assert(first.value[0] > 0.7f && first.value[0] < 0.72f);
   anchored_at(2, 3, -10);

   // Looking away hides the retained anchor; looking back reprojects it again.
   identity_camera();
   camera[0] = camera[10] = -1;
   first.update(true, false, nullptr, project);
   at(first, -2, -2);
   identity_camera();
   first.update(true, false, nullptr, project);
   at(first, 0.6f, 0.35f);

   const int before = projectionCalls;
   for (int i = 0; i < 100; ++i) {
      camera[12] = static_cast<float>(i) * 0.1f;
      first.update(true, false, nullptr, project); // death, then removed/stale handle
      anchored_at(2, 3, -10);
   }
   assert(projectionCalls == before + 100);
   assert(first.value[0] < 0.11f);

   // FOV, resolution, edge pinning and pixel snapping still use the current view.
   identity_camera();
   tanW = 2; tanH = 0.5f;
   first.update(true, false, nullptr, project);
   at(first, 0.55f, 0.2f);
   tanW = tanH = 1;
   width = 1920; height = 1080;
   camera[12] = -100;
   first.update(true, false, nullptr, project);
   assert(first.value[0] <= 1 - insets.right && first.value[0] > 0.89f);
   for (int axis = 0; axis < 2; ++axis) {
      const float pixels = first.value[axis] * (axis ? height : width);
      assert(std::fabs(pixels - std::round(pixels)) < 0.0005f);
   }
   width = height = 1000;
   identity_camera();

   first.update(false, true, &next, project); // another target takes over immediately
   at(first, 0.4f, 0.4f);
   first.update(false, false, &corpse, project); // new target already dead, no own anchor
   at(first, -2, -2);
   first.update(true, false, nullptr, project);
   at(first, -2, -2);
   first.update(false, true, &standing, project);
   first.update(false, true, nullptr, project); // new target has no valid bounds
   at(first, -2, -2);
   first.update(true, true, &standing, project);
   first.update(true, true, nullptr, project); // invalid live bounds invalidate old snapshot
   at(first, -2, -2);
   first.update(true, false, nullptr, project);
   at(first, -2, -2);
   Box invalid = standing;
   invalid.max[1] = std::numeric_limits<float>::quiet_NaN();
   first.update(true, true, &invalid, project);
   first.update(true, false, nullptr, project);
   at(first, -2, -2);

   // Temporarily missing/invalid camera data hides without discarding the world snapshot.
   cameraAvailable = false;
   first.update(true, true, &standing, project);
   at(first, -2, -2);
   cameraAvailable = true;
   first.update(true, false, nullptr, project);
   at(first, 0.5f, 0.4f);
   tanW = 0;
   first.update(true, false, nullptr, project);
   at(first, -2, -2);
   tanW = 1;
   first.update(true, false, nullptr, project);
   at(first, 0.5f, 0.4f);

   // A close vehicle can straddle the eye plane. Retain the box, not just a point.
   const Box closeVehicle = {{-2, 2, -1}, {2, 6, 1}};
   first.update(false, true, &closeVehicle, project);
   const float pinnedY = first.value[1];
   assert(pinnedY >= 0.1f && pinnedY < 0.102f);
   first.update(true, false, nullptr, project);
   at(first, 0.5f, pinnedY);
   camera[14] = -4;
   first.update(true, false, nullptr, project);
   at(first, -2, -2);
   identity_camera();
   first.update(true, false, nullptr, project);
   at(first, 0.5f, pinnedY);

   first.update(false, true, &next, project);
   second.update(false, true, &standing, project);
   camera[12] = 2;
   first.update(true, false, nullptr, project);
   second.update(true, true, &standing, project); // channels retain independent anchors
   at(first, 0.35f, 0.4f);
   at(second, 0.4f, 0.4f);
   first.reset(); // mission reset or removal of listener
   first.update(true, false, nullptr, project);
   at(first, -2, -2);
   std::puts("Target-bar world-space death/fade tests passed.");
}
