#pragma once

#include <cstdint>
#include <cstddef>

#include "../pbl/PblHandle.hpp"
#include "../misc/DamageOwner.hpp"
#include "../misc/GameSoundControllable.hpp"

namespace game {

struct GameObject;
struct PblVector3;

// Forward declare for DamageDesc (used in ApplyDamage)
struct DamageDesc;

// -------------------------------------------------------------------------
// Damageable_vftable — 10 virtual methods (verified modtools PDB + Steam)
// -------------------------------------------------------------------------

struct Damageable_vftable {
    void*          (__thiscall* Dtor)(void* self, uint32_t flags);                        //  0
    void           (__thiscall* Kill)(void* self);                                        //  1
    void           (__thiscall* Respawn)(void* self);                                     //  2
    GameObject*    (__thiscall* GetGameObject)(void* self);                                //  3
    GameObject*    (__thiscall* GetGameObject_const)(void* self);                          //  4
    bool           (__thiscall* ApplyDamage)(void* self, DamageDesc*, PblVector3*,         //  5
                                             PblVector3*, uint32_t);
    void           (__thiscall* InstantDeath)(void* self, DamageOwner*, bool);             //  6
    void           (__thiscall* _purecall)(void* self);                                    //  7  pure virtual
    void           (__thiscall* HealthChangeCallback)(void* self, float delta);            //  8
    void           (__thiscall* ShieldChangeCallback)(void* self, float delta);            //  9
};
static_assert(sizeof(Damageable_vftable) == 40);

// -------------------------------------------------------------------------
// Damageable (embedded at entity+0x140, size 0xC0)
// -------------------------------------------------------------------------

struct Damageable {
    Damageable_vftable* vtable;               // +0x00

    // --- Damageable_data ---
    float          mCurHealth;                // +0x04
    float          mMaxHealth;                // +0x08
    float          mAddHealth;                // +0x0C
    float          mCurShield;                // +0x10
    float          mMaxShield;                // +0x14
    float          mAddShield;                // +0x18
    float          mDisableTime;              // +0x1C
    float          mAIDamageThreshold;        // +0x20
    DamageOwner    mDamageOwner;              // +0x24  (28 bytes)
    PblHandle      mDamageEffect[10];         // +0x40  PblHandle<FLEffectObject>[10]
    GameSoundControllable mDamageEffectSound[10]; // +0x90
    float          mDamageRegionTime;         // +0xB8

    // Bitfields at +0xBC
    // mHealthType:3, mIsAlive:1, mVanishing:1, mIsShielded:1
    uint8_t        mHealthFlags;              // +0xBC
    char           _padBD[0x03];              // +0xBD
};
static_assert(sizeof(Damageable) == 0xC0);
static_assert(offsetof(Damageable, vtable) == 0x00);
static_assert(offsetof(Damageable, mCurHealth) == 0x04);
static_assert(offsetof(Damageable, mMaxHealth) == 0x08);
static_assert(offsetof(Damageable, mAddHealth) == 0x0C);
static_assert(offsetof(Damageable, mCurShield) == 0x10);
static_assert(offsetof(Damageable, mMaxShield) == 0x14);
static_assert(offsetof(Damageable, mAddShield) == 0x18);
static_assert(offsetof(Damageable, mDisableTime) == 0x1C);
static_assert(offsetof(Damageable, mAIDamageThreshold) == 0x20);
static_assert(offsetof(Damageable, mDamageOwner) == 0x24);
static_assert(offsetof(Damageable, mDamageEffect) == 0x40);
static_assert(offsetof(Damageable, mDamageEffectSound) == 0x90);
static_assert(offsetof(Damageable, mDamageRegionTime) == 0xB8);
static_assert(offsetof(Damageable, mHealthFlags) == 0xBC);

} // namespace game
