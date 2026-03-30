#pragma once

#include <cstdint>
#include <cstddef>

#include "../pbl/PblSphere.hpp"
#include "TreeGridStack.hpp"

namespace game {

// Spatial partitioning node. Embedded in Entity at +0x0C.
// Position read via mStackPtr/mStackIdx or fallback to inline mCollisionSphere.
struct TreeGridObject {
    TreeGridStack* mStackPtr;          // +0x00
    int        mStackIdx;              // +0x04
    PblSphere  mCollisionSphere;       // +0x08  (position + radius)
    float      m2dRadius;              // +0x18
    int        mData[1];               // +0x1C
    int        mTreeGridCellIndex;     // +0x20
};
static_assert(offsetof(TreeGridObject, mStackPtr) == 0x00);
static_assert(offsetof(TreeGridObject, mStackIdx) == 0x04);
static_assert(offsetof(TreeGridObject, mCollisionSphere) == 0x08);
static_assert(offsetof(TreeGridObject, m2dRadius) == 0x18);
static_assert(offsetof(TreeGridObject, mTreeGridCellIndex) == 0x20);
static_assert(sizeof(TreeGridObject) == 0x24);

} // namespace game
