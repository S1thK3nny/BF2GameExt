#pragma once

#include <cstdint>
#include <cstddef>

// Build-specific EntitySoldierClass layouts (skeleton offset differs)

namespace game::modtools {

struct EntitySoldierClass {
    char  _pad0[0x1200];
    void* mSpecialSkeleton;  // +0x1200
};
static_assert(offsetof(EntitySoldierClass, mSpecialSkeleton) == 0x1200);

} // namespace game::modtools

namespace game::steam {

struct EntitySoldierClass {
    char  _pad0[0x100C];
    void* mSpecialSkeleton;  // +0x100C
};
static_assert(offsetof(EntitySoldierClass, mSpecialSkeleton) == 0x100C);

} // namespace game::steam

namespace game::gog {

using EntitySoldierClass = game::steam::EntitySoldierClass;

} // namespace game::gog
