#pragma once

#include <cstdint>
#include <cstddef>

namespace game {

// Partial - force-lock fields only
struct LockOnManager {
    char      _pad0[0x928];
    void*     mCurrentTarget;       // +0x928
    uint32_t  mCurrentHandleId;     // +0x92C
    char      _pad1[0x934 - 0x930];
    void*     mForceTarget;         // +0x934
    uint32_t  mForceHandleId;       // +0x938
    char      _pad2[0x940 - 0x93C];
    int       mForceLock;           // +0x940 - set to 5 to activate
};
static_assert(offsetof(LockOnManager, mCurrentTarget) == 0x928);
static_assert(offsetof(LockOnManager, mForceTarget) == 0x934);
static_assert(offsetof(LockOnManager, mForceLock) == 0x940);

} // namespace game
