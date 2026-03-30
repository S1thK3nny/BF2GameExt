#pragma once

#include <cstdint>
#include <cstddef>

namespace game {

struct PblQuaternion {
    float x, y, z, w;
};
static_assert(sizeof(PblQuaternion) == 0x10);

} // namespace game
