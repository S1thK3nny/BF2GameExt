// Standalone test of the TrueWidescreen placement maths: the 4:3 layout, the
// edge each piece keeps, ScreenAnchor's names and shares, the slide,
// world-tracking positions and the parent matrix, checked through the
// interface camera the way the engine projects; and the set of elements
// opted-in files made, which gives the HUD editor's writer their 4:3 width.
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

// AuthoredRatio: a file laid out for its own screen shape. A 4:3 file only on a
// wide screen (a 4:3 one already shows it as written); any other on every
// screen, wider or narrower, where each piece keeps its distance from its edge.
void authored_ratios()
{
   const float r169 = 16.0f / 9.0f, r219 = 21.0f / 9.0f, r1610 = 16.0f / 10.0f;
   assert(near(layout_width(720, r169), 1280) && near(layout_width(1080, r169), 1920));
   assert(near(layout_width(720), 960) && near(layout_width(720, kFourThree), 960));
   assert(valid_ratio(1.0f) && valid_ratio(kFourThree) && valid_ratio(r219) && valid_ratio(32.0f / 9.0f));
   assert(!valid_ratio(0.5f) && !valid_ratio(4.5f) && !valid_ratio(0.0f));

   // Which screens lay a file out.
   assert(laid_out_on(1280, 720, kFourThree) && laid_out_on(2560, 1080, kFourThree));
   assert(!laid_out_on(1024, 768, kFourThree) && !laid_out_on(1280, 1024, kFourThree));
   assert(laid_out_on(1280, 720, r169) && laid_out_on(1024, 768, r169) && laid_out_on(1680, 1050, r169));
   assert(laid_out_on(1280, 1024, r169) && laid_out_on(2560, 1080, r169));
   assert(laid_out_on(1024, 768, 4.0f / 3.0001f) == laid_out_on(1024, 768, kFourThree));   // still 4:3
   assert(!laid_out_on(0, 720, r169) && !laid_out_on(1280, 0, r169) && !laid_out_on(1280, 720, 0.5f));

   // A 16:9 file on its own screen: every piece where it is written.
   {
      const float lw = layout_width(720, r169);
      for (Anchor a : { Anchor::Left, Anchor::Center, Anchor::Right }) assert(near(slide_for(a, 1280, lw), 0));
   }

   // The same file elsewhere keeps each piece's distance from its edge, out on
   // a wider screen and in on a narrower one; the centre stays the centre.
   const float screens[][2] = { { 960, 720 }, { 1024, 768 }, { 1680, 1050 }, { 1280, 720 }, { 2560, 1080 },
                                { 3440, 1440 }, { 1280, 1024 } };
   for (float ratio : { kFourThree, r1610, r169, r219 }) {
      for (const auto& s : screens) {
         const float w = s[0], h = s[1], lw = layout_width(h, ratio);
         const float left = 0.05f * lw, middle = 0.5f * lw, right = 0.93f * lw;
         assert(anchor_for(left, lw) == Anchor::Left && anchor_for(middle, lw) == Anchor::Center &&
                anchor_for(right, lw) == Anchor::Right);
         assert(near(left + slide_for(Anchor::Left, w, lw), left));
         assert(near(middle + slide_for(Anchor::Center, w, lw), 0.5f * w));
         assert(near(w - (right + slide_for(Anchor::Right, w, lw)), lw - right));
      }
   }

   // 16:9 on 4:3 at 720 high: the right third moves in by 320 pixels.
   const float lw = layout_width(720, r169);
   assert(near(slide_for(Anchor::Right, 960, lw), -320) && near(slide_for(Anchor::Center, 960, lw), -160));

   // World-following positions still land on their point when pieces move in.
   for (float slide : { 0.0f, slide_for(Anchor::Center, 960, lw), slide_for(Anchor::Right, 960, lw) })
      for (float f = -0.25f; f <= 1.25f; f += 0.125f)
         assert(near(tracked_fraction(f, 960, lw, slide) * lw + slide, f * 960, 1e-2f));
}

// ScreenAnchor: a named edge or a share of the width beyond the layout, the
// same slides as the thirds give for the three edges, in and out.
void screen_anchors()
{
   Anchor a = Anchor::Center;
   assert(anchor_named("Left", a) && a == Anchor::Left);
   assert(anchor_named("center", a) && a == Anchor::Center);
   assert(anchor_named("RIGHT", a) && a == Anchor::Right);
   assert(!anchor_named("Centre", a) && !anchor_named("", a) && !anchor_named("Lef", a) && !anchor_named("Lefts", a));
   assert(!anchor_named(nullptr, a));
   for (Anchor e : { Anchor::Left, Anchor::Center, Anchor::Right }) {
      assert(anchor_named(anchor_name(e), a) && a == e);
      assert(named_anchor(share_of(e)) == e);   // the editor writes back the name it read
   }
   assert(share_of(Anchor::Left) == 0.0f && share_of(Anchor::Center) == 0.5f && share_of(Anchor::Right) == 1.0f);

   // The HUD editor's list: none, then 0 to 1 in steps of 0.05, the edges by name.
   assert(anchor_index(false, 0.7f) == 0 && anchor_index(true, 0.0f) == 1 && anchor_index(true, 0.5f) == 11 &&
          anchor_index(true, 1.0f) == kAnchorChoices - 1);
   for (int i = 1; i < kAnchorChoices; ++i) {
      assert(anchor_index(true, anchor_share(i)) == i);
      assert(anchor_index_named(i) == (i == 1 || i == 11 || i == 21));
      if (anchor_index_named(i)) assert(anchor_index(true, share_of(named_anchor(anchor_share(i)))) == i);
   }
   assert(near(anchor_share(7), 0.3f) && near(anchor_share(20), 0.95f));
   assert(anchor_index(true, 0.33f) == 8 && anchor_index(true, -0.2f) == 1 && anchor_index(true, 1.4f) == 21);
   assert(valid_share(0.0f) && valid_share(0.25f) && valid_share(1.0f));
   assert(!valid_share(-0.01f) && !valid_share(1.01f) && !valid_share(std::nanf("")));

   const float r169 = 16.0f / 9.0f;
   const float screens[][2] = { { 960, 720 }, { 1280, 720 }, { 1680, 1050 }, { 2560, 1080 }, { 3440, 1440 } };
   for (float ratio : { kFourThree, r169 }) {
      for (const auto& s : screens) {
         const float w = s[0], h = s[1], lw = layout_width(h, ratio);
         for (Anchor e : { Anchor::Left, Anchor::Center, Anchor::Right })
            assert(near(slide_for_share(share_of(e), w, lw), slide_for(e, w, lw)));
         // A share equal to where a point of the piece sits, as a fraction of
         // the layout, keeps that point at the same fraction of every screen.
         for (float f : { 0.0f, 0.25f, 0.3f, 0.62f, 1.0f })
            assert(near(f * lw + slide_for_share(f, w, lw), f * w, 1e-2f));
      }
   }

   // The bands a 4:3 file leaves at 16:9 (25% to 37.5% of the screen), where
   // no third holds a piece through a reload, hold an anchored one: a piece
   // meant for 30% of a 1280 x 720 screen, 384 pixels in, written at x = 288 in
   // the 960-wide layout with ScreenAnchor(0.3), is drawn at 384. The thirds
   // would keep it to the left, at 288.
   const float lw = layout_width(720);
   const float x = 0.3f * 1280 - slide_for_share(0.3f, 1280, lw);
   assert(near(x, 288) && anchor_for(x, lw) == Anchor::Left);
   assert(near(x + slide_for_share(0.3f, 1280, lw), 384));
   assert(!near(x + slide_for(anchor_for(x, lw), 1280, lw), 384));   // the thirds would put it at 288
}

// One rule for where a load puts a piece and where the HUD editor puts one it
// moves, so what the editor shows is what the next load gives.
void placements()
{
   const float w = 1280, lw = layout_width(720);   // a 4:3 file at 16:9: 320 pixels spare
   const float right = 1.0f, quarter = 0.25f;

   // A top-level group at the corner holds pieces, unless it has an anchor.
   Placement p = placement(0, 0, true, nullptr, w, lw);
   assert(p.container && p.slide == 0.0f);
   p = placement(0, 0, true, &right, w, lw);
   assert(!p.container && near(p.slide, 320));
   // A child at its container's corner is a piece of its own, kept left.
   p = placement(0, 0, false, nullptr, w, lw);
   assert(!p.container && near(p.slide, 0));
   // Only both at 0 make the corner.
   assert(!placement(0, 10, true, nullptr, w, lw).container && !placement(10, 0, true, nullptr, w, lw).container);

   // Off the corner, by its third or its anchor, at the top or in a container.
   for (bool top : { true, false }) {
      assert(near(placement(100, 50, top, nullptr, w, lw).slide, 0));
      assert(near(placement(480, 50, top, nullptr, w, lw).slide, 160));
      assert(near(placement(900, 50, top, nullptr, w, lw).slide, 320));
      assert(near(placement(100, 50, top, &right, w, lw).slide, 320));
      assert(near(placement(900, 50, top, &quarter, w, lw).slide, 80));
      assert(!placement(480, 50, top, nullptr, w, lw).container);
   }

   // Dragged across the first third line, an unanchored piece jumps by half the
   // spare width, as it would on the next load; an anchored one does not.
   assert(near(placement(319, 50, true, nullptr, w, lw).slide, 0));
   assert(near(placement(321, 50, true, nullptr, w, lw).slide, 160));
   assert(near(placement(319, 50, true, &quarter, w, lw).slide, placement(321, 50, true, &quarter, w, lw).slide));

   // At the file's own ratio nothing moves, wherever it is dragged.
   const float lw169 = layout_width(720, 16.0f / 9.0f);
   for (float x : { 0.0f, 300.0f, 640.0f, 1000.0f, 1280.0f })
      assert(placement(x, 50, true, nullptr, 1280, lw169).slide == 0.0f);
}

const void* address(uintptr_t at)
{
   return reinterpret_cast<const void*>(at);
}

// The elements opted-in files made: found by address with what they were made
// as, filled to three quarters, an element added again replaced in place, and
// a rebuild from the ones kept, as prune_made does.
void made_set()
{
   static MadeSet<64> set;
   set.clear();
   assert(set.count == 0 && !set.find(address(0x10000000)) && !set.find(nullptr) &&
          !set.add(nullptr, nullptr, 0, kFourThree));

   // Element-sized strides, and ones that share their low bits, all land.
   for (uintptr_t stride : { uintptr_t(0x220), uintptr_t(0x1000), uintptr_t(0x10000) }) {
      set.clear();
      for (int i = 0; i < 48; ++i)
         assert(set.add(address(0x10000000 + i * stride), address(0x20000000 + i), 0x00A5C754u + i,
                        i % 2 ? kFourThree : 16.0f / 9.0f));
      assert(set.count == 48);
      assert(!set.add(address(0x10000000 + 48 * stride), nullptr, 0, kFourThree));   // past three quarters
      for (int i = 0; i < 48; ++i) {
         const auto* e = set.find(address(0x10000000 + i * stride));
         assert(e && e->red == address(0x20000000 + i) && e->vtable == 0x00A5C754u + i &&
                e->ratio == (i % 2 ? kFourThree : 16.0f / 9.0f));
         assert(!set.find(address(0x10000000 + i * stride + 4)));
      }
      // Added again, even when full: replaced, not added.
      assert(set.add(address(0x10000000 + 7 * stride), address(0x30000000), 0x1234u, 21.0f / 9.0f));
      assert(set.count == 48 && set.find(address(0x10000000 + 7 * stride))->vtable == 0x1234u &&
             set.find(address(0x10000000 + 7 * stride))->ratio == 21.0f / 9.0f);
   }

   // A rebuild keeps exactly the ones carried over.
   static MadeSet<64> kept;
   kept.clear();
   for (int i = 0; i < 48; i += 2) {
      const auto* e = set.find(address(0x10000000 + i * 0x10000));
      assert(e && kept.add(e->element, e->red, e->vtable, e->ratio));
   }
   set = kept;
   assert(set.count == 24);
   for (int i = 0; i < 48; ++i)
      assert((set.find(address(0x10000000 + i * 0x10000)) != nullptr) == (i % 2 == 0));

   // The size GameExt uses holds a whole HUD's worth.
   static MadeSet<4096> big;
   big.clear();
   for (int i = 0; i < 3072; ++i) assert(big.add(address(0x08000000 + i * 0x220), address(i), 1, kFourThree));
   assert(!big.add(address(0x08000000 + 3072 * 0x220), address(0), 1, kFourThree));
   for (int i = 0; i < 3072; ++i) assert(big.find(address(0x08000000 + i * 0x220)));
   big.clear();
   assert(big.count == 0 && !big.find(address(0x08000000)));
}

} // namespace

int main()
{
   layout_and_anchors();
   tracking();
   parent_matrix();
   stock_mapping();
   authored_ratios();
   screen_anchors();
   placements();
   made_set();
   std::printf("hud true widescreen: layout, anchors, tracking, parent matrix, the stock mapping, authored "
               "ratios on wider and narrower screens, ScreenAnchor, placement on load and in the editor, and the "
               "elements opted-in files made\n");
   return 0;
}
