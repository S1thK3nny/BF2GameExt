#pragma once

#include <cstdint>
#include <cstddef>

namespace game {

struct Entity;
struct GameObject;

// -------------------------------------------------------------------------
// Entity_vftable — 12 virtual methods (verified modtools PDB + Steam)
// -------------------------------------------------------------------------

struct Entity_vftable {
    bool           (__thiscall* IsRtti)(void* self, uint32_t hash);                //  0
    uint32_t       (__thiscall* GetDerivedRtti)(void* self);                        //  1
    char*          (__thiscall* GetDerivedRttiName)(void* self);                    //  2
    void*          (__thiscall* Dtor)(void* self, uint32_t flags);                  //  3
    void           (__thiscall* SetProperty)(void* self, uint32_t hash, char* val); //  4
    void           (__thiscall* Init)(void* self);                                  //  5
    Entity*        (__thiscall* GetEntity)(void* self);                             //  6
    Entity*        (__thiscall* GetEntity_const)(void* self);                       //  7
    GameObject*    (__thiscall* GetGameObject)(void* self);                         //  8
    GameObject*    (__thiscall* GetGameObject_const)(void* self);                   //  9
    void*          (__thiscall* GetEntityClass)(void* self);                        // 10
    void*          (__thiscall* GetEntityClass_const)(void* self);                  // 11
};
static_assert(sizeof(Entity_vftable) == 48);

// -------------------------------------------------------------------------
// Entity — base of all entities, just a vtable pointer
// -------------------------------------------------------------------------
// EntityEx extends this with mId and mEntityClass.
// GameObject extends further with collision, damageable, controllable, etc.

struct Entity {
    Entity_vftable* vtable;  // +0x00
};
static_assert(sizeof(Entity) == 0x04);

} // namespace game
