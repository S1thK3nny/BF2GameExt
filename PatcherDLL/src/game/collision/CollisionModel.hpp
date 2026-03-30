#pragma once

#include <cstdint>
#include <cstddef>

namespace game {

struct CollisionBody;  // forward declaration

// -------------------------------------------------------------------------
// CollisionModel_vftable — 3 virtual methods (verified Steam)
// -------------------------------------------------------------------------

struct CollisionModel_vftable {
    void*  (__thiscall* Dtor)(void* self, uint32_t flags);                          //  0
    bool   (__thiscall* CheckForCollision)(void* self, void* obj1, void* obj2,      //  1
                                           void* restrictor, void* optInfo);
    bool   (__thiscall* CheckForCollision_SSSC)(void* self, void* obj1, void* obj2, //  2
                                                void* restrictor, void* optInfo);
};
static_assert(sizeof(CollisionModel_vftable) == 12);

// -------------------------------------------------------------------------
// CollisionMask (from PDB)
// -------------------------------------------------------------------------

struct CollisionMask {
    uint32_t mMask[2];
};
static_assert(sizeof(CollisionMask) == 0x08);

// -------------------------------------------------------------------------
// CollisionModel
// -------------------------------------------------------------------------
// vtable (4) + CollisionModel_data (0x148)
// Total: 0x14C bytes

struct CollisionModel {
    CollisionModel_vftable* vtable;                   // +0x00

    // --- CollisionModel_data (starts at +0x04) ---
    bool            mDenyFlyerLand;                   // +0x04
    bool            mDeathOnFlyerLand;                // +0x05
    bool            mIsFlyerFlag;                     // +0x06
    bool            mUseVehicleCollisionAgainstFlyers;// +0x07
    CollisionBody*  mBody[64];                        // +0x08
    int             mNumBodies;                       // +0x108
    CollisionMask   mSoftMask;                        // +0x10C
    CollisionMask   mRigidMask;                       // +0x114
    CollisionMask   mStaticMask;                      // +0x11C
    CollisionMask   mTerrainMask;                     // +0x124
    CollisionMask   mOrdnanceMask;                    // +0x12C
    CollisionMask   mTowCableMask;                    // +0x134
    CollisionMask   mTargetableMask;                  // +0x13C
    CollisionMask   mFlyingOrdnanceMask;              // +0x144
};
static_assert(sizeof(CollisionModel) == 0x14C);
static_assert(offsetof(CollisionModel, mDenyFlyerLand) == 0x04);
static_assert(offsetof(CollisionModel, mBody) == 0x08);
static_assert(offsetof(CollisionModel, mNumBodies) == 0x108);
static_assert(offsetof(CollisionModel, mSoftMask) == 0x10C);
static_assert(offsetof(CollisionModel, mRigidMask) == 0x114);
static_assert(offsetof(CollisionModel, mStaticMask) == 0x11C);
static_assert(offsetof(CollisionModel, mTerrainMask) == 0x124);
static_assert(offsetof(CollisionModel, mOrdnanceMask) == 0x12C);
static_assert(offsetof(CollisionModel, mTowCableMask) == 0x134);
static_assert(offsetof(CollisionModel, mTargetableMask) == 0x13C);
static_assert(offsetof(CollisionModel, mFlyingOrdnanceMask) == 0x144);

} // namespace game
