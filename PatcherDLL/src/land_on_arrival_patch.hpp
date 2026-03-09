#pragma once

#include <stdint.h>

// Fix LandOnArrival path node property.
//
// Two bugs prevent LandOnArrival from working:
//   1. A JG instruction gates the mLandOnArrival check to mIndex==0 only.
//   2. mbLandNow is never cleared after Land() is called, so the flyer
//      re-lands every frame and can never take off again.
//
// identify_land_on_arrival() locates both patch sites during init.
// The Lua function EnableLandOnArrival(bool) toggles the JG NOP at runtime.
// The Land() Detours hook is always active to clear mbLandNow after landing.

void identify_land_on_arrival(uintptr_t exe_base);
void land_on_arrival_install();
void land_on_arrival_uninstall();

// Globals for the Lua toggle (JG patch)
extern uint8_t* g_landOnArrivalPatch;
extern uint8_t  g_landOnArrivalOrigJG;
