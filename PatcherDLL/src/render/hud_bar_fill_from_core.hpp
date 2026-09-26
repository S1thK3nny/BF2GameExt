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

// ElementBarBitmap::SetValue's geometry for a value, with the default
// ScaleTexture and rectangle-scaling flags both on.
constexpr Bar fill(const Bar& b, float value)
{
   Bar out = b;
   out.rect.right = b.rect.left + value * b.width;
   out.uv.u1      = b.uv.u0 + value * b.spanU;
   return out;
}

} // namespace hud_bar_fill_from
