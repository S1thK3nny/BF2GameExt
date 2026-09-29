// Standalone tests (not a DLL build). Compile with /std:c++17 /W4 /WX, no NDEBUG.
#include "../PatcherDLL/src/render/hud_world_markers_core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>

using namespace hud_world_markers;

namespace {

// A camera at the origin looking down world -Z, 90 degrees across and down.
const float kIdentity[16] = { 1, 0, 0, 0,   0, 1, 0, 0,   0, 0, 1, 0,   0, 0, 0, 1 };

Camera square(const float* matrix = kIdentity)
{
   return Camera{ matrix, 1.0f, 1.0f, 1000, 1000 };
}

bool near(double a, double b, double tolerance = 1e-4) { return std::fabs(a - b) <= tolerance; }

Placement at(const Camera& camera, float x, float y, float z, bool sideSlide = true)
{
   const float world[3] = { x, y, z };
   Placement p{};
   assert(place(camera, world, sideSlide, p));
   return p;
}

} // namespace

int main()
{
   const Camera cam = square();

   // Straight ahead is the centre, on screen, with no turn.
   Placement p = at(cam, 0, 0, -10);
   assert(p.onScreen && near(p.position[0], 0.5) && near(p.position[1], 0.5) && p.position[2] == 0);
   assert(p.rotation[0] == 0 && p.rotation[1] == 0 && p.rotation[2] == 0);

   // In view: right is +x, up is -y on the HUD.
   p = at(cam, 5, 0, -10);
   assert(p.onScreen && near(p.position[0], 0.75) && near(p.position[1], 0.5) && near(p.rotation[2], 90));
   p = at(cam, 0, 5, -10);
   assert(p.onScreen && near(p.position[1], 0.25) && near(p.rotation[2], 0));
   // The safe square's edge is still on screen; just past it is not.
   assert(at(cam, 9, 0, -10).onScreen && !at(cam, 9.2f, 0, -10).onScreen);

   // In front but out of view: pinned to the 0.9 square along its direction.
   p = at(cam, 20, 0, -10);
   assert(!p.onScreen && near(p.position[0], 0.95) && near(p.position[1], 0.5) && near(p.rotation[2], 90));
   p = at(cam, 0, 20, -10);
   assert(!p.onScreen && near(p.position[1], 0.05) && near(p.rotation[2], 0));
   p = at(cam, 0, -20, -10);
   assert(!p.onScreen && near(p.position[1], 0.95) && near(std::fabs(p.rotation[2]), 180));
   p = at(cam, 20, 20, -10);                                  // a corner
   assert(!p.onScreen && near(p.position[0], 0.95) && near(p.position[1], 0.05) && near(p.rotation[2], 45));

   // Behind: mirrored, so behind-right still points right.
   p = at(cam, 5, 0, 10);
   assert(!p.onScreen && near(p.position[0], 0.95) && near(p.position[1], 0.5) && near(p.rotation[2], 90));
   p = at(cam, -5, 0, 10);
   assert(near(p.position[0], 0.05) && near(p.rotation[2], -90));

   // Behind and steeply up: slides to the side edge, unless in a flyer.
   p = at(cam, 1, 5, 10);
   assert(near(p.position[0], 0.95) && near(p.position[1], 0.5 - 0.5 * 0.27));
   p = at(cam, 1, 5, 10, false);
   assert(near(p.position[1], 0.05) && near(p.position[0], 0.5 + 0.5 * 0.18));
   // Well off to the side in front, the same slide (facing under 0.2).
   p = at(cam, 10, 10, -1);
   assert(near(p.position[0], 0.95) && near(p.position[1], 0.5 - 0.5 * 0.27));
   // Nearer the view axis (60 degrees off, facing 0.51) the slide does not apply.
   p = at(cam, 1, 5, -3, true);
   assert(!p.onScreen && near(p.position[1], 0.05) && near(p.position[0], 0.5 + 0.5 * 0.18));

   // Straight behind: the bottom edge, turned all the way round.
   p = at(cam, 0, 0, 10);
   assert(near(p.position[0], 0.5) && near(p.position[1], 0.95) && near(std::fabs(p.rotation[2]), 180));
   // Behind and above in a flyer: the top edge; outside one the slide flattens
   // it to nothing, so the bottom.
   assert(near(at(cam, 0, 1, 10, false).position[1], 0.05));
   assert(near(at(cam, 0, 1, 10, true).position[1], 0.95));
   // On the camera plane: finite, pinned by direction.
   p = at(cam, 3, 0, 0);
   assert(!p.onScreen && near(p.position[0], 0.95));

   // Whole pixels, and the turn in pixels on a wide screen: halfway up-right on
   // 1920 x 1080 is atan2(960, 540), not 45 degrees.
   const Camera wide{ kIdentity, 1.0f, 1.0f, 1920, 1080 };
   p = at(wide, 0.3333f, -0.1234f, -1);
   assert(std::round(p.position[0] * 1920) == p.position[0] * 1920);
   assert(std::round(p.position[1] * 1080) == p.position[1] * 1080);
   p = at(wide, 0.5f, 0.5f, -1);
   assert(near(p.rotation[2], std::atan2(960.0, 540.0) * kDegrees, 1e-3));

   // A moved and turned camera: at x = 100, looking down world +X.
   const float turned[16] = { 0, 0, 1, 0,   0, 1, 0, 0,   -1, 0, 0, 0,   100, 0, 0, 1 };
   p = at(square(turned), 110, 0, 0);
   assert(p.onScreen && near(p.position[0], 0.5) && near(p.position[1], 0.5));
   p = at(square(turned), 110, 0, 5);
   assert(p.onScreen && near(p.position[0], 0.75));
   p = at(square(turned), 90, 0, 0);                          // behind it
   assert(!p.onScreen && near(p.position[1], 0.95));

   // Field of view: a narrower one pushes the same point out.
   const Camera zoomed{ kIdentity, 0.25f, 0.25f, 1000, 1000 };
   assert(at(cam, 2, 0, -10).onScreen && !at(zoomed, 2.5f, 0, -10).onScreen);

   // Bad input places nothing.
   Placement none{};
   const float point[3] = { 0, 0, -10 };
   const float nan[3] = { NAN, 0, -10 };
   assert(!place(Camera{ nullptr, 1, 1, 100, 100 }, point, true, none));
   assert(!place(Camera{ kIdentity, 0, 1, 100, 100 }, point, true, none));
   assert(!place(Camera{ kIdentity, 1, 1, 0, 100 }, point, true, none));
   assert(!place(cam, nan, true, none));
   float broken[16];
   for (int i = 0; i < 16; ++i) broken[i] = kIdentity[i];
   broken[13] = INFINITY;
   assert(!place(square(broken), point, true, none));

   // Anywhere at all: on screen inside the square, else on its edge, finite.
   std::mt19937 rng(28092026);
   std::uniform_real_distribution<float> coord(-500, 500);
   for (int i = 0; i < 20000; ++i) {
      p = at(wide, coord(rng), coord(rng), coord(rng), (i & 1) != 0);
      const double ex = std::fabs(p.position[0] - 0.5) * 2, ey = std::fabs(p.position[1] - 0.5) * 2;
      assert(ex <= kSafeZone + 1e-3 && ey <= kSafeZone + 1e-3);
      if (!p.onScreen) assert(near(std::fmax(ex, ey), kSafeZone, 2e-3));
      assert(p.rotation[2] >= -180.001f && p.rotation[2] <= 180.001f);
   }

   const float a[3] = { 0, 0, 0 }, b[3] = { 3, 4, 0 };
   assert(distance(a, b) == 5 && distance(b, a) == 5 && distance(a, a) == 0);

   std::puts("World marker tests passed (centre, view, edge pinning, behind, side slide, flyers, pixels, "
             "wide-screen turn, turned camera, field of view, bad input, 20,000 random points, distance).");
}
