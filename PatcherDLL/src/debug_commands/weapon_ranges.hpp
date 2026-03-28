#pragma once

#include "debug_command.hpp"

// =============================================================================
// WeaponRanges — console debug command
//
// Draws flat range circles around soldiers showing weapon AI ranges:
//   Red    = MinRange
//   Green  = OptimalRange
//   Blue   = MaxRange
//   Yellow = AI comfort band (inner / outer)
//
// Usage: type "ShowWeaponRanges" in the ~ console to toggle.
// =============================================================================

class WeaponRanges : public DebugCommand {
public:
   static void install(uintptr_t exe_base);
   static void lateInit();
   static void uninstall();

   // Called from HoverSprings' shared FreeCamera::Update hook
   static void freecamTick();
};
