#pragma once

#include <cstdint>
#include <cstddef>

namespace game {

struct PblMatrix {
    float m[4][4];  // row-major 4x4
};
static_assert(sizeof(PblMatrix) == 0x40);

} // namespace game
