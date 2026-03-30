#pragma once

#include <cstdint>
#include <cstddef>

#include "Entity.hpp"

namespace game {

// Base of all entities, at entity+0x00
// Same 12-slot vtable layout as Entity (overrides slots 0-3 and 10-11).
struct EntityEx {
    Entity_vftable* vtable;       // +0x00
    uint32_t        mId;          // +0x04  PblHash of entity instance name
    void*           mEntityClass; // +0x08  EntityClass* (Factory<Entity,EntityClass,EntityDesc>*)
};
static_assert(offsetof(EntityEx, mId) == 0x04);
static_assert(offsetof(EntityEx, mEntityClass) == 0x08);

} // namespace game
