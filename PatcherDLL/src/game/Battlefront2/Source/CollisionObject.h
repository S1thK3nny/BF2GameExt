#pragma once

#include <stdint.h>

// =============================================================================
// CollisionObject and CollisionResult: what a CollisionCallback is handed, the
// parts GameExt reads. The same on every build. Read off the hover's callback,
// EntityHover::CollisionCallback (modtools 0x005155B0, Steam and GOG
// 0x004C66A0), which works out the other object's type and the closing speed
// this way itself:
//
//                         modtools            Steam
//   the other's type      0x5155BB-0x5155CD   0x4C66AD-0x4C66BD   [other + 4], then
//                                             [stack + idx * 4 + 4] or [other + 0x20]
//   its own, the same way 0x5155D0-0x5155E0   0x4C66C0-0x4C66D0
//   other's below its own 0x5155E3           0x4C66D3            CMP / JL past it all
//   its GameObject        0x51560B            0x4C66FD            CollisionObject vtable +0x38
//   that one's velocity   0x51561D            0x4C6713            GameObject vtable +0x44
//   the contact normal    0x515657            0x4C676D            LEA [result + 0xC]
//
// A CollisionObject's TreeGridObject part sits at +4. Its type is in its
// TreeGridStack's mData row when it has a stack, else in its own mData[0]. The
// hover's callback handles a contact only when the other's type is at least
// its own, and then never a soldier's (COLL_SOFT).
// mSeparationNormal points out of what was hit, toward the object whose
// callback runs: a contact is closing while that object's velocity against
// the other, dotted with the normal, is below 0.
// =============================================================================

namespace layout::CollisionObject {

constexpr uint32_t kTreeGrid         = 0x04;   // TreeGridObject part
constexpr uint32_t kVt_GetGameObject = 0x38;   // GameObject*, or null
constexpr int      kTypeSoft         = 2;      // CollisionObjectType COLL_SOFT: soldiers

} // namespace layout::CollisionObject

namespace layout::TreeGridObject {

constexpr uint32_t kStackPtr = 0x00;           // TreeGridStack*, or null
constexpr uint32_t kStackIdx = 0x04;
constexpr uint32_t kData     = 0x1C;           // int mData[1]: the type, with no stack

} // namespace layout::TreeGridObject

namespace layout::TreeGridStack {

constexpr uint32_t kData = 0x04;               // int mData[1][15]: entry i's type at + 4 * i

} // namespace layout::TreeGridStack

namespace layout::CollisionResult {

constexpr uint32_t kSeparationNormal = 0x0C;   // PblVector3

} // namespace layout::CollisionResult
