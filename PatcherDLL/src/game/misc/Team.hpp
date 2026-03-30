#pragma once

#include <cstdint>
#include <cstddef>

#include "RedColor.hpp"

namespace game {

struct Character;
struct Controllable;
struct GameObjectClass;

// -------------------------------------------------------------------------
// PblRandom
// -------------------------------------------------------------------------

struct PblRandom {
    uint32_t _uiSeed;  // +0x00
};
static_assert(sizeof(PblRandom) == 0x04);

// -------------------------------------------------------------------------
// Side enum
// -------------------------------------------------------------------------

struct Side {
    int value;
};
static_assert(sizeof(Side) == 0x04);

// -------------------------------------------------------------------------
// Team_vftable — 6 virtual methods (verified modtools PDB + Steam)
// -------------------------------------------------------------------------

struct Team_vftable {
    void*          (__thiscall* Dtor)(void* self, uint32_t flags);                  //  0
    void           (__thiscall* Init)(void* self);                                  //  1
    void           (__thiscall* InitialUpdate)(void* self);                         //  2
    void           (__thiscall* Update)(void* self);                                //  3
    Character*     (__thiscall* FindByUnit)(void* self, Controllable* unit);        //  4
    void           (__thiscall* CreateCharacter)(void* self, int p1, uint32_t p2);  //  5
};
static_assert(sizeof(Team_vftable) == 24);

// -------------------------------------------------------------------------
// Team
// -------------------------------------------------------------------------
// Layout: vtable (4) + Team_data
// Team_data starts at +0x04 from Team base.

struct Team {
    // --- vtable ---
    Team_vftable*      vtable;                     // +0x00

    // --- Team_data (starts at +0x04) ---
    PblRandom          mNameRand;                  // +0x04
    int                mTeamNumber;                // +0x08
    uint16_t*          mName;                      // +0x0C
    char               mSideNameLocal[12];         // +0x10
    uint32_t           mIcon;                      // +0x1C
    uint32_t           mConquestIcon;              // +0x20
    uint32_t           mCTFIcon;                   // +0x24
    int                mCurReinforcements;         // +0x28
    int                mMaxReinforcements;         // +0x2C
    float              mBleedRate;                 // +0x30
    int                mMemberLimit;               // +0x34
    int                mMemberCount;               // +0x38
    int                mMemberActive;              // +0x3C
    Character**        mMemberArray;               // +0x40
    int                mClassLimit;                // +0x44
    int                mClassCount;                // +0x48
    int                mSpecialClassCount;         // +0x4C
    GameObjectClass**  mClassArray;                // +0x50
    int*               mClassMinUnits;             // +0x54
    int*               mClassMaxUnits;             // +0x58
    GameObjectClass*   mCarrier;                   // +0x5C
    int                mPoints;                    // +0x60
    Side               mSide;                      // +0x64
    RedColor           mColor[8];                  // +0x68
    char               mAffiliation[8];            // +0x88
    int                mCaptureTeam;               // +0x90
    float              mAggressiveness;            // +0x94
    uint16_t*          mFirstNames[64];            // +0x98
    int                mFirstCount;                // +0x198
    uint16_t*          mLastNames[64];             // +0x19C
    int                mLastCount;                 // +0x29C
    bool               mbAIMembersIgnoreVehicles;  // +0x2A0
    bool               mbAllowAISpawn;             // +0x2A1
    bool               mbInitialUpdateDone;        // +0x2A2
};

// Verify key offsets (data offset + 0x04 for vtable)
static_assert(offsetof(Team, vtable) == 0x00);
static_assert(offsetof(Team, mNameRand) == 0x04);
static_assert(offsetof(Team, mTeamNumber) == 0x08);
static_assert(offsetof(Team, mName) == 0x0C);
static_assert(offsetof(Team, mSideNameLocal) == 0x10);
static_assert(offsetof(Team, mIcon) == 0x1C);
static_assert(offsetof(Team, mCurReinforcements) == 0x28);
static_assert(offsetof(Team, mMaxReinforcements) == 0x2C);
static_assert(offsetof(Team, mBleedRate) == 0x30);
static_assert(offsetof(Team, mMemberLimit) == 0x34);
static_assert(offsetof(Team, mMemberCount) == 0x38);
static_assert(offsetof(Team, mMemberActive) == 0x3C);
static_assert(offsetof(Team, mMemberArray) == 0x40);
static_assert(offsetof(Team, mClassLimit) == 0x44);
static_assert(offsetof(Team, mClassCount) == 0x48);
static_assert(offsetof(Team, mSpecialClassCount) == 0x4C);
static_assert(offsetof(Team, mClassArray) == 0x50);
static_assert(offsetof(Team, mClassMinUnits) == 0x54);
static_assert(offsetof(Team, mClassMaxUnits) == 0x58);
static_assert(offsetof(Team, mCarrier) == 0x5C);
static_assert(offsetof(Team, mPoints) == 0x60);
static_assert(offsetof(Team, mSide) == 0x64);
static_assert(offsetof(Team, mColor) == 0x68);
static_assert(offsetof(Team, mAffiliation) == 0x88);
static_assert(offsetof(Team, mCaptureTeam) == 0x90);
static_assert(offsetof(Team, mAggressiveness) == 0x94);
static_assert(offsetof(Team, mFirstNames) == 0x98);
static_assert(offsetof(Team, mFirstCount) == 0x198);
static_assert(offsetof(Team, mLastNames) == 0x19C);
static_assert(offsetof(Team, mLastCount) == 0x29C);
static_assert(offsetof(Team, mbAIMembersIgnoreVehicles) == 0x2A0);
static_assert(offsetof(Team, mbAllowAISpawn) == 0x2A1);
static_assert(offsetof(Team, mbInitialUpdateDone) == 0x2A2);

} // namespace game
