// Standalone test of the TrueWidescreen placement maths: the 4:3 layout, the
// edge each piece keeps, the slide, world-tracking positions and the parent
// matrix, checked through the interface camera the way the engine projects.
#include "../PatcherDLL/src/render/hud_true_widescreen_core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace hud_true_widescreen;

namespace {

bool near(float a, float b, float tolerance = 1e-3f)
{
   return std::fabs(a - b) <= tolerance;
}

// The interface camera RedInterfaceScreen::Render sets up: SetFrustum(1, 10000,
// t, t * H / W), t = 2 tan 30. A world point on a plane parallel to the screen
// lands at pixel ((x / (z t / 2)) + 1) W / 2, (1 - y / (z t H / 2W)) H / 2.
const float kT = 2.0f * std::tan(3.14159265f / 6.0f);

void to_pixels(const float m[16], float lx, float ly, float width, float height, float& px, float& py)
{
   const float x = lx * m[0] + ly * m[4] + m[12];
   const float y = lx * m[1] + ly * m[5] + m[13];
   const float z = -(lx * m[2] + ly * m[6] + m[14]);   // distance in front of the camera
   const float halfW = z * kT * 0.5f;
   const float halfH = z * kT * (height / width) * 0.5f;
   px = (x / halfW + 1.0f) * 0.5f * width;
   py = (1.0f - y / halfH) * 0.5f * height;
}

void layout_and_anchors()
{
   assert(is_wide(1280, 720) && is_wide(1920, 1080) && is_wide(1680, 1050) && is_wide(2560, 1080));
   assert(!is_wide(1024, 768) && !is_wide(1280, 1024) && !is_wide(0, 720) && !is_wide(1280, 0));
   assert(near(layout_width(720), 960) && near(layout_width(1080), 1440));

   // Thirds of the layout, with the boundaries in the middle third.
   const float lw = layout_width(720);
   assert(anchor_for(0.054041f * lw, lw) == Anchor::Left);     // player1info_group
   assert(anchor_for(0.5f * lw, lw) == Anchor::Center);        // reticule, team scores
   assert(anchor_for(0.88f * lw, lw) == Anchor::Right);        // minimap
   assert(anchor_for(lw / 3.0f, lw) == Anchor::Center);
   assert(anchor_for(lw * 2.0f / 3.0f, lw) == Anchor::Center);
   assert(anchor_for(-50.0f, lw) == Anchor::Left);
   assert(anchor_for(lw + 50.0f, lw) == Anchor::Right);

   // 1280 x 720: 320 pixels spare.
   assert(near(slide_for(Anchor::Left, 1280, lw), 0));
   assert(near(slide_for(Anchor::Center, 1280, lw), 160));
   assert(near(slide_for(Anchor::Right, 1280, lw), 320));

   // Each piece keeps its 4:3 distance from its edge.
   const float health = 0.054041f * lw;
   assert(near(health + slide_for(Anchor::Left, 1280, lw), health));
   const float map = 0.88f * lw;
   assert(near(1280 - (map + slide_for(Anchor::Right, 1280, lw)), lw - map));
   const float middle = 0.5f * lw;
   assert(near(middle + slide_for(Anchor::Center, 1280, lw), 640));

   assert(is_origin(0, 0) && !is_origin(0, 1) && !is_origin(0.5f, 0));
}

// A world-tracking position lands on the real screen point whatever slide is
// above it: the engine multiplies the fraction by the layout width and the
// draw adds the slide.
void tracking()
{
   const float widths[] = { 1280, 1920, 1680, 2560, 3440 };
   const float heights[] = { 720, 1080, 1050, 1080, 1440 };
   for (int s = 0; s < 5; ++s) {
      const float w = widths[s], h = heights[s], lw = layout_width(h);
      for (float slide : { 0.0f, slide_for(Anchor::Center, w, lw), slide_for(Anchor::Right, w, lw) }) {
         for (float f = -0.25f; f <= 1.25f; f += 0.125f) {
            const float x = tracked_fraction(f, w, lw, slide) * lw + slide;
            assert(near(x, f * w, 1e-2f));
         }
      }
   }
   // The reticule's fixed 0.5 under the middle slide is the screen's centre.
   const float lw = layout_width(720);
   const float x = tracked_fraction(0.5f, 1280, lw, slide_for(Anchor::Center, 1280, lw)) * lw;
   assert(near(x, 480) && near(x + 160, 640));
}

// Through the interface camera, the parent maps a layout pixel to the same
// screen pixel (plus the slide), the same size across and down.
void parent_matrix()
{
   const float sizes[][2] = { { 1280, 720 }, { 1920, 1080 }, { 1680, 1050 }, { 2560, 1080 }, { 1024, 768 } };
   for (const auto& size : sizes) {
      const float w = size[0], h = size[1];
      for (float slide : { 0.0f, 160.0f, 427.5f }) {
         float m[16];
         pixel_parent(m, w, h, kT, slide);
         const float points[][2] = { { 0, 0 }, { 0.5f * w, 0.5f * h }, { 100, 37 }, { w, h }, { -20, h + 5 } };
         for (const auto& p : points) {
            float px, py;
            to_pixels(m, p[0], p[1], w, h, px, py);
            assert(near(px, p[0] + slide, 1e-2f) && near(py, p[1], 1e-2f));
         }
         // A 100 x 100 box stays square: no squash, no stretch.
         float x0, y0, x1, y1, x2, y2;
         to_pixels(m, 10, 10, w, h, x0, y0);
         to_pixels(m, 110, 10, w, h, x1, y1);
         to_pixels(m, 10, 110, w, h, x2, y2);
         assert(near(x1 - x0, 100, 1e-2f) && near(y2 - y0, 100, 1e-2f));
      }
   }
   // The plane is the one the stock HUD groups are drawn on.
   float m[16];
   pixel_parent(m, 1280, 720, kT, 0);
   assert(near(m[14], -640.0f / kT) && near(m[14], -554.256f, 1e-2f));
}

// The stock mapping this replaces, as measured in game at 1280 x 720 (the HUD
// diagnostic): x exact, y = 40 + 8/9 y. Bitmaps also get a 4/3 vertical scale,
// so a Pixels 128 x 108 bitmap came out square. Pinned so the comparison in the
// docs stays honest.
void stock_mapping()
{
   const float w = 1280, h = 720;
   const float r = 0.75f * w / h;                              // 4/3
   const float letterboxScale = 1.0f - (0.75f * w - h) / h;    // 2/3
   const float letterboxOffset = 0.5f * (0.75f * w - h);       // 120
   const auto stockY = [&](float y) {
      // virtual 640 x 480 mapping: screen y = H/2 - 0.375 W + r (s y + o)
      return h * 0.5f - 0.375f * w + r * (letterboxScale * y + letterboxOffset);
   };
   assert(near(stockY(0), 40) && near(stockY(720), 680));
   assert(near(stockY(100) - stockY(0), 100 * 8.0f / 9.0f));
   assert(near(108 * r * (stockY(1) - stockY(0)), 128));
}

} // namespace

int main()
{
   layout_and_anchors();
   tracking();
   parent_matrix();
   stock_mapping();
   std::printf("hud true widescreen: layout, anchors, tracking, parent matrix and the stock mapping\n");
   return 0;
}
