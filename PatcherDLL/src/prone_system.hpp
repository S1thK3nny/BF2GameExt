#pragma once

#include <stdint.h>

// =============================================================================
// Prone stance system
//
// Wires up the unused PRONE soldier state (SoldierState 2) that Pandemic
// stubbed out before shipping BF2.  The data side (animations, camera
// sections, ODF properties, foley sounds) is already present in the game
// assets — this module provides the code side:
//
//   1. Hooks EntitySoldier::Crouch so the crouch key cycles:
//        STAND -> CROUCH -> PRONE -> STAND
//      The stand-up path reuses the vanilla StandUp function which already
//      has headroom collision checks for prone.
//
//   2. Patches the Controllable vtable Prone slot (offset 0xA0) from the
//      vanilla "return false" stub to a real function that enters prone.
//
//   3. Melee weapon guard: blocks prone entry when holding a melee weapon,
//      and forces out of prone if the soldier switches to one mid-prone.
//
// IMPORTANT: Prone animations must be loaded into the soldier's animation
// bank (simpprone) for the visual result to work.  This module only handles
// state transitions, sounds, and input cycling.
// =============================================================================

void prone_system_install(uintptr_t exe_base);
void prone_system_uninstall();
