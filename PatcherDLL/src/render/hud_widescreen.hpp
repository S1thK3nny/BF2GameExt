#pragma once

#include <stdint.h>

// HUD widescreen reticle correction.
// On a screen wider than 4:3 the HUD loader letterboxes every HUD screen group
// (modtools 0x006B7300 sets each group's local matrix to a y scale and offset
// when the HUD loads), which misaligns the reticle with the 3D aim point (error
// grows toward the screen edges).  This pre-distorts ONLY the reticle Y in
// ReticuleDisplay::Update so it lands on the correct spot after the letterbox;
// all other HUD elements are left untouched.
//
// INI: [Fixes] ReticleCorrection = -1 (auto, scales with aspect ratio),
//      0 to disable, or a manual strength (0..1, full letterbox undo at 1).
extern float g_reticleCorrection;

void hud_widescreen_install(uintptr_t exe_base);
void hud_widescreen_uninstall();

// The reticule y (a fraction of the screen's height, as ReticuleDisplay sends
// it in weaponN.reticule.position) before the correction above: for elements
// drawn without the letterbox (TrueWidescreen HUD files). Unchanged when the
// correction is off.
float hud_widescreen_reticle_uncorrect(float y);
