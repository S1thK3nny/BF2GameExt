#pragma once

#include <cstdint>
#include <cstddef>

#include "../pbl/PblVector3.hpp"
#include "../misc/RedColor.hpp"
#include "EnergyBar.hpp"

namespace game {

// -------------------------------------------------------------------------
// AmmoCounterClass
// -------------------------------------------------------------------------

struct AmmoCounterClass {
    int   mRoundsPerClip;  // +0x00
    float mAmmoPerRound;   // +0x04
};
static_assert(sizeof(AmmoCounterClass) == 0x08);

// -------------------------------------------------------------------------
// GameSound (Steam/GOG layout — 8 bytes)
// -------------------------------------------------------------------------
// Modtools is DIFFERENT: Node(8) + void* + uchar = larger.
// This causes all fields after the first GameSound to shift on modtools.

struct GameSound {
    void*   mHandle;  // +0x00  sound handle
    uint8_t mType;    // +0x04
    char    _pad[3];  // +0x05
};
static_assert(sizeof(GameSound) == 0x08);

} // namespace game

// =============================================================================
// Build-specific WeaponClass layouts
// =============================================================================
// The base fields (up to mAITargetTypes) are at the same offset across all
// builds. After that, GameSound size differences cause all subsequent fields
// to shift. Modtools GameSound is larger → all offsets after 0x118 are bigger.
//
// Key auto-aim fields:
//   Steam/GOG: mAutoPitchScreenDist at +0x1C0, mAutoTurnScreenDist at +0x1C4
//   Modtools:  mAutoPitchScreenDist at +0x298, mAutoTurnScreenDist at +0x29C

namespace game::steam {

// Full WeaponClass layout (Steam/GOG build)
// Header: vtable + base class = 0x20 bytes before WeaponClass_data.
// All offsets below are absolute from WeaponClass object base.
struct WeaponClass {
    // --- Header (vtable + base class) ---
    char               _header[0x20];              // +0x00

    // --- WeaponClass_data ---
    int                mSoldierAnimationWeapon;    // +0x20  WEAPON enum
    PblVector3         mFirePointOffset;           // +0x24
    char               mFilename[32];              // +0x30
    uint32_t*          mLabel;                     // +0x50
    uint32_t           mNameHash;                  // +0x54
    uint32_t           mHUDNameHash;               // +0x58
    float              mStrengthLimit;             // +0x5C
    void*              mNextCharge;                // +0x60  WeaponClass*
    void*              mModel;                     // +0x64  RedModel*
    void*              mHighResModel;              // +0x68  RedModel*
    uint32_t           mIconTexture;               // +0x6C
    uint32_t           mReticuleTexture;           // +0x70
    uint32_t           mScopeTexture;              // +0x74
    void*              mOrdnanceClass;             // +0x78  OrdnanceClass*
    float              mRoundsPerShot;             // +0x7C
    float              mHeatPerShot;               // +0x80
    float              mHeatRecoverRate;           // +0x84
    AmmoCounterClass   mAmmoCounterClass;          // +0x88
    EnergyBarClass     mEnergyBarClass;            // +0x90
    float              mReloadTime;                // +0x9C
    float              mZoomMin;                   // +0xA0
    float              mZoomMax;                   // +0xA4
    float              mZoomRate;                  // +0xA8
    float              mZoomTurnDivisorMin;        // +0xAC
    float              mZoomTurnDivisorMax;        // +0xB0

    // Recoil / charge (same offsets all builds)
    float              mRecoilStrengthHeavy;       // +0xB4
    float              mRecoilStrengthLight;       // +0xB8
    float              mRecoilLengthLight;         // +0xBC
    float              mRecoilLengthHeavy;         // +0xC0
    float              mRecoilDelayLight;          // +0xC4
    float              mRecoilDelayHeavy;          // +0xC8
    float              mRecoilDecayLight;          // +0xCC
    float              mRecoilDecayHeavy;          // +0xD0
    float              mChargeRateLight;           // +0xD4
    float              mChargeRateHeavy;           // +0xD8
    float              mMaxChargeStrengthHeavy;    // +0xDC
    float              mMaxChargeStrengthLight;    // +0xE0
    float              mChargeDelayLight;          // +0xE4
    float              mChargeDelayHeavy;          // +0xE8
    float              mTimeAtMaxCharge;           // +0xEC

    // Range / targeting (same offsets all builds)
    float              mExtremeRange;              // +0xF0
    float              mLockOnRange;               // +0xF4
    float              mLockOnAngle;               // +0xF8
    float              mLockOffAngle;              // +0xFC
    float              mMinRange;                  // +0x100
    float              mOptimalRange;              // +0x104
    float              mMaxRange;                  // +0x108
    int                mTargetSides;               // +0x10C
    int                mTargetTypes;               // +0x110
    int                mAITargetTypes;             // +0x114

    // --- DIVERGENCE POINT: GameSound size differs on modtools ---

    // Sounds (Steam: GameSound = 8 bytes)
    GameSound          mFireSound;                 // +0x118
    GameSound          mFireLoopSound;             // +0x120
    GameSound          mFireEmptySound;            // +0x128
    GameSound          mReloadSound;               // +0x130
    GameSound          mChargeSound;               // +0x138
    float              mChargeSoundPitch;          // +0x140
    GameSound          mChargedSound;              // +0x144
    GameSound          mWeaponChangeSound;         // +0x14C
    GameSound          mOverheatSound;             // +0x154
    float              mOverheatSoundPitch;        // +0x15C
    uint32_t           mFireSoundStop;             // +0x160
    GameSound          mFoleyFX[10];               // +0x164

    // Effects / auto-aim
    void*              mChargeUpEffect;            // +0x1B4  FLEffectClass*
    float              mFlashLength;               // +0x1B8
    float              mEnergyDrain;               // +0x1BC
    float              mAutoPitchScreenDist;       // +0x1C0  (was mAutoAimVert)
    float              mAutoTurnScreenDist;        // +0x1C4  (was mAutoAimHoriz)
    float              mTargetLockMaxDistance;      // +0x1C8
    float              mTargetLockMaxDistanceLose;  // +0x1CC
    int16_t            mScoreForMedalsType;        // +0x1D0
    int16_t            mMedalsTypeToUnlock;        // +0x1D2
    int16_t            mMedalsTypeToLock;           // +0x1D4
    char               _pad1D6[0x02];              // +0x1D6

    // Bitfields at +0x1D8
    // mFireAnim:2, mTriggerSingle:1, mZoomFirstPerson:1, mSniperScope:1,
    // mMaxRangeDefault:1, mInstantPlayFireAnim:1, mIsOffhand:1
    uint8_t            mWeaponFlags1;              // +0x1D8
    char               _pad1D9[0x03];              // +0x1D9

    // Bitfields at +0x1DC
    // mHashWeaponName:1, mDisplayRefire:1, mPostLoadInit:1,
    // mWarnedAboutLocalization:1, mReticuleInAimingOnly:1,
    // mNoFirstPersonFireAnim:1, mStopChargeSound:1, mAIUseBubbleCircle:1
    uint8_t            mWeaponFlags2;              // +0x1DC
    char               _pad1DD[0x03];              // +0x1DD

    // AI bubble
    float              mAIBubbleSizeMultiplier;    // +0x1E0
    float              mAIBubbleScaleDistDivider;  // +0x1E4
    float              mAIBubbleScaleClamp;        // +0x1E8

    // Muzzle flash
    void*              mMuzzleFlashModel;          // +0x1EC  RedModel*
    void*              mMuzzleFlashEffect;         // +0x1F0  FLEffectClass*
    float              mFlashRadius;               // +0x1F4
    float              mFlashWidth;                // +0x1F8
    RedColor           mFlashColor;                // +0x1FC
    RedColor           mFlashLightColor;           // +0x200
    float              mFlashLightRadius;          // +0x204
    float              mFlashLightDuration;        // +0x208

    // Barrage
    int                mBarrageMin;                // +0x20C
    int                mBarrageMax;                // +0x210
    float              mBarrageDelay;              // +0x214

    // Name
    char               mName[20];                  // +0x218
};

// Verify key offsets
static_assert(offsetof(WeaponClass, mSoldierAnimationWeapon) == 0x20);
static_assert(offsetof(WeaponClass, mFirePointOffset) == 0x24);
static_assert(offsetof(WeaponClass, mFilename) == 0x30);
static_assert(offsetof(WeaponClass, mOrdnanceClass) == 0x78);
static_assert(offsetof(WeaponClass, mRoundsPerShot) == 0x7C);
static_assert(offsetof(WeaponClass, mAmmoCounterClass) == 0x88);
static_assert(offsetof(WeaponClass, mEnergyBarClass) == 0x90);
static_assert(offsetof(WeaponClass, mReloadTime) == 0x9C);
static_assert(offsetof(WeaponClass, mRecoilStrengthHeavy) == 0xB4);
static_assert(offsetof(WeaponClass, mTimeAtMaxCharge) == 0xEC);
static_assert(offsetof(WeaponClass, mExtremeRange) == 0xF0);
static_assert(offsetof(WeaponClass, mAITargetTypes) == 0x114);
static_assert(offsetof(WeaponClass, mFireSound) == 0x118);
static_assert(offsetof(WeaponClass, mFoleyFX) == 0x164);
static_assert(offsetof(WeaponClass, mChargeUpEffect) == 0x1B4);
static_assert(offsetof(WeaponClass, mAutoPitchScreenDist) == 0x1C0);
static_assert(offsetof(WeaponClass, mAutoTurnScreenDist) == 0x1C4);
static_assert(offsetof(WeaponClass, mScoreForMedalsType) == 0x1D0);
static_assert(offsetof(WeaponClass, mAIBubbleSizeMultiplier) == 0x1E0);
static_assert(offsetof(WeaponClass, mMuzzleFlashModel) == 0x1EC);
static_assert(offsetof(WeaponClass, mFlashColor) == 0x1FC);
static_assert(offsetof(WeaponClass, mBarrageMin) == 0x20C);
static_assert(offsetof(WeaponClass, mName) == 0x218);

} // namespace game::steam

namespace game::gog {

using WeaponClass = game::steam::WeaponClass;

} // namespace game::gog

// Modtools WeaponClass: GameSound is larger (Node(8)+void*+uchar vs 8 bytes
// on retail), causing all fields after +0x118 to shift. Key offsets:
//   mAutoPitchScreenDist at +0x298
//   mAutoTurnScreenDist  at +0x29C
// TODO: Full modtools WeaponClass struct when modtools GameSound size confirmed.
namespace game::modtools {

struct WeaponClass {
    // Common fields up to +0x114 are identical to Steam layout.
    // After that, offsets diverge due to GameSound size.
    char   _common[0x298];
    float  mAutoPitchScreenDist;   // +0x298  (was mAutoAimVert)
    float  mAutoTurnScreenDist;    // +0x29C  (was mAutoAimHoriz)
};
static_assert(offsetof(WeaponClass, mAutoPitchScreenDist) == 0x298);
static_assert(offsetof(WeaponClass, mAutoTurnScreenDist) == 0x29C);

} // namespace game::modtools
