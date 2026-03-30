#pragma once

#include <cstdint>
#include <cstddef>

namespace game {

// PblHandle - typed pointer + generation counter for stale-detection
struct PblHandle {
    void*    mObject;        // +0x00 - raw pointer (Entity*, GameObject*, etc.)
    uint32_t mSavedHandleId; // +0x04 - generation counter
};
static_assert(sizeof(PblHandle) == 0x08);

} // namespace game
