#pragma once

#include <cmath>

// Engine-independent placement of a world point on the HUD, tested by
// tests/hud_world_markers_tests.cpp. The rule is the one the stock Target element
// uses for objective markers, LockOnManager::CalculateScreenCoordinates (modtools
// 0x00453DF0, Steam 0x0057B430, GOG 0x0057C1B0; see docs/RE/HUDSystem.md, "World
// markers and distances"):
//
// - A point in front of the camera and inside the safe square is on screen.
// - Anything else is pinned to that square's edge along its direction on screen.
//   Behind the camera the direction is mirrored, so it still points the way to turn.
// - Outside a flyer, a point more than about 78 degrees off the view axis slides to
//   the left or right edge rather than the top or bottom.
//
// Matrices use the engine's right/up/forward/translation rows; camera forward is -Z.
namespace hud_world_markers {

constexpr double kSafeZone   = 0.9;  // RedRenderer's default safe zone (Phantom 0x0087FB30)
constexpr double kSideFacing = 0.2;  // cos of the view angle below which a point slides sideways
constexpr double kSideSlope  = 0.3;  // how far up or down of the side such a point may sit
constexpr double kDegrees    = 57.295779513082320876;

struct Camera {
   const float* matrix;   // 16 floats: right, up, forward, translation rows
   float tanW, tanH;      // tangent of half the field of view, across and down
   unsigned width, height;
};

struct Placement {
   float position[3];     // viewport fractions, Y down, on whole pixels; z is 0
   float rotation[3];     // (0, 0, degrees) for EventRotation
   bool  onScreen;
};

inline bool finite3(const float v[3])
{
   return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

inline double distance(const float a[3], const float b[3])
{
   const double d[3] = { (double)a[0] - b[0], (double)a[1] - b[1], (double)a[2] - b[2] };
   return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// Places `world` for `camera`. `sideSlide` is the stock rule for everything but a
// flyer. The rotation turns an arrow drawn pointing up toward the point: HUD Y is
// down and a positive EventRotation turns up toward +X, so it is atan2(dx, -dy)
// in pixels, the stock off-screen arrow's angle.
inline bool place(const Camera& camera, const float world[3], bool sideSlide, Placement& out)
{
   if (!camera.matrix || !camera.width || !camera.height || !finite3(world) ||
       !(camera.tanW > 1.0e-6f) || !(camera.tanH > 1.0e-6f) ||
       !std::isfinite(camera.tanW) || !std::isfinite(camera.tanH)) return false;
   const float* m = camera.matrix;
   for (int i = 0; i < 16; ++i)
      if (i % 4 != 3 && !std::isfinite(m[i])) return false;

   const double r[3] = { (double)world[0] - m[12], (double)world[1] - m[13], (double)world[2] - m[14] };
   double p[3];
   for (int a = 0; a < 3; ++a)
      p[a] = r[0] * m[a * 4] + r[1] * m[a * 4 + 1] + r[2] * m[a * 4 + 2];

   // Up is positive here. Dividing by depth only in front keeps a point near
   // the camera plane from blowing up; its direction alone decides the edge.
   const double depth = -p[2];
   const double dx = p[0] / camera.tanW, dy = p[1] / camera.tanH;
   double x = dx, y = dy;
   bool onScreen = false;
   if (depth > 1.0e-4) {
      x = dx / depth;
      y = dy / depth;
      onScreen = std::fabs(x) <= kSafeZone && std::fabs(y) <= kSafeZone;
   }
   if (!onScreen) {
      x = dx;
      y = dy;
      const double length = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
      const double facing = length > 0.0 ? depth / length : 1.0;
      if (sideSlide && facing < kSideFacing && std::fabs(y) > kSideSlope * std::fabs(x))
         y = std::copysign(kSideSlope * std::fabs(x), y);
      const double edge = std::fmax(std::fabs(x), std::fabs(y));
      if (edge > 1.0e-12) {
         x *= kSafeZone / edge;
         y *= kSafeZone / edge;
      } else {
         x = 0.0;                 // straight behind: the bottom edge
         y = -kSafeZone;
      }
   }

   const double fx = 0.5 + 0.5 * x, fy = 0.5 - 0.5 * y;
   out.position[0] = (float)(std::round(fx * camera.width) / camera.width);
   out.position[1] = (float)(std::round(fy * camera.height) / camera.height);
   out.position[2] = 0.0f;
   const double px = x * camera.width, py = y * camera.height;   // py is up
   out.rotation[0] = out.rotation[1] = 0.0f;
   out.rotation[2] = (px != 0.0 || py != 0.0) ? (float)(std::atan2(px, py) * kDegrees) : 0.0f;
   out.onScreen = onScreen;
   return finite3(out.position) && std::isfinite(out.rotation[2]);
}

} // namespace hud_world_markers
