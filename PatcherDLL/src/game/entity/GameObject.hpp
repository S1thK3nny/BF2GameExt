#pragma once

#include <cstdint>
#include <cstddef>

#include "EntityEx.hpp"
#include "Damageable.hpp"
#include "../treegrid/TreeGridObject.hpp"
#include "../pbl/PblVector3.hpp"

namespace game {

struct Controllable;  // forward declaration

// Full entity base. Damageable at +0x140, Controllable at +0x240.
struct GameObject {
    EntityEx       mEntityEx;              // +0x00  (0x0C bytes: vtable + EntityEx_data)
    void*          mCollisionObjVtable;    // +0x0C  CollisionObject vtable
    TreeGridObject mTreeGrid;              // +0x10
    char           _pad1[0x140 - 0x10 - sizeof(TreeGridObject)];
    Damageable     mDamageable;      // +0x140
    char           _pad2[0x204 - 0x140 - sizeof(Damageable)];
    uint32_t       mHandleId;        // +0x204 - PblHandled generation counter
    char           _pad3[0x234 - 0x208];
    uint32_t       mTeamAndType;     // +0x234 - low 4 bits = team index
};
static_assert(offsetof(GameObject, mCollisionObjVtable) == 0x0C);
static_assert(offsetof(GameObject, mTreeGrid) == 0x10);
static_assert(offsetof(GameObject, mDamageable) == 0x140);
static_assert(offsetof(GameObject, mHandleId) == 0x204);
static_assert(offsetof(GameObject, mTeamAndType) == 0x234);

// Controllable is embedded at this offset from the Entity/GameObject base
constexpr uintptr_t CONTROLLABLE_ENTITY_OFFSET = 0x240;
constexpr uintptr_t DAMAGEABLE_ENTITY_OFFSET   = 0x140;

inline int GetObjectTeam(const GameObject* go) {
    return (int)(go->mTeamAndType & 0xF);
}

inline PblVector3 GetCollisionSpherePos(const GameObject* entity) {
    auto* tgo = &entity->mTreeGrid;
    if (tgo->mStackPtr) {
        auto* pos = reinterpret_cast<const float*>(
            reinterpret_cast<uintptr_t>(tgo->mStackPtr)
            + (tgo->mStackIdx + 4) * 16);
        return { pos[0], pos[1], pos[2] };
    }
    return tgo->mCollisionSphere.mRenderPosition;
}

} // namespace game
