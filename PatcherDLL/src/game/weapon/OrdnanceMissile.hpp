#pragma once

#include <cstdint>
#include <cstddef>

#include "../pbl/PblVector3.hpp"
#include "../pbl/PblHandle.hpp"
#include "../pbl/PblSphere.hpp"
#include "../misc/DamageOwner.hpp"
#include "Weapon.hpp"

namespace game {

struct RedSceneObject;  // forward declaration

// -------------------------------------------------------------------------
// OrdnanceMissile
// -------------------------------------------------------------------------
// Full inheritance chain:
//   Thread → PblHandled → FoleyFXCollider/ee → Ordnance
//   → DynDisplayable → RedSceneObject → OrdnanceBullet → OrdnanceMissile
//
// Total: 0x168 bytes

struct OrdnanceMissile {
    // --- Thread (vtable0 + Thread_data) ---
    void*          vtable0;                      // +0x00
    char           mThreadData[0x14];            // +0x04  Thread_data (Node, 20 bytes)

    // --- PblHandled (vtable1 + data) ---
    void*          vtable1;                      // +0x18
    int            mHandleId;                    // +0x1C  PblHandled_data

    // --- FoleyFXCollider ---
    void*          mColliderClass;               // +0x20  FoleyFXColliderClass*
    void*          mFoleyFX;                     // +0x24  FoleyFX*
    void*          mFoleyFXGroup;                // +0x28  FoleyFXGroup*

    // --- FoleyFXCollidee ---
    void*          mCollideeClass;               // +0x2C  FoleyFXCollideeClass*

    // --- Ordnance_data ---
    void*          mClass;                       // +0x30  OrdnanceClass*
    float          mRatio;                       // +0x34
    uint8_t        mOrdFlags0;                   // +0x38
    uint8_t        mOrdFlags1;                   // +0x39
    char           _pad3A[0x02];                 // +0x3A
    float          mLifeTime;                    // +0x3C
    float          mLifeElapsed;                 // +0x40
    uint32_t       mRenderColor;                 // +0x44  RedColor
    PblVector3     mPosition;                    // +0x48
    PblHandle      mOwner;                       // +0x54  PblHandle<GameObject>
    DamageOwner    mDamageOwner;                 // +0x5C
    char           mCollisionManagerNode[0x10];  // +0x78  undefined4[4]
    PblHandle      mTrailEmitterObj;             // +0x88  PblHandle<FLEffectObject>
    GameSoundControllable mSoundOrdnance;        // +0x90
    int            mInterpIndex;                 // +0x94

    // --- DynDisplayable (vtable2 + data) ---
    void*          vtable2;                      // +0x98
    void*          sector_;                      // +0x9C  Sector*
    uint32_t       dynFlags_;                    // +0xA0
    int            aframe_;                      // +0xA4
    int            pframe_;                      // +0xA8
    float          nextCheckDist_;               // +0xAC
    PblVector3     lastCheckPos_;                // +0xB0

    // --- RedSceneObject_data ---
    RedSceneObject* mNextSceneObject;            // +0xBC
    RedSceneObject* mPrevSceneObject;            // +0xC0
    void*          mSceneAABox;                  // +0xC4  RedSceneAABox*
    PblSphere      _Sphere;                      // +0xC8
    void*          _pLodData;                    // +0xD8  RedLodData*
    uint32_t       _uiRenderFlags;               // +0xDC
    bool           _bActive;                     // +0xE0
    char           _padE1[0x03];                 // +0xE1
    float          _fPriorityMod;                // +0xE4
    int            _iFrameNum[1];                // +0xE8
    uint32_t       _uiOcclusionIndex[1];         // +0xEC

    // --- OrdnanceBullet_data ---
    PblVector3     mOldPosition;                 // +0xF0
    PblVector3     mVelocity;                    // +0xFC
    PblVector3     mFinalCollisionNormal;        // +0x108
    void*          mFinalCollision;              // +0x114  CollisionObject*
    PblHandle      mFinalCollisionHandle;        // +0x118  PblHandle<GameObject>
    bool           mFinalCollisionOnNextUpdate;  // +0x120
    char           _pad121[0x03];                // +0x121
    void*          m_pLight;                     // +0x124  EntityLight*
    float          mOffscreenTime;               // +0x128

    // --- OrdnanceMissile_data ---
    PblVector3     mTargetDir;                   // +0x12C
    PblHandle      mTarget;                      // +0x138  PblHandle<GameObject const>
    int            mBodyID;                      // +0x140
    float          mScale;                       // +0x144
    float          mWaverPitch;                  // +0x148
    float          mWaverYaw;                    // +0x14C
    GameSoundControllable mLockedOnSoundCtrl;    // +0x150
    PblVector3     mVelocityDir;                 // +0x154
    float          mGoalSpeed;                   // +0x160
    float          mSpeed;                       // +0x164
};
static_assert(sizeof(OrdnanceMissile) == 0x168);
static_assert(offsetof(OrdnanceMissile, vtable1) == 0x18);
static_assert(offsetof(OrdnanceMissile, mClass) == 0x30);
static_assert(offsetof(OrdnanceMissile, mPosition) == 0x48);
static_assert(offsetof(OrdnanceMissile, mDamageOwner) == 0x5C);
static_assert(offsetof(OrdnanceMissile, vtable2) == 0x98);
static_assert(offsetof(OrdnanceMissile, mNextSceneObject) == 0xBC);
static_assert(offsetof(OrdnanceMissile, _Sphere) == 0xC8);
static_assert(offsetof(OrdnanceMissile, mOldPosition) == 0xF0);
static_assert(offsetof(OrdnanceMissile, mVelocity) == 0xFC);
static_assert(offsetof(OrdnanceMissile, mTargetDir) == 0x12C);
static_assert(offsetof(OrdnanceMissile, mTarget) == 0x138);
static_assert(offsetof(OrdnanceMissile, mVelocityDir) == 0x154);
static_assert(offsetof(OrdnanceMissile, mSpeed) == 0x164);

} // namespace game
