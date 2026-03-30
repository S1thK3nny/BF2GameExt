#pragma once

#include <cstdint>
#include <cstddef>

#include "../pbl/PblVector3.hpp"

namespace game {

struct Controllable;  // forward declaration
struct Team;
struct CommandPost;
struct FlagItem;
struct GameObject;
struct MedalsMgr;
struct NetPktGroup;

// -------------------------------------------------------------------------
// Character_vftable — 8 virtual methods (verified modtools PDB + Steam)
// -------------------------------------------------------------------------

struct Character_vftable {
    void*          (__thiscall* Dtor)(void* self, uint32_t flags);                        //  0
    bool           (__thiscall* Update)(void* self, float dt);                            //  1
    void           (__thiscall* ActivateThread)(void* self, int p1, int p2);              //  2
    void           (__thiscall* DeactivateThread)(void* self);                            //  3
    bool           (__thiscall* IsThreadActive)(void* self);                              //  4
    void           (__thiscall* InitialUpdate)(void* self);                               //  5
    void           (__thiscall* Write)(void* self, NetPktGroup* pkt);                     //  6
    void           (__thiscall* Read)(void* self, NetPktGroup* pkt);                      //  7
};
static_assert(sizeof(Character_vftable) == 32);

// -------------------------------------------------------------------------
// CommandPostHandle
// -------------------------------------------------------------------------

struct CommandPostHandle {
    CommandPost* mPost;  // +0x00
};
static_assert(sizeof(CommandPostHandle) == 0x04);

// -------------------------------------------------------------------------
// Adrenaline
// -------------------------------------------------------------------------

struct Adrenaline {
    float mLevel;  // +0x00
};
static_assert(sizeof(Adrenaline) == 0x04);

// -------------------------------------------------------------------------
// Character (stride 0x1B0, all builds)
// -------------------------------------------------------------------------
// Layout: vtable (4) + Thread_data (0x14) + Character_data
// Character_data starts at +0x18 from Character base.

struct Character {
    // --- vtable + Thread_data ---
    Character_vftable* vtable;                     // +0x00
    char               mThreadData[0x14];          // +0x04  Thread_data (Node, 20 bytes)

    // --- Character_data (starts at +0x18) ---
    CommandPostHandle  mAssignedPost;              // +0x18
    Adrenaline         mAdrenaline;                // +0x1C
    char               mGoalNode[0x10];            // +0x20  PblList<Character>::Node
    uint32_t           mName[32];                  // +0x30
    uint32_t           mRegularName[32];           // +0xB0
    int                mPlayerId;                  // +0x130
    int                mTeamNumber;                // +0x134
    Team*              mTeamPtr;                   // +0x138
    int                mClassIndex;                // +0x13C
    void*              mClassPtr;                  // +0x140  EntityClass*
    CommandPostHandle  mPost;                      // +0x144
    Controllable*      mUnit;                      // +0x148
    Controllable*      mVehicle;                   // +0x14C
    Controllable*      mRemote;                    // +0x150
    float              mLastVehicleOrRemoteChangeTime; // +0x154
    float              mDelayTimer;                // +0x158
    int                mSpawnCycle;                // +0x15C
    int                mSpawnSlot;                 // +0x160
    bool               mSpawnReady;                // +0x164
    bool               mHeroFlag;                  // +0x165
    char               _pad166[0x02];              // +0x166  (alignment)
    float              mVehicleBlockTime;          // +0x168
    float              mObjectHitTimer[3];         // +0x16C
    float              mObjectHitMultiplier[3];    // +0x178
    float              mOutOfBoundsTimer;          // +0x184
    float              mSpawnTime;                 // +0x188
    FlagItem*          mCarriedFlagPtr;            // +0x18C
    GameObject*        mDamagedVehicle;            // +0x190
    float              mTotalVehicleDamage;        // +0x194
    CommandPostHandle  mCPHold;                    // +0x198
    float              mCPHoldTimestamp;           // +0x19C
    MedalsMgr*         mMedalsMgr;                // +0x1A0
    Character*         mTakeoverChr;               // +0x1A4
    float              mTakeoverTime;              // +0x1A8
    bool               mTakeoverFirstTime : 1;     // +0x1AC  (bitfield)
};

// Stride is 0x1B0 (verified from character array indexing)
static_assert(sizeof(Character) == 0x1B0);

// Verify key offsets (vtable + Thread_data offset = 0x18 added to data offsets)
static_assert(offsetof(Character, vtable) == 0x00);
static_assert(offsetof(Character, mAssignedPost) == 0x18);
static_assert(offsetof(Character, mAdrenaline) == 0x1C);
static_assert(offsetof(Character, mGoalNode) == 0x20);
static_assert(offsetof(Character, mName) == 0x30);
static_assert(offsetof(Character, mRegularName) == 0xB0);
static_assert(offsetof(Character, mPlayerId) == 0x130);
static_assert(offsetof(Character, mTeamNumber) == 0x134);
static_assert(offsetof(Character, mTeamPtr) == 0x138);
static_assert(offsetof(Character, mClassIndex) == 0x13C);
static_assert(offsetof(Character, mClassPtr) == 0x140);
static_assert(offsetof(Character, mPost) == 0x144);
static_assert(offsetof(Character, mUnit) == 0x148);
static_assert(offsetof(Character, mVehicle) == 0x14C);
static_assert(offsetof(Character, mRemote) == 0x150);
static_assert(offsetof(Character, mSpawnCycle) == 0x15C);
static_assert(offsetof(Character, mSpawnReady) == 0x164);
static_assert(offsetof(Character, mHeroFlag) == 0x165);
static_assert(offsetof(Character, mVehicleBlockTime) == 0x168);
static_assert(offsetof(Character, mObjectHitTimer) == 0x16C);
static_assert(offsetof(Character, mOutOfBoundsTimer) == 0x184);
static_assert(offsetof(Character, mSpawnTime) == 0x188);
static_assert(offsetof(Character, mCarriedFlagPtr) == 0x18C);
static_assert(offsetof(Character, mDamagedVehicle) == 0x190);
static_assert(offsetof(Character, mCPHold) == 0x198);
static_assert(offsetof(Character, mMedalsMgr) == 0x1A0);
static_assert(offsetof(Character, mTakeoverChr) == 0x1A4);
static_assert(offsetof(Character, mTakeoverTime) == 0x1A8);

} // namespace game
