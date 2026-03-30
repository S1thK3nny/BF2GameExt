#pragma once

#include <cstdint>
#include <cstddef>

#include "../pbl/PblSphere.hpp"

namespace game {

struct TreeGridObject;  // forward declaration

struct TreeGridStack {
    int              mStackSize;       // +0x00
    int              mData[1][15];     // +0x04
    PblSphere        mSphere[15];      // +0x40
    TreeGridObject*  mObject[15];      // +0x130
    TreeGridStack*   mNext;            // +0x16C
};
static_assert(offsetof(TreeGridStack, mStackSize) == 0x00);
static_assert(offsetof(TreeGridStack, mData) == 0x04);
static_assert(offsetof(TreeGridStack, mSphere) == 0x40);
static_assert(offsetof(TreeGridStack, mObject) == 0x130);
static_assert(offsetof(TreeGridStack, mNext) == 0x16C);
static_assert(sizeof(TreeGridStack) == 0x170);

} // namespace game
