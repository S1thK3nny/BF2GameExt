#pragma once

#include <stddef.h>
#include <stdint.h>

#include "game/Battlefront2/Source/GameObject.h"
#include "game/PebbleFL/Common/PblHandle.h"
#include "game/PebbleFL/Common/PblList.h"
#include "game/PebbleFL/Common/PblMatrix.h"

class CommandPost;
class EntityClass;
class EntityFlyerClass;
class VehicleSpawnClass;

// Same layout on every build. Names and order from the PDB; every field GameExt
// uses is accessed at the same displacement by UpdateSpawn (modtools 0x00665A50,
// Steam 0x0066F370, GOG 0x00670410) or SetProperty (0x00664E60, 0x0066EAD0,
// 0x0066FB70).
struct VehicleSpawn {
   // Allocated from VehicleTracker::sMemoryPool (UpdateSpawn pushes 0x1C).
   struct VehicleTracker : PblList<VehicleTracker>::Node {
      PblHandle<GameObject> mVehicle;
      float                 mEmptyTimer;
   };

   uint8_t                   _bases[0x1C];    // Entity, Thread
   PblList<VehicleSpawn>::Node m_ListNode;
   PblMatrix                 mMatrix;
   VehicleSpawnClass*        mClass;
   const CommandPost*        mCommandPost;
   uint32_t                  mNameId;
   int                       mSpawnCount;
   float                     mSpawnTime;
   float                     mExpireTimeEnemy;
   float                     mExpireTimeField;
   float                     mDecayTime;
   EntityClass*              mSpawnClass[8];  // per team
   EntityFlyerClass*         mFlyerClass[8];
   bool                      mUseCarrier[8];
   PblList<VehicleTracker>   mTrackerList;
   PblHandle<GameObject>     mCarrier;
   int                       mVehicleTeam;
   int                       mSpawnTeam;      // 1-based
   bool                      mSpawnTrigger;
   float                     mSpawnTimer;
   float                     mCycleTimer;
   alignas(16) uint8_t       mSpawnRegion[0x60];  // RedRegion
};

static_assert(sizeof(VehicleSpawn::VehicleTracker) == 0x1C, "VehicleTracker");
static_assert(offsetof(VehicleSpawn::VehicleTracker, mVehicle) == 0x10, "mVehicle");
static_assert(offsetof(VehicleSpawn::VehicleTracker, mEmptyTimer) == 0x18, "mEmptyTimer");

static_assert(sizeof(VehicleSpawn) == 0x170, "VehicleSpawn");
static_assert(offsetof(VehicleSpawn, m_ListNode) == 0x1C, "m_ListNode");
static_assert(offsetof(VehicleSpawn, mMatrix) == 0x30, "mMatrix");
static_assert(offsetof(VehicleSpawn, mClass) == 0x70, "mClass");
static_assert(offsetof(VehicleSpawn, mCommandPost) == 0x74, "mCommandPost");
static_assert(offsetof(VehicleSpawn, mNameId) == 0x78, "mNameId");
static_assert(offsetof(VehicleSpawn, mSpawnCount) == 0x7C, "mSpawnCount");
static_assert(offsetof(VehicleSpawn, mSpawnTime) == 0x80, "mSpawnTime");
static_assert(offsetof(VehicleSpawn, mDecayTime) == 0x8C, "mDecayTime");
static_assert(offsetof(VehicleSpawn, mSpawnClass) == 0x90, "mSpawnClass");
static_assert(offsetof(VehicleSpawn, mFlyerClass) == 0xB0, "mFlyerClass");
static_assert(offsetof(VehicleSpawn, mUseCarrier) == 0xD0, "mUseCarrier");
static_assert(offsetof(VehicleSpawn, mTrackerList) == 0xD8, "mTrackerList");
static_assert(offsetof(VehicleSpawn, mTrackerList._iCount) == 0xE8, "mTrackerList._iCount");
static_assert(offsetof(VehicleSpawn, mCarrier) == 0xEC, "mCarrier");
static_assert(offsetof(VehicleSpawn, mVehicleTeam) == 0xF4, "mVehicleTeam");
static_assert(offsetof(VehicleSpawn, mSpawnTeam) == 0xF8, "mSpawnTeam");
static_assert(offsetof(VehicleSpawn, mSpawnTrigger) == 0xFC, "mSpawnTrigger");
static_assert(offsetof(VehicleSpawn, mSpawnTimer) == 0x100, "mSpawnTimer");
static_assert(offsetof(VehicleSpawn, mCycleTimer) == 0x104, "mCycleTimer");
static_assert(offsetof(VehicleSpawn, mSpawnRegion) == 0x110, "mSpawnRegion");
