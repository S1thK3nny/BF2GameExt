#pragma once

#include "core/build_field.hpp"

// =============================================================================
// OrdnanceGrenadeClass - a thrown grenade's ODF class. Offsets are from the
// start of the class object, which the ordnance holds at +0x30 on every build.
//
//   field           modtools @                         Steam @
//   mAlignVertical  0x1A5  OrdnanceGrenade::Render 0x60FB8F   0x13D  Render 0x6001B0
//                          copy ctor             0x60FE60          ctor   0x6009C0
//
// The class ctor defaults mAlignVertical to true. GOG shares the Steam layout:
// Render compares byte for byte (0x00601240).
// =============================================================================

namespace layout::OrdnanceGrenadeClass {

inline constexpr Field<bool> mAlignVertical{0x1A5, 0x13D};

} // namespace layout::OrdnanceGrenadeClass
