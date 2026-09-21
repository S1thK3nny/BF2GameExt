#pragma once

#include <cmath>

// Engine-independent geometry for target.position. Matrices have the engine's
// right/up/forward/translation layout (four floats per row); camera forward is
// -Z. Keep this separate so projection edge cases can be tested without the DLL.
namespace target_bar_geometry {

struct Box {
   float min[3];
   float max[3];
};

inline bool valid_box(const Box& box)
{
   bool extent = false;
   for (int a = 0; a < 3; ++a) {
      if (!std::isfinite(box.min[a]) || !std::isfinite(box.max[a]) ||
          box.min[a] > box.max[a]) return false;
      extent = extent || box.min[a] < box.max[a];
   }
   return extent;
}

inline bool valid_matrix(const float m[16])
{
   // Only the 4x3 affine entries are used. Don't depend on padding/W lanes in
   // the engine's matrix storage being initialized.
   for (int row = 0; row < 4; ++row)
      for (int a = 0; a < 3; ++a)
         if (!std::isfinite(m[row*4+a])) return false;
   // A newly allocated pose can contain all zeroes before its first evaluation.
   for (int row = 0; row < 3; ++row) {
      const float* r = m + row * 4;
      const float length2 = r[0]*r[0] + r[1]*r[1] + r[2]*r[2];
      if (!(length2 > 1.0e-8f) || !std::isfinite(length2)) return false;
   }
   return true;
}

inline void transform(const float m[16], const float p[3], float out[3])
{
   for (int a = 0; a < 3; ++a)
      out[a] = p[0]*m[a] + p[1]*m[4+a] + p[2]*m[8+a] + m[12+a];
}

inline void to_camera(const float camera[16], const float world[3], float out[3])
{
   const float r[3] = { world[0]-camera[12], world[1]-camera[13], world[2]-camera[14] };
   for (int a = 0; a < 3; ++a)
      out[a] = r[0]*camera[a*4] + r[1]*camera[a*4+1] + r[2]*camera[a*4+2];
}

// Camera-independent world AABB. The eight model corners are transformed, NOT
// projected: changing the view must never select a different corner as anchor.
inline bool world_bounds(const Box& local, const float model[16], Box& out)
{
   if (!valid_box(local) || !valid_matrix(model)) return false;
   Box result = {};
   for (int i = 0; i < 8; ++i) {
      const float p[3] = { (i & 1) ? local.max[0] : local.min[0],
                           (i & 2) ? local.max[1] : local.min[1],
                           (i & 4) ? local.max[2] : local.min[2] };
      float world[3];
      transform(model, p, world);
      for (int a = 0; a < 3; ++a) {
         if (!std::isfinite(world[a])) return false;
         if (!i || world[a] < result.min[a]) result.min[a] = world[a];
         if (!i || world[a] > result.max[a]) result.max[a] = world[a];
      }
   }
   if (!valid_box(result)) return false;
   out = result;
   return true;
}

// Infantry collision boxes are symmetric around the live collision centre.
// Keep their stance-dependent size, but don't inherit a stale world position
// when the native jump/flail branch updates the centre without updating AABB.
inline bool recenter_bounds(const Box& box, const float centre[3], Box& out)
{
   if (!valid_box(box)) return false;
   Box result;
   for (int a = 0; a < 3; ++a) {
      const float half = box.max[a]*0.5f - box.min[a]*0.5f;
      result.min[a] = centre[a] - half;
      result.max[a] = centre[a] + half;
   }
   if (!valid_box(result)) return false;
   out = result;
   return true;
}

inline void top_centre(const Box& world, float out[3])
{
   out[0] = world.min[0]*0.5f + world.max[0]*0.5f;
   out[1] = world.max[1]; // world Y offset is deliberately zero
   out[2] = world.min[2]*0.5f + world.max[2]*0.5f;
}

inline bool project_anchor(const Box& world, const float camera[16],
                           float tanW, float tanH, float out[3])
{
   if (!valid_box(world) || !valid_matrix(camera) ||
       !std::isfinite(tanW) || !std::isfinite(tanH) ||
       !(tanW > 1.0e-6f) || !(tanH > 1.0e-6f)) return false;

   // Reject wholly behind-camera targets, but retain a close vehicle whose
   // bounds intersect the camera plane even when its top-centre is behind it.
   float nearest[3];
   for (int a = 0; a < 3; ++a)
      nearest[a] = camera[8+a] >= 0 ? world.min[a] : world.max[a];
   float cameraNear[3];
   to_camera(camera, nearest, cameraNear);
   if (!(cameraNear[2] < -1.0e-4f)) return false;

   float anchor[3], p[3];
   top_centre(world, anchor);
   to_camera(camera, anchor, p);
   for (float c : p)
      if (!std::isfinite(c)) return false;
   // A positive depth floor prevents flips/infinity while crossing the camera.
   // The resulting out-of-view position is pinned below, not used as a size.
   const double depth = p[2] < -0.01f ? -(double)p[2] : 0.01;
   out[0] = (float)(0.5 + (double)p[0] / (2.0*tanW*depth));
   out[1] = (float)(0.5 - (double)p[1] / (2.0*tanH*depth));
   out[2] = 0.0f;
   return std::isfinite(out[0]) && std::isfinite(out[1]);
}

// Space around the POSITION anchor, in viewport fractions (built in by caller).
// This does not set/read/scale bar dimensions. It lets a .hud keep manual child
// offsets and labels while reserving sufficient space at each screen edge.
struct Insets { float left, top, right, bottom; };

inline bool valid_insets(const Insets& insets)
{
   const float values[] = { insets.left, insets.top, insets.right, insets.bottom };
   for (float v : values)
      if (!std::isfinite(v) || v < 0 || v >= 1) return false;
   return insets.left + insets.right < 1 && insets.top + insets.bottom < 1;
}

inline bool pin_to_screen(float position[3], unsigned width, unsigned height,
                          const Insets& insets)
{
   if (!width || !height || !valid_insets(insets) ||
       !std::isfinite(position[0]) || !std::isfinite(position[1])) return false;
   const double minPx[2] = { std::ceil((double)insets.left*width),
                             std::ceil((double)insets.top*height) };
   const double maxPx[2] = { std::floor((1.0-(double)insets.right)*width),
                             std::floor((1.0-(double)insets.bottom)*height) };
   const double size[2] = { (double)width, (double)height };
   for (int a = 0; a < 2; ++a) {
      if (minPx[a] > maxPx[a]) return false;
      double px = std::round((double)position[a]*size[a]);
      if (px < minPx[a]) px = minPx[a];
      if (px > maxPx[a]) px = maxPx[a];
      position[a] = (float)(px / size[a]);
   }
   position[2] = 0;
   return true;
}

} // namespace target_bar_geometry
