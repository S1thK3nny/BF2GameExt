#pragma once

#include <stdint.h>

// Fixes HUD reticle vertical misalignment on widescreen displays.
// Pre-distorts the reticle Y position in ReticuleDisplay::Update so that
// after the vanilla letterbox transform, it lands on the correct 3D aim point.
// Only affects the reticle — all other HUD elements are untouched.
// INI: [Patches] ReticleCorrection=-1 (auto), 0 to disable, or manual float
void patch_hud_widescreen(uintptr_t exe_base, float correction_strength = 1.0f);
void unpatch_hud_widescreen();
