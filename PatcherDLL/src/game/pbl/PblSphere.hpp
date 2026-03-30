#pragma once

#include "PblVector3.hpp"

namespace game {

struct PblSphere {
    PblVector3 mRenderPosition;  // +0x00
    float      mRenderRadius;    // +0x0C
};
static_assert(sizeof(PblSphere) == 0x10);

} // namespace game
