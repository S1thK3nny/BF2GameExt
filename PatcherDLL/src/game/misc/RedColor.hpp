#pragma once

#include <cstdint>

namespace game {

// -------------------------------------------------------------------------
// RedColor (BGRA byte order)
// -------------------------------------------------------------------------

struct RedColor {
    uint8_t b, g, r, a;
};
static_assert(sizeof(RedColor) == 0x04);

} // namespace game
