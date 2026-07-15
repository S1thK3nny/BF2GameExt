#pragma once

#include <cstdint>

// Flyer path-following engine-sound stutter fix.
//
// During path following the speedRatio/acceleration values fed to
// VehicleEngine::Update jitter frame-to-frame (Catmull-Rom parametric speed
// variation, amplified by the speed derivative), which makes AI flyer engine
// audio stutter/warble.  flyer_sound_install() detours VehicleEngine::Update
// and EMA-smooths both values before the original runs.  Always on; no INI
// toggle.  Free flight is effectively unaffected (tau = 0.05s).

void flyer_sound_install(uintptr_t exe_base);
void flyer_sound_uninstall();
