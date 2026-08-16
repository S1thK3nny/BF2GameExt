#pragma once

#include "debug_command.hpp"

// =============================================================================
// FreecamTargetFix — crash guard, not a console command
//
// Releases the free camera's follow lock when the entity it is following has
// been destroyed.  Without it, spectating a soldier that dies faults on the
// next frame.
//
// No toggle: the check is the safety.
// =============================================================================

class FreecamTargetFix {
public:
   static void install(uintptr_t exe_base);

   // Called from HoverSprings' shared FreeCamera::Update hook, before the
   // original runs.
   static void preFreeCamUpdate();
};
