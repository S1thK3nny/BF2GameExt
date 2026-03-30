#pragma once

#include <cstdint>
#include <cstddef>

#include "../pbl/PblVector3.hpp"
#include "../pbl/PblMatrix.hpp"

namespace game {

// -------------------------------------------------------------------------
// CamPathInfo (from PDB)
// -------------------------------------------------------------------------

struct CamPathInfo {
    PblVector3 Vec[6];       // +0x00   (72 bytes)
    PblVector3 Slope[6];     // +0x48   (72 bytes)
    PblVector3 Ore[6];       // +0x90   (72 bytes)
    PblVector3 RSlope[6];    // +0xD8   (72 bytes)
    float      Time[6];      // +0x120  (24 bytes)
    uint32_t   numPoints;    // +0x138
};
static_assert(sizeof(CamPathInfo) == 0x13C);
static_assert(offsetof(CamPathInfo, Slope) == 0x48);
static_assert(offsetof(CamPathInfo, Time) == 0x120);
static_assert(offsetof(CamPathInfo, numPoints) == 0x138);

// -------------------------------------------------------------------------
// PilotType enum
// -------------------------------------------------------------------------

enum PilotType : int {
    PILOT_NONE      = 0,
    PILOT_DRIVER    = 1,
    PILOT_GUNNER    = 2,
    PILOT_PASSENGER = 3,
};

// -------------------------------------------------------------------------
// PilotDeathType enum
// -------------------------------------------------------------------------

enum PilotDeathType : int {
    PILOT_DEATH_NONE    = 0,
    PILOT_DEATH_NORMAL  = 1,
    PILOT_DEATH_RAGDOLL = 2,
};

// -------------------------------------------------------------------------
// ControllableClass
// -------------------------------------------------------------------------
// Total: 0x1B8 bytes

struct ControllableClass {
    bool           mCombatInterrupts;          // +0x00
    bool           mIgnoreHintNodes;           // +0x01
    char           _pad02[0x02];               // +0x02
    float          mShakeScale;                // +0x04
    CamPathInfo    mBobPath;                   // +0x08
    float          mCockpitTension;            // +0x144
    uint32_t       mLocalNameID;               // +0x148
    uint32_t       mForceMode;                 // +0x14C
    float          mFirstPersonFOV;            // +0x150
    float          mThirdPersonFOV;            // +0x154
    uint32_t       mPilotParentNodeHash;       // +0x158
    char           _pad15C[0x04];              // +0x15C
    PblMatrix      mPilotOffsetMatrix;         // +0x160
    uint32_t       mPilotAnimation;            // +0x1A0  ANIMATION hash
    bool           mAnimatedPilotNodeFlag;     // +0x1A4
    char           _pad1A5[0x03];              // +0x1A5
    PilotDeathType mPilotDeathType;            // +0x1A8
    PilotType      mPilotType;                 // +0x1AC
    bool           mIsPilotExposed;            // +0x1B0
    bool           mIsPilotAnimation9Pose;     // +0x1B1
    bool           mCanCapturePosts;           // +0x1B2
    bool           mCanEnterVehicles;          // +0x1B3
    bool           mControlsUnit;              // +0x1B4
    bool           mAnimatesUnitAsTurret;      // +0x1B5
    char           _pad1B6[0x02];              // +0x1B6
};
static_assert(sizeof(ControllableClass) == 0x1B8);
static_assert(offsetof(ControllableClass, mShakeScale) == 0x04);
static_assert(offsetof(ControllableClass, mBobPath) == 0x08);
static_assert(offsetof(ControllableClass, mCockpitTension) == 0x144);
static_assert(offsetof(ControllableClass, mLocalNameID) == 0x148);
static_assert(offsetof(ControllableClass, mFirstPersonFOV) == 0x150);
static_assert(offsetof(ControllableClass, mPilotOffsetMatrix) == 0x160);
static_assert(offsetof(ControllableClass, mPilotAnimation) == 0x1A0);
static_assert(offsetof(ControllableClass, mPilotDeathType) == 0x1A8);
static_assert(offsetof(ControllableClass, mPilotType) == 0x1AC);
static_assert(offsetof(ControllableClass, mIsPilotExposed) == 0x1B0);
static_assert(offsetof(ControllableClass, mControlsUnit) == 0x1B4);

} // namespace game
