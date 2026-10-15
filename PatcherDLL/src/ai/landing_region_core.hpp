#pragma once

#include "flyer_wingmen_core.hpp"

#include <cmath>
#include <limits>

// =============================================================================
// A map's landing regions, on their own: the shapes the game keeps for them
// (PblRegion) and what the wingmen ask of one. No engine reads, so
// tests/flyer_wingmen_tests.cpp can exercise all of it; flyer_wingmen.cpp reads
// the regions out of TriggerRegionManager (docs/RE/FlyerAI.md, "Landing regions
// as hangars").
//
// A player can land only inside a landing region, so every hangar it can land
// in sits in one; custom maps put them in the open air too. The user: "Landing
// regions should encompass the hangar model roughly, and so should work.
// However in custom maps they are used in open air settings too." So a region
// alone is not a hangar: inside one, the wingmen's roof test looks up to the
// region's top (top) instead of a fixed 40 m, and only a roof found makes it a
// hangar.
//
//   - inside: PblRegion::IsPointInside (modtools 0x007E7820, Phantom
//     0x00868C40), shape by shape, its comparisons as the game makes them.
//   - top: the region's highest point; NaN for a plane, which has none.
// =============================================================================

namespace landing_region {

using flyer_wingmen::V3;

// PblRegion's shapes (its +0x54), as modtools numbers them.
enum Shape : int {
   kSphere     = 1,   // radius w
   kBox        = 2,   // half sizes x, y, z along right, up and forward
   kCylinder   = 3,   // radius x, half height y along up
   kPlane      = 4,   // inside: forward . p < w
   kBoxAligned = 5,   // half sizes x, y, z along the world's axes
   kCylinderX  = 6,   // on the world's x: half length y, radius x
   kCylinderY  = 7,
   kCylinderZ  = 8,
};

// One, as read from its PblRegion: its matrix's rows (right +0x00, up +0x10,
// forward +0x20, position +0x30), its sizes (+0x40) and its shape (+0x54).
struct Region {
   V3    right{1.0f, 0.0f, 0.0f}, up{0.0f, 1.0f, 0.0f}, fwd{0.0f, 0.0f, 1.0f}, at;
   float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
   int   shape = 0;
};

inline bool inside(const Region& r, V3 p)
{
   const V3 d = p - r.at;
   switch (r.shape) {
   case kSphere:
      return flyer_wingmen::dot(d, d) < r.w * r.w;
   case kBox:
      return std::fabs(flyer_wingmen::dot(d, r.right)) < r.x && std::fabs(flyer_wingmen::dot(d, r.up)) < r.y &&
             std::fabs(flyer_wingmen::dot(d, r.fwd)) < r.z;
   case kCylinder: {
      if (!(std::fabs(flyer_wingmen::dot(d, r.up)) < r.y)) return false;
      const float a = flyer_wingmen::dot(d, r.right), b = flyer_wingmen::dot(d, r.fwd);
      return a * a + b * b < r.x * r.x;
   }
   case kPlane:
      return flyer_wingmen::dot(r.fwd, p) < r.w;
   case kBoxAligned:
      return std::fabs(d.x) < r.x && std::fabs(d.y) < r.y && std::fabs(d.z) < r.z;
   case kCylinderX:
      return std::fabs(d.x) < r.y && d.y * d.y + d.z * d.z < r.x * r.x;
   case kCylinderY:
      return std::fabs(d.y) < r.y && d.x * d.x + d.z * d.z < r.x * r.x;
   case kCylinderZ:
      return std::fabs(d.z) < r.y && d.x * d.x + d.y * d.y < r.x * r.x;
   default:
      return false;
   }
}

// Its highest point: a turned box's highest corner, a turned cylinder's highest
// rim; NaN for a plane or an unknown shape.
inline float top(const Region& r)
{
   switch (r.shape) {
   case kSphere:
      return r.at.y + r.w;
   case kBox:
      return r.at.y + std::fabs(r.right.y) * r.x + std::fabs(r.up.y) * r.y + std::fabs(r.fwd.y) * r.z;
   case kCylinder: {
      const float lean = 1.0f - r.up.y * r.up.y;
      return r.at.y + std::fabs(r.up.y) * r.y + r.x * std::sqrt(lean > 0.0f ? lean : 0.0f);
   }
   case kBoxAligned:
   case kCylinderY:
      return r.at.y + r.y;
   case kCylinderX:
   case kCylinderZ:
      return r.at.y + r.x;
   default:
      return std::numeric_limits<float>::quiet_NaN();
   }
}

} // namespace landing_region
