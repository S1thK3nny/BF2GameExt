#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

// =============================================================================
// TrueWidescreen HUD files: the placement maths, engine-independent and shared
// with the standalone tests. Sizes are the interface's screen (s_screenFull)
// in pixels; "layout" is the screen of the file's authored ratio and the same
// height that an opted-in file is laid out on.
// =============================================================================

namespace hud_true_widescreen {

// TrueWidescreen(1) files are authored for 4:3.
constexpr float kFourThree = 4.0f / 3.0f;

// The engine treats a screen as wide, and squashes its HUD into a band, once
// height / width is below 0.75: that is when RedInterfaceScreen draws the HUD
// groups through the 640 x 480 mapping and the loader sets the letterbox.
inline bool is_wide(float width, float height)
{
   return width > 0.0f && height > 0.0f && height < 0.75f * width;
}

// The ratios AuthoredRatio(w, h) takes, width over height: from square to
// past 32:9.
inline bool valid_ratio(float ratio)
{
   return ratio >= 1.0f && ratio <= 4.0f;
}

// An opted-in file is laid out on a screen of its authored ratio at the real
// height.
inline float layout_width(float height, float ratio = kFourThree)
{
   return height * ratio;
}

// Whether a file authored for `ratio` is laid out on this screen at all: a
// 4:3 file only on a screen the engine treats as wide, since on a 4:3 one the
// stock layout is the file's own; any other ratio on every screen, since the
// stock layout reads its numbers as fractions of the real screen.
inline bool laid_out_on(float width, float height, float ratio)
{
   if (!(width > 0.0f && height > 0.0f) || !valid_ratio(ratio)) return false;
   return std::fabs(ratio - kFourThree) > 1e-4f || is_wide(width, height);
}

enum class Anchor { Left, Center, Right };

// The edge a piece without a ScreenAnchor keeps its distance from, picked
// from where it is written, in layout pixels: the left third, the middle third
// or the right.
inline Anchor anchor_for(float x, float layoutWidth)
{
   if (x < layoutWidth / 3.0f) return Anchor::Left;
   if (x > layoutWidth * (2.0f / 3.0f)) return Anchor::Right;
   return Anchor::Center;
}

// The share of the width the screen has beyond the layout that a piece kept to
// an edge moves by: none, half or all of it.
inline float share_of(Anchor anchor)
{
   switch (anchor) {
   case Anchor::Left:   return 0.0f;
   case Anchor::Center: return 0.5f;
   case Anchor::Right:  return 1.0f;
   }
   return 0.0f;
}

// How far right a piece is drawn from where the layout puts it: its share of
// the width the screen has beyond the layout. On a screen narrower than the
// layout that width is negative, and the pieces move in instead.
inline float slide_for_share(float share, float width, float layoutWidth)
{
   return share * (width - layoutWidth);
}

inline float slide_for(Anchor anchor, float width, float layoutWidth)
{
   return slide_for_share(share_of(anchor), width, layoutWidth);
}

// ScreenAnchor names the edge a piece keeps, "Left", "Center" or "Right" in
// any case, or gives its share as a number from 0 (the left edge) to 1 (the
// right).
inline const char* anchor_name(Anchor anchor)
{
   switch (anchor) {
   case Anchor::Left:   return "Left";
   case Anchor::Center: return "Center";
   case Anchor::Right:  return "Right";
   }
   return "";
}

inline bool same_name(const char* a, const char* b)
{
   for (;; ++a, ++b) {
      const char x = (*a >= 'A' && *a <= 'Z') ? char(*a + ('a' - 'A')) : *a;
      const char y = (*b >= 'A' && *b <= 'Z') ? char(*b + ('a' - 'A')) : *b;
      if (x != y) return false;
      if (!x) return true;
   }
}

inline bool anchor_named(const char* name, Anchor& anchor)
{
   for (Anchor a : { Anchor::Left, Anchor::Center, Anchor::Right }) {
      if (name && same_name(name, anchor_name(a))) {
         anchor = a;
         return true;
      }
   }
   return false;
}

inline bool valid_share(float share)
{
   return share >= 0.0f && share <= 1.0f;
}

// The name a named anchor's share was read from.
inline Anchor named_anchor(float share)
{
   return share < 0.25f ? Anchor::Left : share > 0.75f ? Anchor::Right : Anchor::Center;
}

// The HUD editor offers ScreenAnchor as a list: 0 for none (the piece keeps
// to its third), then shares 0 to 1 in steps of 0.05, the three edges by name.
constexpr int kAnchorChoices = 22;

inline int anchor_index(bool anchored, float share)
{
   if (!anchored) return 0;
   const int step = static_cast<int>(std::floor(share * 20.0f + 0.5f));
   return 1 + (step < 0 ? 0 : step > 20 ? 20 : step);
}

inline float anchor_share(int index)
{
   return index <= 1 ? 0.0f : index >= kAnchorChoices - 1 ? 1.0f : (index - 1) * 0.05f;
}

// The choices that are the three edges, kept as names when written.
inline bool anchor_index_named(int index)
{
   return index == 1 || index == 11 || index == kAnchorChoices - 1;
}

// A piece written exactly at the layout's corner with nothing moving it there
// is a plain container, unless it has a ScreenAnchor: its children are placed
// one by one instead.
inline bool is_origin(float x, float y)
{
   return x == 0.0f && y == 0.0f;
}

// Where a load puts a piece, and where the HUD editor puts one it moves: a
// top-level piece written at the corner without an anchor is a plain
// container, slid not itself but through its children; any other piece is
// slid by its anchor's share if it has one (share not null), else by the
// third of the layout it sits in.
struct Placement {
   bool  container;
   float slide;
};

inline Placement placement(float x, float y, bool topLevel, const float* share, float width, float layoutWidth)
{
   if (share) return { false, slide_for_share(*share, width, layoutWidth) };
   if (topLevel && is_origin(x, y)) return { true, 0.0f };
   return { false, slide_for(anchor_for(x, layoutWidth), width, layoutWidth) };
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

// The elements an opted-in file made while it loaded, each kept with the
// vtable and drawable it had, so that a later element at a freed one's address
// can be told apart, and the ratio its file was authored for. Open addressing
// on the element's address, filled to three quarters at most; Capacity is a
// power of two.
template <int Capacity>
struct MadeSet {
   static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

   struct Entry {
      const void* element;
      const void* red;
      uint32_t    vtable;
      float       ratio;
   };

   Entry entries[Capacity] = {};
   int   count = 0;

   static unsigned slot_of(const void* element)
   {
      const uint32_t h = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(element) >> 2) * 2654435761u;
      return (h ^ (h >> 15)) & (Capacity - 1);
   }

   // False when the element is null or the set is full; an element already in
   // it takes the new vtable, drawable and ratio.
   bool add(const void* element, const void* red, uint32_t vtable, float ratio)
   {
      if (!element) return false;
      for (unsigned i = slot_of(element), n = 0; n < Capacity; i = (i + 1) & (Capacity - 1), ++n) {
         if (entries[i].element == element) {
            entries[i] = { element, red, vtable, ratio };
            return true;
         }
         if (!entries[i].element) {
            if (4 * (count + 1) > 3 * Capacity) return false;
            entries[i] = { element, red, vtable, ratio };
            ++count;
            return true;
         }
      }
      return false;
   }

   const Entry* find(const void* element) const
   {
      if (!element || !count) return nullptr;
      for (unsigned i = slot_of(element), n = 0; n < Capacity; i = (i + 1) & (Capacity - 1), ++n) {
         if (entries[i].element == element) return &entries[i];
         if (!entries[i].element) return nullptr;
      }
      return nullptr;
   }

   void clear()
   {
      std::memset(entries, 0, sizeof(entries));
      count = 0;
   }
};

} // namespace hud_true_widescreen
