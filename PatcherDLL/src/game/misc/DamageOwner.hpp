#pragma once

#include <cstdint>
#include <cstddef>

#include "../pbl/PblHandle.hpp"

namespace game {

struct Character;

// Passed to Damageable::ApplyDamage
struct DamageOwner {
    Character* mCharacter;       // +0x00
    PblHandle  mGameObject;      // +0x04  PblHandle<GameObject>
    int        mTeam;            // +0x0C
    void*      mClass;           // +0x10  EntityClass*
    void*      mWeapon;          // +0x14  WeaponClass*
    float      mDamageMultiplier;// +0x18
};
static_assert(offsetof(DamageOwner, mGameObject) == 0x04);
static_assert(offsetof(DamageOwner, mTeam) == 0x0C);
static_assert(sizeof(DamageOwner) == 0x1C);

} // namespace game
