#pragma once

#include <cstdint>
#include <cstddef>

#include "../pbl/PblMatrix.hpp"

namespace game {

struct RedCamera {
    char       _pad0[0x30];
    PblMatrix  mMatrix;         // +0x30 - 4x4 row-major view matrix
    char       _pad1[0x140 - 0x70];
    float      mZoom;           // +0x140
    float      mTanHalfFOVWidth;// +0x144
};
static_assert(offsetof(RedCamera, mMatrix) == 0x30);
static_assert(offsetof(RedCamera, mZoom) == 0x140);
static_assert(offsetof(RedCamera, mTanHalfFOVWidth) == 0x144);

} // namespace game
