#pragma once

#include <cstdint>
#include <cstddef>

#include "../pbl/PblVector3.hpp"
#include "../pbl/PblQuaternion.hpp"

namespace game {

struct Controllable;  // forward declaration

// -------------------------------------------------------------------------
// Controller (base class for PlayerController, AIController, etc.)
// -------------------------------------------------------------------------
// vtable (4) + Controller_data (0x74)
// Total: 0x78 bytes

struct Controller {
    void*          vtable;                    // +0x00

    // --- Controller_data (starts at +0x04) ---
    Controllable*  mOwner;                    // +0x04
    PblVector3     mTargetBubbleLastVel;      // +0x08
    float          mTargetBubbleSize;         // +0x14
    float          mAvailableBubbleCharge;    // +0x18
    float          mTargetBubbleSize_cached;  // +0x1C
    PblVector3     mTargetBubbleAxis;         // +0x20
    PblQuaternion  mTargetBubbleRotation;     // +0x2C
    PblVector3     mTargetBubbleOffset;       // +0x3C
    float          mOpenRadius;               // +0x48
    float          mOpenRadiusTime;           // +0x4C
    float          mFoliageVisCrouch;         // +0x50
    float          mFoliageVisStand;          // +0x54
    float          mRegionVisCrouch;          // +0x58
    float          mRegionVisStand;           // +0x5C
    float          mRegionVisTime;            // +0x60
    float          mTotalVisFactor;           // +0x64
    bool           mForceAudible;             // +0x68
    char           _pad69[0x03];              // +0x69  (alignment)
    float          mCrowdTestTimer;           // +0x6C
    int            mCrowdEnemies;             // +0x70
    int            mCrowdAllies;              // +0x74
};
static_assert(sizeof(Controller) == 0x78);
static_assert(offsetof(Controller, mOwner) == 0x04);
static_assert(offsetof(Controller, mTargetBubbleLastVel) == 0x08);
static_assert(offsetof(Controller, mTargetBubbleSize) == 0x14);
static_assert(offsetof(Controller, mTargetBubbleRotation) == 0x2C);
static_assert(offsetof(Controller, mTargetBubbleOffset) == 0x3C);
static_assert(offsetof(Controller, mOpenRadius) == 0x48);
static_assert(offsetof(Controller, mForceAudible) == 0x68);
static_assert(offsetof(Controller, mCrowdTestTimer) == 0x6C);
static_assert(offsetof(Controller, mCrowdAllies) == 0x74);

} // namespace game
