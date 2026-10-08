// Standalone tests (not a DLL build). Compile with /std:c++17 /W4 /WX, no NDEBUG.
#include "../PatcherDLL/src/render/hud_bar_fill_from_core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace hud_bar_fill_from;

namespace {

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// The U the drawn quad shows at x: its two edges carry u0 and u1.
float u_at(const Bar& b, float x)
{
   const float t = (x - b.rect.left) / (b.rect.right - b.rect.left);
   return b.uv.u0 + t * (b.uv.u1 - b.uv.u0);
}

// The U the full, unfilled bar shows at x.
float full_u_at(const Rect& r, const TexCoords& t, float x)
{
   return t.u0 + (x - r.left) / (r.right - r.left) * (t.u1 - t.u0);
}

} // namespace

int main()
{
   const Rect rects[] = { {-96, -64, 96, 64}, {0, 0, 192, 128}, {-192, 10, 0, 40} };
   const TexCoords uvs[] = { {0, 0, 1, 1}, {0, 0.5f, 1, 1}, {0, 1, 1, 0}, {0.25f, 0, 0.75f, 1},
                             {1, 0, 0, 1} };

   for (const Rect& r : rects)
      for (const TexCoords& t : uvs)
         for (int step = 0; step <= 20; ++step) {
            const float value = step / 20.0f;
            const Bar b = fill(anchor_right(r, t), value);

            // The right edge stays put and the bar covers the right-hand
            // `value` of its box.
            assert(near(b.rect.left, r.right));
            assert(near(b.rect.right, r.right - value * (r.right - r.left)));
            assert(b.rect.top == r.top && b.rect.bottom == r.bottom);
            assert(b.uv.v0 == t.v0 && b.uv.v1 == t.v1);
            // Every point shows exactly what the full bar shows there.
            if (value > 0)
               for (int k = 0; k <= 8; ++k) {
                  const float x = b.rect.left + (b.rect.right - b.rect.left) * k / 8.0f;
                  assert(near(u_at(b, x), full_u_at(r, t, x)));
               }
         }

   // A stock bar at the health and a FillFrom("Right") bar at the missing
   // health meet at the same point and show the same pixel there.
   const Rect r = {-96, -64, 96, 64};
   const TexCoords t = {0, 0, 1, 1};
   for (int step = 0; step <= 20; ++step) {
      const float health = step / 20.0f;
      const Bar left  = fill(stock(r, t), health);
      const Bar right = fill(anchor_right(r, t), 1.0f - health);
      assert(near(left.rect.right, right.rect.right));
      assert(near(left.uv.u1, right.uv.u1));
   }

   // The stock storage is only right while u0 is 0: TexCoords starting at 0.25
   // make a full stock bar reach U 1.0 instead of 0.75. FillFrom's storage has
   // no such limit, as checked above.
   const TexCoords offset = {0.25f, 0, 0.75f, 1};
   assert(near(fill(stock(r, offset), 1.0f).uv.u1, 1.0f));
   assert(near(fill(stock(r, t), 1.0f).uv.u1, 1.0f));

   // FillFrom("Bottom") and ("Top"): the anchored edge stays, the other moves
   // with the value, left/right and U never change, and every point shows the
   // pixel the full bar shows there, flipped V included.
   const TexCoords vuvs[] = { {0, 0, 1, 1}, {0, 0.25f, 1, 0.75f}, {0, 1, 1, 0}, {0.1f, 0.2f, 0.9f, 0.6f} };
   for (const Rect& full : rects)
      for (const TexCoords& uv : vuvs)
         for (int top = 0; top <= 1; ++top)
            for (int step = 0; step <= 20; ++step) {
               const float value = step / 20.0f;
               const Vertical layout = { full, uv, top == 1, true };
               const Bar b = fill_vertical(layout, value);
               assert(b.rect.left == full.left && b.rect.right == full.right);
               assert(b.uv.u0 == uv.u0 && b.uv.u1 == uv.u1);
               const float height = full.bottom - full.top;
               if (top) {
                  assert(b.rect.top == full.top && near(b.rect.bottom, full.top + value * height));
                  assert(b.uv.v0 == uv.v0);
               } else {
                  assert(b.rect.bottom == full.bottom && near(b.rect.top, full.bottom - value * height));
                  assert(b.uv.v1 == uv.v1);
               }
               if (value > 0)
                  for (int k = 0; k <= 8; ++k) {
                     const float y = b.rect.top + (b.rect.bottom - b.rect.top) * k / 8.0f;
                     const float shown = b.uv.v0 + (y - b.rect.top) / (b.rect.bottom - b.rect.top)
                                                   * (b.uv.v1 - b.uv.v0);
                     const float wanted = uv.v0 + (y - full.top) / height * (uv.v1 - uv.v0);
                     assert(near(shown, wanted));
                  }
            }

   // Values outside 0..1 clamp, as the stock bar's do.
   const Vertical up = { r, t, false, true };
   assert(near(fill_vertical(up, 1.5f).rect.top, r.top));
   assert(near(fill_vertical(up, -0.5f).rect.top, r.bottom));

   // ScaleTexture off squeezes the whole texture into the moving quad.
   const Vertical squeeze = { r, t, false, false };
   const Bar half = fill_vertical(squeeze, 0.5f);
   assert(near(half.rect.top, 0) && half.uv.v0 == t.v0 && half.uv.v1 == t.v1);

   // A Bottom bar at the health and a Top bar at the rest meet at one edge,
   // on the same pixel.
   for (int step = 0; step <= 20; ++step) {
      const float health = step / 20.0f;
      const Bar lower = fill_vertical({ r, t, false, true }, health);
      const Bar upper = fill_vertical({ r, t, true, true }, 1.0f - health);
      assert(near(lower.rect.top, upper.rect.bottom));
      assert(near(lower.uv.v0, upper.uv.v1));
   }

   // The stock fill's two flags, as a bar edited in the HUD editor is cropped
   // again: ScaleSize moves the edge, ScaleTexture crops U, each on its own.
   for (const TexCoords& uv : uvs) {
      const Bar full = anchor_right(r, uv);
      const Bar both = fill(full, 0.25f, true, true);
      const Bar edge = fill(full, 0.25f, false, true);
      const Bar crop = fill(full, 0.25f, true, false);
      const Bar none = fill(full, 0.25f, false, false);
      assert(near(both.rect.right, fill(full, 0.25f).rect.right) && near(both.uv.u1, fill(full, 0.25f).uv.u1));
      assert(near(edge.rect.right, both.rect.right) && edge.uv.u1 == full.uv.u1);
      assert(crop.rect.right == full.rect.right && near(crop.uv.u1, both.uv.u1));
      assert(none.rect.right == full.rect.right && none.uv.u1 == full.uv.u1);
      assert(edge.width == full.width && crop.spanU == full.spanU);
   }

   // The HUD editor switching a stock bar to another FillFrom: the bar as its
   // setup left it comes back from what it draws at any value, with either
   // flag, and fills to the same drawing again.
   for (const Rect& rect : rects)
      for (const TexCoords& uv : uvs)
         for (int flags = 0; flags < 4; ++flags)
            for (int step = 0; step <= 10; ++step) {
               const bool crop = (flags & 1) != 0, edge = (flags & 2) != 0;
               const Bar set = stock(rect, uv);
               const Bar drawn = fill(set, step / 10.0f, crop, edge);
               const Bar back = unfilled(drawn);
               assert(back.rect.left == rect.left && back.rect.top == rect.top && near(back.rect.right, rect.right) &&
                      back.rect.bottom == rect.bottom);
               assert(back.uv.u0 == uv.u0 && back.uv.v0 == uv.v0 && near(back.uv.u1, uv.u1) && back.uv.v1 == uv.v1);
               assert(back.width == set.width && back.spanU == set.spanU);
               const Bar again = fill(back, step / 10.0f, crop, edge);
               assert(near(again.rect.right, drawn.rect.right) && near(again.uv.u1, drawn.uv.u1));
            }

   std::puts("Bar FillFrom tests passed (right anchor, bottom and top, texture alignment, complementary "
             "pairs, U offsets and flips, the fill's two flags, a stock bar unfilled for the editor).");
}
