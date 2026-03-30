#pragma once

#include <cstdint>
#include <cstddef>

namespace game {

struct Controllable;  // forward declaration

struct PlayerController {
    char          _pad0[0x04];
    Controllable* mOwner;     // +0x04
};
static_assert(offsetof(PlayerController, mOwner) == 0x04);

} // namespace game
