#pragma once

#include <cstring>

// =============================================================================
// TrueWidescreen HUD files: the placement maths, engine-independent and shared
// with the standalone tests. Sizes are the interface's screen (s_screenFull)
// in pixels; "layout" is the 4:3 screen of the same height that an opted-in
// file is laid out on.
// =============================================================================

namespace hud_true_widescreen {

// The engine treats a screen as wide, and squashes its HUD into a band, once
// height / width is below 0.75: that is when RedInterfaceScreen draws the HUD
// groups through the 640 x 480 mapping and the loader sets the letterbox.
inline bool is_wide(float width, float height)
{
   return width > 0.0f && height > 0.0f && height < 0.75f * width;
}

// An opted-in file is laid out on a 4:3 screen of the real height.
inline float layout_width(float height)
{
   return height * (4.0f / 3.0f);
}

enum class Anchor { Left, Center, Right };

// The edge a top-level piece keeps its distance from, picked from where it is
// written, in layout pixels: the left third, the middle third or the right.
inline Anchor anchor_for(float x, float layoutWidth)
{
   if (x < layoutWidth / 3.0f) return Anchor::Left;
   if (x > layoutWidth * (2.0f / 3.0f)) return Anchor::Right;
   return Anchor::Center;
}

// How far right a piece is drawn from where the layout puts it: none, half or
// all of the width the screen has beyond the layout.
inline float slide_for(Anchor anchor, float width, float layoutWidth)
{
   const float spare = width - layoutWidth;
   switch (anchor) {
   case Anchor::Left:   return 0.0f;
   case Anchor::Center: return spare * 0.5f;
   case Anchor::Right:  return spare;
   }
   return 0.0f;
}

// A piece written exactly at the layout's corner with nothing moving it there
// is a plain container: its children are placed one by one instead.
inline bool is_origin(float x, float y)
{
   return x == 0.0f && y == 0.0f;
}

// A position that follows the world (the reticule, a lock-on, a marker) comes
// as a fraction of the real screen's width. The engine multiplies the fraction
// it is given by the layout width, and the draw then slides the element by the
// slides above it, so it is given the fraction that lands on the real point.
inline float tracked_fraction(float fraction, float width, float layoutWidth, float slide)
{
   return (fraction * width - slide) / layoutWidth;
}

// The parent matrix an opted-in top-level piece is drawn under instead of its
// screen group's: one layout pixel to one screen pixel, slid right by `slide`
// pixels. It sits on the plane the HUD groups are drawn on, z = 640 / t, where
// the interface camera (frustum width t at distance 1) sees 640 units across;
// row-vector order, as PblMatrix.
inline void pixel_parent(float m[16], float width, float height, float t, float slide)
{
   const float k = 640.0f / width;   // world units per pixel on that plane
   std::memset(m, 0, 16 * sizeof(float));
   m[0] = k;
   m[5] = -k;
   m[10] = 1.0f;
   m[12] = (slide - 0.5f * width) * k;
   m[13] = 0.5f * height * k;
   m[14] = -640.0f / t;
   m[15] = 1.0f;
}

} // namespace hud_true_widescreen
