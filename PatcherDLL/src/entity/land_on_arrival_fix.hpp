#pragma once

#include <cstdint>

// LandOnArrival path-node property fix (EntityFlyer path following).
//
// Two engine bugs make the .pth NodeProperties LandOnArrival flag near-useless:
//   1. EntityPathFollower::Update only honors it on the first node (mIndex < 1).
//   2. mbLandNow is never cleared after EntityFlyer::Land, so the flyer
//      re-lands every frame and can never take off again.
//
// land_on_arrival_install() NOPs the index gate and detours Land() to clear
// mbLandNow + release the path follower.  Always on; no INI toggle.

void land_on_arrival_install(uintptr_t exe_base);
void land_on_arrival_uninstall();
