#pragma once

#include <cstdint>
#include <cstddef>

#include "../../exe_type.hpp"
#include "../pbl/PblVector3.hpp"
#include "../pbl/PblMatrix.hpp"
#include "../pbl/PblHandle.hpp"
#include "../misc/GameSoundControllable.hpp"

namespace game {

struct Controllable;  // forward declaration
struct GameObject;
struct Aimer;
struct Trigger;
struct NetPktGroup;
struct RedPose;
struct RedColor;

// -------------------------------------------------------------------------
// Weapon_vftable — 51 virtual methods (verified modtools PDB + Steam)
// -------------------------------------------------------------------------

struct Weapon_vftable {
    void*              (__thiscall* Dtor)(void* self, uint32_t flags);                   //  0
    bool               (__thiscall* Update)(void* self, float dt);                       //  1
    void               (__thiscall* ActivateThread)(void* self, int p1, int p2);         //  2
    void               (__thiscall* DeactivateThread)(void* self);                       //  3
    bool               (__thiscall* IsThreadActive)(void* self);                         //  4
    bool               (__thiscall* IsRtti)(void* self, int rtti);                       //  5
    uint32_t           (__thiscall* GetDerivedRtti)(void* self);                          //  6
    char*              (__thiscall* GetDerivedRttiName)(void* self);                      //  7
    void               (__thiscall* Init)(void* self);                                    //  8
    float              (__thiscall* GetOrdnanceVelocity)(void* self);                     //  9
    float              (__thiscall* GetOrdnanceGravity)(void* self);                      // 10
    bool               (__thiscall* IsLocked)(void* self);                                // 11
    GameObject*        (__thiscall* GetLocked)(void* self);                                // 12
    int                (__thiscall* GetLockedTargetBodyID)(void* self);                    // 13
    void               (__thiscall* SetKickSpreadFactor)(void* self, float f);             // 14
    void               (__thiscall* SetYawSpreadFactor)(void* self, float f);              // 15
    float              (__thiscall* GetYawSpread)(void* self);                             // 16
    float              (__thiscall* GetPitchSpread)(void* self);                           // 17
    bool               (__thiscall* Deflect)(void* self);                                  // 18
    void               (__thiscall* SignalFire)(void* self);                               // 19
    bool               (__thiscall* ShouldShowReticule)(void* self);                       // 20
    bool               (__thiscall* IsMelee)(void* self);                                  // 21
    bool               (__thiscall* IsMeleeThrow)(void* self);                             // 22
    void               (__thiscall* NotifySoldierState)(void* self, int state);             // 23
    bool               (__thiscall* SoldierCanOperate)(void* self);                        // 24
    bool               (__thiscall* OverrideSoldierVelocity)(void* self);                  // 25
    bool               (__thiscall* OverrideSoldierControls)(void* self);                   // 26
    bool               (__thiscall* OverrideSoldierEnergyRestore)(void* self);              // 27
    bool               (__thiscall* OverrideAimer)(void* self);                            // 28
    void               (__thiscall* Select)(void* self, uint32_t p1, bool p2);             // 29
    void               (__thiscall* Deselect)(void* self);                                 // 30
    bool               (__thiscall* IsBusy)(void* self);                                   // 31
    bool               (__thiscall* WillBecomeBusy)(void* self);                           // 32
    void               (__thiscall* Write)(void* self, NetPktGroup* pkt);                  // 33
    void               (__thiscall* Read)(void* self, NetPktGroup* pkt);                   // 34
    void               (__thiscall* Render)(void* self, PblMatrix*, RedPose*,              // 35
                                            RedColor*, uint32_t, bool);
    int                (__thiscall* GetNameHash)(void* self);                               // 36
    void               (__thiscall* EnterIdle)(void* self);                                // 37
    bool               (__thiscall* UpdateIdle)(void* self, float dt);                     // 38
    void               (__thiscall* ExitIdle)(void* self);                                 // 39
    void               (__thiscall* EnterFire)(void* self);                                // 40
    bool               (__thiscall* UpdateFire)(void* self);                               // 41
    void               (__thiscall* StopFireSound)(void* self);                            // 42
    void               (__thiscall* EnterCharge)(void* self);                              // 43
    bool               (__thiscall* UpdateCharge)(void* self, float dt);                   // 44
    void               (__thiscall* ExitCharge)(void* self);                               // 45
    void               (__thiscall* EnterReload)(void* self);                              // 46
    bool               (__thiscall* UpdateReload)(void* self, float dt);                   // 47
    void               (__thiscall* ExitReload)(void* self);                               // 48
    void               (__thiscall* EnterEmpty)(void* self);                               // 49
    bool               (__thiscall* UpdateEmpty)(void* self, float dt);                    // 50
};
static_assert(sizeof(Weapon_vftable) == 204);

// -------------------------------------------------------------------------
// Weapon instance (all builds identical)
// -------------------------------------------------------------------------
// Layout: vtable (4) + Thread_data (0x14) + Weapon_data
// Weapon_data starts at +0x18 from Weapon base.

struct Weapon {
    // --- vtable + Thread_data ---
    Weapon_vftable*    vtable;                     // +0x00
    char               mThreadData[0x14];          // +0x04  Thread_data

    // --- Weapon_data (starts at +0x18) ---
    char               _pad18[0x08];               // +0x18  (8 undefined bytes)

    PblMatrix          mFirePointMatrix;           // +0x20

    // Class pointers (cast to build-specific WeaponClass at point of use)
    void*              mStart;                     // +0x60  original WeaponClass*
    void*              mClass;                     // +0x64  current active WeaponClass*
    void*              mRenderClass;               // +0x68  rendering WeaponClass*
    Controllable*      mOwner;                     // +0x6C
    Aimer*             mAimer;                     // +0x70
    Trigger*           mTrigger;                   // +0x74
    Trigger*           mReload;                    // +0x78
    PblVector3         mFirePos;                   // +0x7C

    // Ammo / energy
    void*              m_pAmmoCounter;             // +0x88  AmmoCounter*
    void*              m_pEnergyBar;               // +0x8C  EnergyBar*

    // Charge state
    float              mCurChargeStrengthHeavy;    // +0x90
    float              mCurChargeStrengthLight;    // +0x94
    float              mCurChargeDelayHeavy;       // +0x98
    float              mCurChargeDelayLight;       // +0x9C
    float              mCurChargeRateLight;        // +0xA0
    float              mCurChargeRateHeavy;        // +0xA4
    float              mCurTimeAtMaxCharge;        // +0xA8

    // Bitfields at +0xAC
    // mHideWeapon:1, mFiredFlag:1, mSelectedFlag:1, m_iSoldierState:6
    uint32_t           mFlags;                     // +0xAC

    // State machine
    int                mState;                     // +0xB0  WeaponState enum
    float              mStateTimer;                // +0xB4
    float              mStateLimit;                // +0xB8

    // Zoom
    float              mZoom;                      // +0xBC
    float              mZoomTurnScale;             // +0xC0

    // Misc
    float              mMuzzleFlashStartTime;      // +0xC4
    int                mSoldierAnimationMap;       // +0xC8  MAP enum

    // Sound
    GameSoundControllable mSoundControl;           // +0xCC
    GameSoundControllable mSoundControlFire;       // +0xD0
    GameSoundControllable mFoleyFXControl;         // +0xD4
    void*              mSoundProps;                // +0xD8  GameSound*
    char               _padDC[0x04];               // +0xDC
    void*              mSoundPropsFire;            // +0xE0  GameSound*
    char               _padE4[0x04];               // +0xE4
    void*              mFoleyFXProps;              // +0xE8  GameSound*
    char               _padEC[0x04];               // +0xEC

    // Charge emitter
    PblHandle          mChargeUpEmitter;           // +0xF0

    // Timing
    float              mLastFireTime;              // +0xF8
    float              mSkipTime;                  // +0xFC

    // Bitfields at +0x100: mSkip:1, mCharged:1
    uint8_t            mSkipCharged;               // +0x100
    uint8_t            mDeactivateScheduled;       // +0x101
    char               _pad102[0x02];              // +0x102  (alignment)

    // Target
    PblHandle          mTarget;                    // +0x104
    int                mTargetBodyID;              // +0x10C
};

// Verify key offsets
static_assert(offsetof(Weapon, vtable) == 0x00);
static_assert(offsetof(Weapon, mFirePointMatrix) == 0x20);
static_assert(offsetof(Weapon, mStart) == 0x60);
static_assert(offsetof(Weapon, mClass) == 0x64);
static_assert(offsetof(Weapon, mRenderClass) == 0x68);
static_assert(offsetof(Weapon, mOwner) == 0x6C);
static_assert(offsetof(Weapon, mAimer) == 0x70);
static_assert(offsetof(Weapon, mTrigger) == 0x74);
static_assert(offsetof(Weapon, mReload) == 0x78);
static_assert(offsetof(Weapon, mFirePos) == 0x7C);
static_assert(offsetof(Weapon, m_pAmmoCounter) == 0x88);
static_assert(offsetof(Weapon, m_pEnergyBar) == 0x8C);
static_assert(offsetof(Weapon, mCurChargeStrengthHeavy) == 0x90);
static_assert(offsetof(Weapon, mCurTimeAtMaxCharge) == 0xA8);
static_assert(offsetof(Weapon, mState) == 0xB0);
static_assert(offsetof(Weapon, mStateTimer) == 0xB4);
static_assert(offsetof(Weapon, mZoom) == 0xBC);
static_assert(offsetof(Weapon, mMuzzleFlashStartTime) == 0xC4);
static_assert(offsetof(Weapon, mSoldierAnimationMap) == 0xC8);
static_assert(offsetof(Weapon, mSoundControl) == 0xCC);
static_assert(offsetof(Weapon, mSoundControlFire) == 0xD0);
static_assert(offsetof(Weapon, mFoleyFXControl) == 0xD4);
static_assert(offsetof(Weapon, mSoundProps) == 0xD8);
static_assert(offsetof(Weapon, mSoundPropsFire) == 0xE0);
static_assert(offsetof(Weapon, mFoleyFXProps) == 0xE8);
static_assert(offsetof(Weapon, mChargeUpEmitter) == 0xF0);
static_assert(offsetof(Weapon, mLastFireTime) == 0xF8);
static_assert(offsetof(Weapon, mSkipTime) == 0xFC);
static_assert(offsetof(Weapon, mTarget) == 0x104);
static_assert(offsetof(Weapon, mTargetBodyID) == 0x10C);

// Weapon states
enum WeaponState : int {
    WPN_IDLE     = 0,
    WPN_FIRE     = 1,
    WPN_FIRE2    = 2,
    WPN_CHARGE   = 3,
    WPN_RELOAD   = 4,
    WPN_OVERHEAT = 5,
    WPN_EMPTY    = 6,
};

// -------------------------------------------------------------------------
// Build-specific field accessors
// -------------------------------------------------------------------------
// Some Weapon fields shift between modtools and retail (Steam/GOG).

inline float Weapon_GetLastFireTime(const Weapon* wpn) {
    constexpr unsigned MODTOOLS_OFF = 0x11C;
    constexpr unsigned RETAIL_OFF   = 0xF8;
    unsigned off = (g_exeType == ExeType::MODTOOLS) ? MODTOOLS_OFF : RETAIL_OFF;
    return *(const float*)((const char*)wpn + off);
}

} // namespace game
