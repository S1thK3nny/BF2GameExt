#pragma once

#include <stdint.h>

// HUD widescreen reticle correction.
// The vanilla letterbox transform in HUD::Manager::Update applies a Y-scale and
// Y-offset to every HUD element on widescreen displays, which misaligns the
// reticle with the 3D aim point (error grows toward the screen edges).  This
// pre-distorts ONLY the reticle Y in ReticuleDisplay::Update so it lands on the
// correct spot after the letterbox transform; all other HUD elements are left
// untouched.
//
// INI: [Fixes] ReticleCorrection = -1 (auto, scales with aspect ratio),
//      0 to disable, or a manual strength (0..1, full letterbox undo at 1).
extern float g_reticleCorrection;

void hud_widescreen_install(uintptr_t exe_base);
void hud_widescreen_uninstall();
