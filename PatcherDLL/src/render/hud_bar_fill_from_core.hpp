#pragma once

// =============================================================================
// FillFrom("Right") for HUD BarBitmaps as pure arithmetic, shared with
// tests/hud_bar_fill_from_tests.cpp.
//
// The stock fill, ElementBarBitmap::SetValue, keeps the rectangle's left edge
// and the texture's left U, and moves the other end:
//     right = left + value * width        u1 = u0 + value * spanU
// PostReadSetup stored width = right - left, but spanU = u1, the right-hand U
// itself, which is only the span while u0 is 0.
//
// Storing the bar end for end, rectangle and U range swapped with a negative
// width and span, makes that same fill keep the right edge instead. The texture
// keeps its mapping (right U at the right edge), so the pixel shown at any
// point is the one the full bar shows there, whatever u0 is.
// =============================================================================

namespace hud_bar_fill_from {

struct Rect { float left, top, right, bottom; };
struct TexCoords { float u0, v0, u1, v1; };

// What the bar holds: the drawn rectangle and coordinates, plus the stored
// width and U span the fill multiplies by the value.
struct Bar {
   Rect      rect;
   TexCoords uv;
   float     width;
   float     spanU;
};

// The state stock PostReadSetup leaves.
constexpr Bar stock(const Rect& r, const TexCoords& t)
{
   return { r, t, r.right - r.left, t.u1 };
}

// The same bar stored so the stock fill anchors on its right end.
constexpr Bar anchor_right(const Rect& r, const TexCoords& t)
{
   return { { r.right, r.top, r.left, r.bottom }, { t.u1, t.v0, t.u0, t.v1 },
            r.left - r.right, t.u0 - t.u1 };
}

// ElementBarBitmap::SetValue's geometry for a value: the rectangle-scaling
// flag (ScaleSize) moves the edge, ScaleTexture crops U.
constexpr Bar fill(const Bar& b, float value, bool scaleTexture, bool scaleSize)
{
   Bar out = b;
   if (scaleSize)    out.rect.right = b.rect.left + value * b.width;
   if (scaleTexture) out.uv.u1      = b.uv.u0 + value * b.spanU;
   return out;
}

// The same with both flags on, their defaults.
constexpr Bar fill(const Bar& b, float value)
{
   return fill(b, value, true, true);
}

// FillFrom("Bottom") and FillFrom("Top"). The stock fill can only move the
// right edge, so a vertical bar has its ScaleTexture and rectangle-scaling
// flags cleared (the stock fill then leaves it alone) and is laid out from the
// full rectangle and coordinates it had after setup: the anchored edge stays,
// the other one moves with the value, and V is cropped to match so each point
// keeps the full bar's pixel. With ScaleTexture off the whole texture is
// squeezed into the moving quad instead, as the stock fill does.
struct Vertical {
   Rect      full;
   TexCoords fullUV;
   bool      fromTop;   // anchor the top edge; otherwise the bottom
   bool      crop;      // ScaleTexture as authored
};

constexpr float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// The rectangle and coordinates for a value: the full ones with only the
// moving edge and its V changed.
constexpr Bar fill_vertical(const Vertical& b, float value)
{
   const float v = clamp01(value);
   const Rect& r = b.full;
   const TexCoords& t = b.fullUV;
   Bar out = { r, t, 0, 0 };
   if (b.fromTop) {
      out.rect.bottom = r.top + v * (r.bottom - r.top);
      if (b.crop) out.uv.v1 = t.v0 + v * (t.v1 - t.v0);
   } else {
      out.rect.top = r.bottom + v * (r.top - r.bottom);
      if (b.crop) out.uv.v0 = t.v1 + v * (t.v0 - t.v1);
   }
   return out;
}

} // namespace hud_bar_fill_from
