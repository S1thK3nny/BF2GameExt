#pragma once

#include <cstdint>
#include <cstddef>

#include "../pbl/PblVector3.hpp"
#include "../pbl/PblHandle.hpp"
#include "../entity/Entity.hpp"
#include "../entity/GameObject.hpp"
#include "../collision/CollisionModel.hpp"
#include "../camera/CameraTrackSetting.hpp"
#include "../weapon/EnergyBar.hpp"
#include "Character.hpp"
#include "ControllableClass.hpp"
#include "../../exe_type.hpp"

namespace game {

// Forward declarations
struct Controllable;
struct Controller;
struct OrdnanceMissile;
struct Weapon;
struct Aimer;
struct PblMatrix;

// -------------------------------------------------------------------------
// Trigger (4-byte control input, boolean-like)
// -------------------------------------------------------------------------

struct Trigger {
    int value;
};
static_assert(sizeof(Trigger) == 0x04);

// -------------------------------------------------------------------------
// Node (intrusive linked list node)
// -------------------------------------------------------------------------

struct Node {
    char _data[0x14];  // 20 bytes (Thread_data), 16 bytes in AIThread context
};

// -------------------------------------------------------------------------
// Thread_data
// -------------------------------------------------------------------------

struct Thread_data {
    Node mThreadListNode;  // +0x00, 20 bytes
};
static_assert(sizeof(Thread_data) == 0x14);

// -------------------------------------------------------------------------
// Trackable_data
// -------------------------------------------------------------------------

struct Trackable_data {
    int    whichCornerToCastForCamera;  // +0x00
    float  minCameraCastDist[5];        // +0x04
    void*  mTracker;                    // +0x18  Tracker*
    char   _pad1C[0x04];               // +0x1C  (unknown trailing bytes)
};
static_assert(offsetof(Trackable_data, whichCornerToCastForCamera) == 0x00);
static_assert(offsetof(Trackable_data, minCameraCastDist) == 0x04);
static_assert(offsetof(Trackable_data, mTracker) == 0x18);
static_assert(sizeof(Trackable_data) == 0x20);

// -------------------------------------------------------------------------
// AIThread (embedded in Controllable_data)
// -------------------------------------------------------------------------

struct AIThread {
    char _data[0x18];  // 24 bytes
};
static_assert(sizeof(AIThread) == 0x18);

// -------------------------------------------------------------------------
// TargetInfo
// -------------------------------------------------------------------------

struct TargetInfo {
    char _data[0x1C];  // 28 bytes
};
static_assert(sizeof(TargetInfo) == 0x1C);

// PilotType and PilotDeathType enums are in ControllableClass.hpp

// -------------------------------------------------------------------------
// Trackable_vftable (vtable1) — 17 virtual methods
// -------------------------------------------------------------------------

struct Trackable_vftable {
    void              (__thiscall* Dtor)(void* self, uint32_t flags);                          //  0
    void              (__thiscall* SetupCameraTrackMatrixHUDTargeting)(void* self, PblMatrix* mat); //  1
    void              (__thiscall* SetupCameraTrackMatrix)(void* self, PblMatrix* mat, float dt);   //  2
    void              (__thiscall* SetupCameraZoomFactor)(void* self, float* zoom);            //  3
    void              (__thiscall* SetupCameraFOV)(void* self, float* fov);                    //  4
    void              (__thiscall* SetupCameraIconVisibility)(void* self, uint32_t* flags);    //  5
    void              (__thiscall* SetupInterpolatedCameraTrackMatrix)(void* self, float* t);  //  6
    GameObject*       (__thiscall* GetGameObject)(void* self);                                  //  7
    GameObject*       (__thiscall* GetGameObject_const)(void* self);                            //  8
    Controllable*     (__thiscall* GetControllable)(void* self);                                //  9
    Controllable*     (__thiscall* GetControllable_const)(void* self);                          // 10
    CameraTrackSetting* (__thiscall* GetCameraTrackSettings)(void* self);                      // 11
    bool              (__thiscall* RenderTrackable)(void* self, uint32_t param);               // 12
    bool              (__thiscall* IsFirstPersonAvailablePS2)(void* self);                      // 13
    void              (__thiscall* SetFirstPersonView)(void* self, int mode, bool flag);       // 14
    bool              (__thiscall* IsForcedThirdPerson)(void* self);                            // 15
    bool              (__thiscall* IsForcedFirstPerson)(void* self);                            // 16
};
static_assert(sizeof(Trackable_vftable) == 68);

// -------------------------------------------------------------------------
// Controllable_vftable0 — 51 virtual methods
// -------------------------------------------------------------------------

struct Controllable_vftable0 {
    void               (__thiscall* Dtor)(void* self, uint32_t flags);                        //  0
    bool               (__thiscall* Update)(void* self, float dt);                            //  1
    void               (__thiscall* ActivateThread)(void* self, int p1, int p2);              //  2  (Thread)
    void               (__thiscall* DeactivateThread)(void* self);                            //  3  (Thread)
    bool               (__thiscall* IsThreadActive)(void* self);                              //  4  (Thread)
    void               (__thiscall* Kill)(void* self);                                         //  5
    Entity*            (__thiscall* GetEntity_const)(void* self);                              //  6
    Entity*            (__thiscall* GetEntity)(void* self);                                    //  7
    void*              (__thiscall* GetTrackable)(void* self);                                 //  8  returns Trackable*
    CollisionModel*    (__thiscall* GetDefaultPilotCollision)(void* self);                     //  9
    PblMatrix*         (__thiscall* GetMatrix)(void* self);                                    // 10  pure virtual
    float              (__thiscall* GetTargetBubbleStanceModifier)(void* self);               // 11
    float              (__thiscall* GetSoundRadius)(void* self);                              // 12
    float              (__thiscall* GetAimTurnRate)(void* self);                              // 13
    int                (__thiscall* GetWeaponCount)(void* self);                              // 14
    int                (__thiscall* GetWeaponIndex)(void* self, int channel);                 // 15
    Weapon*            (__thiscall* GetWeaponPtr)(void* self, int index);                     // 16
    EnergyBar*         (__thiscall* GetEnergyBar)(void* self);                                // 17
    int                (__thiscall* GetCurrentWeaponIndex)(void* self);                       // 18
    int                (__thiscall* GetSoldierAnimationBank)(void* self);                     // 19
    void               (__thiscall* SetWeaponIndex)(void* self, int p1, int p2);              // 20
    Aimer*             (__thiscall* GetAimer)(void* self, int p1);                             // 21
    bool               (__thiscall* CanEnterRemote)(void* self, GameObject* gameObj);         // 22
    bool               (__thiscall* CanEnterVehicle)(void* self, GameObject* gameObj);        // 23
    bool               (__thiscall* EnterVehicle)(void* self, GameObject* gameObj);           // 24
    bool               (__thiscall* CyclePosition)(void* self, bool forward);                 // 25
    bool               (__thiscall* ExitVehicle)(void* self, bool p1, bool p2);               // 26
    bool               (__thiscall* EnterRemote)(void* self, GameObject* gameObj);            // 27
    bool               (__thiscall* ExitRemote)(void* self);                                   // 28
    ControllableClass* (__thiscall* GetControllableClass_const)(void* self);                   // 29
    ControllableClass* (__thiscall* GetControllableClass)(void* self);                         // 30
    void               (__thiscall* SetupController)(void* self);                             // 31
    void               (__thiscall* SetPilot)(void* self, Controllable* pilot);               // 32
    void               (__thiscall* SetPilotInfo)(void* self, PblMatrix* mat, PblVector3* vec, // 33
                                                  int anim, bool flag,
                                                  float f1, float f2, float f3, float f4);
    void               (__thiscall* GetPilotMatrix)(void* self, PblMatrix* mat1, PblMatrix* mat2); // 34
    int                (__thiscall* GetPilotDeathType)(void* self);                           // 35
    void               (__thiscall* ApplyWeaponKick)(void* self, float yaw, float pitch);     // 36
    void               (__thiscall* GetPitchYawLimits)(void* self,                            // 37
                                                       float* minPitch, float* maxPitch,
                                                       float* minYaw, float* maxYaw);
    bool               (__thiscall* Stand)(void* self);                                        // 38
    bool               (__thiscall* Crouch)(void* self);                                       // 39
    bool               (__thiscall* Prone)(void* self);                                         // 40
    bool               (__thiscall* Jump)(void* self);                                          // 41
    bool               (__thiscall* Roll)(void* self);                                          // 42
    bool               (__thiscall* Sprint)(void* self);                                        // 43
    bool               (__thiscall* EndSprint)(void* self);                                    // 44
    bool               (__thiscall* JetJump)(void* self);                                      // 45
    bool               (__thiscall* EndJetJump)(void* self);                                   // 46
    float              (__thiscall* GetOffenseMultiplier)(void* self);                         // 47
    int                (__thiscall* GetActiveWeaponChannel)(void* self);                      // 48
    void               (__thiscall* UpdateIndirect)(void* self, float dt);                    // 49  pure virtual
    void               (__thiscall* DeactivateIndirect)(void* self);                          // 50
};
static_assert(sizeof(Controllable_vftable0) == 204);

// -------------------------------------------------------------------------
// Controllable (embedded at entity+0x240)
// -------------------------------------------------------------------------
// Player/AI-controllable component. Internal offsets identical across all builds.

struct Controllable {
    // --- vtable 0 ---
    Controllable_vftable0* vtable0;          // +0x00

    // --- Thread_data ---
    Thread_data    mThreadData;              // +0x04

    // --- vtable 1 ---
    Trackable_vftable* vtable1;              // +0x18

    // --- Trackable_data ---
    Trackable_data mTrackableData;           // +0x1C

    // --- Controllable_data (starts at +0x3C) ---

    // Control inputs
    Trigger        mControlFire[2];          // +0x3C
    Trigger        mControlReload;           // +0x44
    Trigger        mControlJump;             // +0x48
    Trigger        mControlCrouch;           // +0x4C
    Trigger        mControlSprint;           // +0x50
    Trigger        mControlUse;              // +0x54
    Trigger        mControlZoom;             // +0x58
    Trigger        mControlView;             // +0x5C
    Trigger        mControlLockTarget;       // +0x60
    Trigger        mControlSquadCommand;     // +0x64
    Trigger        mControlThrustFwd;        // +0x68
    Trigger        mControlThrustBack;       // +0x6C
    Trigger        mControlStrafeLeft;       // +0x70
    Trigger        mControlStrafeRight;      // +0x74
    Trigger        mTriggerControlSwitch[2]; // +0x78

    // Analog inputs
    float          mControlMove;             // +0x80
    float          mControlStrafe;           // +0x84
    float          mControlTurn;             // +0x88
    float          mControlPitch;            // +0x8C
    float          mControlSwitch[2];        // +0x90

    // Weapon / AI
    int            mWpnChannel;              // +0x98
    AIThread       mAIThread;                // +0x9C
    char           mNode[0x10];              // +0xB4  Node (16 bytes)
    char           _padC4[0x04];             // +0xC4  (alignment gap)

    // Ownership
    Controller*    mCtrl;                    // +0xC8
    Character*     mCharacter;              // +0xCC
    Controllable*  mPilot;                   // +0xD0
    int            mPlayerId;                // +0xD4
    int            mInitialPlayerId;         // +0xD8

    // Aim state
    PblVector3     mEyePoint;                // +0xDC
    PblVector3     mEyeDir;                  // +0xE8

    // Turn/pitch
    float          mTurnBuildup;             // +0xF4
    float          mPitchBuildup;            // +0xF8
    bool           mIndirectControlFlag;     // +0xFC
    bool           mUsingTurnAdjusted;       // +0xFD
    bool           mPlayAnim;                // +0xFE
    char           _padFF;                   // +0xFF  (alignment)
    uint32_t       mAnimNameId;              // +0x100
    float          mWpnDistFactor;           // +0x104

    // Collision / lock-on warning
    PblHandle      mCollisionObj;            // +0x108
    float          mEnemyLockedOnMeState;    // +0x110
    float          mEnemyLockedOnMeTimestamp;// +0x114
    OrdnanceMissile* mEnemyLockedOnMeMissile;// +0x118
    float          mEnemyLockedOnMeDistance; // +0x11C

    // Adjusted aim
    float          mTurnAdjusted;            // +0x120
    float          mPitchAdjusted;           // +0x124
    float          mTurnAuto;                // +0x128
    float          mPitchAuto;               // +0x12C
    float          mLockBreakTimer;          // +0x130
    float          mInCaptureRegionTimestamp; // +0x134

    // Target lock
    PblHandle      mTargetLockedObj;         // +0x138
    int            mAllowedLockTypes;        // +0x140
    PilotType      mPilotType;               // +0x144

    // Target info / reticule
    char           mTargetInfoData[0x18];    // +0x148  TargetInfo (first 0x18 bytes)
    bool           mIsAiming;                // +0x160
    char           _pad161[0x03];            // +0x161  (alignment)
    PblHandle      mReticuleTarget[2];       // +0x164
    PblHandle      mAIReticuleTarget[2];     // +0x174
    int            mReticuleAffiliation[2];  // +0x184
};

// Verify key offsets against known values
static_assert(offsetof(Controllable, vtable0) == 0x00);
static_assert(offsetof(Controllable, mThreadData) == 0x04);
static_assert(offsetof(Controllable, vtable1) == 0x18);
static_assert(offsetof(Controllable, mTrackableData) == 0x1C);
static_assert(offsetof(Controllable, mControlFire) == 0x3C);
static_assert(offsetof(Controllable, mControlTurn) == 0x88);
static_assert(offsetof(Controllable, mControlPitch) == 0x8C);
static_assert(offsetof(Controllable, mWpnChannel) == 0x98);
static_assert(offsetof(Controllable, mCtrl) == 0xC8);
static_assert(offsetof(Controllable, mCharacter) == 0xCC);
static_assert(offsetof(Controllable, mPilot) == 0xD0);
static_assert(offsetof(Controllable, mPlayerId) == 0xD4);
static_assert(offsetof(Controllable, mInitialPlayerId) == 0xD8);
static_assert(offsetof(Controllable, mEyePoint) == 0xDC);
static_assert(offsetof(Controllable, mEyeDir) == 0xE8);
static_assert(offsetof(Controllable, mTurnBuildup) == 0xF4);
static_assert(offsetof(Controllable, mIndirectControlFlag) == 0xFC);
static_assert(offsetof(Controllable, mAnimNameId) == 0x100);
static_assert(offsetof(Controllable, mCollisionObj) == 0x108);
static_assert(offsetof(Controllable, mEnemyLockedOnMeState) == 0x110);
static_assert(offsetof(Controllable, mTurnAdjusted) == 0x120);
static_assert(offsetof(Controllable, mTurnAuto) == 0x128);
static_assert(offsetof(Controllable, mLockBreakTimer) == 0x130);
static_assert(offsetof(Controllable, mTargetLockedObj) == 0x138);
static_assert(offsetof(Controllable, mAllowedLockTypes) == 0x140);
static_assert(offsetof(Controllable, mPilotType) == 0x144);
static_assert(offsetof(Controllable, mTargetInfoData) == 0x148);
static_assert(offsetof(Controllable, mIsAiming) == 0x160);
static_assert(offsetof(Controllable, mReticuleTarget) == 0x164);
static_assert(offsetof(Controllable, mReticuleAffiliation) == 0x184);


inline GameObject* ControllableToEntity(Controllable* ctrl) {
    return reinterpret_cast<GameObject*>(
        reinterpret_cast<uintptr_t>(ctrl) - CONTROLLABLE_ENTITY_OFFSET);
}

inline Controllable* EntityToControllable(GameObject* ent) {
    return reinterpret_cast<Controllable*>(
        reinterpret_cast<uintptr_t>(ent) + CONTROLLABLE_ENTITY_OFFSET);
}

// -------------------------------------------------------------------------
// Build-specific accessors for fields past the Controllable struct
// (reaching into EntitySoldier data relative to Controllable base)
// -------------------------------------------------------------------------

inline int Controllable_GetSoldierState(const Controllable* ctrl) {
    constexpr unsigned MODTOOLS_OFF = 0x514;
    constexpr unsigned RETAIL_OFF   = 0x504;
    unsigned off = (g_exeType == ExeType::MODTOOLS) ? MODTOOLS_OFF : RETAIL_OFF;
    return *(const int*)((const char*)ctrl + off);
}

inline void* Controllable_GetSoldierClass(const Controllable* ctrl) {
    constexpr unsigned MODTOOLS_OFF = 0x218;
    constexpr unsigned RETAIL_OFF   = 0x200;
    unsigned off = (g_exeType == ExeType::MODTOOLS) ? MODTOOLS_OFF : RETAIL_OFF;
    return *(void* const*)((const char*)ctrl + off);
}

inline float* Controllable_GetVelocity(const Controllable* ctrl) {
    constexpr unsigned MODTOOLS_OFF = 0x2AC;
    constexpr unsigned RETAIL_OFF   = 0x29C;
    unsigned off = (g_exeType == ExeType::MODTOOLS) ? MODTOOLS_OFF : RETAIL_OFF;
    return (float*)((char*)ctrl + off);
}

} // namespace game
