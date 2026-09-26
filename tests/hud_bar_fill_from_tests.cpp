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

   std::puts("Bar FillFrom tests passed (right anchor, texture alignment, complementary pair, U offsets and flips).");
}
