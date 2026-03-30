#pragma once

#include <cstdint>
#include <cstddef>
#include "../pbl/PblListNode.hpp"

namespace game {

// Factory<Entity, EntityClass, EntityDesc> — class registry entry
// Source: c:\battlefront2\main\battlefront2\source\Factory.h
//
// Each ODF-defined class (e.g. "rep_inf_ep3_rifleman") creates a Factory
// instance that is linked into a global PblList (Factory::sList).
// The Factory IS the EntityClass base — mId is the PblHash of the ODF name.
//
// Global list head (PblList<Factory<Entity,EntityClass,EntityDesc>>):
//   Modtools: 0x00ACD2C4
//   Steam:    0x007EC55C
//   GOG:      0x007ED4EC
struct Factory {
    void*                  vtable;      // +0x00
    PblListNode<Factory>   mNode;       // +0x04  (16 bytes) — linked into Factory::sList
    Factory*               mParent;     // +0x14
    uint32_t               mId;         // +0x18  PblHash of ODF name
    uint32_t               mNetIndex;   // +0x1C
    // EntityClass-specific fields follow at +0x20 ...
};
static_assert(offsetof(Factory, mNode)     == 0x04);
static_assert(offsetof(Factory, mParent)   == 0x14);
static_assert(offsetof(Factory, mId)       == 0x18);
static_assert(offsetof(Factory, mNetIndex) == 0x1C);

} // namespace game
