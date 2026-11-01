#pragma once

#include "core/build_field.hpp"

// =============================================================================
// OrdnanceSticky - ClassLabel "sticky" (also the base of haywire and beacon).
// Derives from OrdnanceShell, NOT OrdnanceGrenade, and has its own Render.
//
//   field                         modtools @                  Steam / GOG @
//   OrdnanceStickyClass::
//     mAlignVertical              0x1AD  Render 0x616B96      0x139  Render 0x6063D3 / 0x607463
//                                                                    SetProperty 0x607142 (Steam)
//   OrdnanceSticky::
//     mStickInfo                  0x130  Render 0x616AF2      0x130  Render 0x6062E0 / 0x607370
//
// mStickInfo is set while stuck to something; Render then draws on the stuck
// pose and never reads the velocity.
// =============================================================================

namespace layout::OrdnanceStickyClass {

inline constexpr Field<bool> mAlignVertical{0x1AD, 0x139};

} // namespace layout::OrdnanceStickyClass

namespace layout::OrdnanceSticky {

inline constexpr Field<void*> mStickInfo{0x130};

} // namespace layout::OrdnanceSticky
