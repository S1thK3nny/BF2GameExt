#pragma once

#include <cstdint>
#include <cstddef>

namespace game {

struct PblVector3 {
    float x, y, z;
};
static_assert(sizeof(PblVector3) == 0x0C);

} // namespace game
