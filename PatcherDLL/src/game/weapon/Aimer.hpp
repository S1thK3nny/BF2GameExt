#pragma once

#include <cstdint>
#include <cstddef>

#include "../pbl/PblVector3.hpp"

namespace game {

// Referenced from Weapon+0x70
struct Aimer {
    char       _pad0[0x70];
    PblVector3 mRootPos;    // +0x70
    char       _pad1[0x88 - 0x7C];
    PblVector3 mFirePos;    // +0x88
};
static_assert(offsetof(Aimer, mRootPos) == 0x70);
static_assert(offsetof(Aimer, mFirePos) == 0x88);

} // namespace game
