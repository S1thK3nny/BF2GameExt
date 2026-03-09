#pragma once

#include <stdint.h>

// Fix flyer path-following engine sound stutter.
//
// During path following, the speed value fed to VehicleEngine::Update
// oscillates due to Catmull-Rom parametric speed variation and the
// speed derivative amplifies frame-to-frame noise.  This makes the
// engine sound stutter constantly.
//
// Fix: Detours hook on VehicleEngine::Update that applies exponential
// moving average smoothing to speedRatio (arg5) and acceleration (arg6)
// before calling the original.

void identify_flyer_sound(uintptr_t exe_base);
void flyer_sound_install();
void flyer_sound_uninstall();
