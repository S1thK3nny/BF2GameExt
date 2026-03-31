#pragma once

#include <stdint.h>

// =============================================================================
// Aim assist — ported from Xbox Controllable::UpdateTargetLockedObjTracking
//
// Hooks PlayerController::Update to apply:
//   1. Proximity friction — omnidirectional stick slowdown near enemies
//   2. Auto-tracking — camera pull toward locked target
//   3. Directional friction — stick resistance when moving away from lock
//   4. Auto-lock-on-hit — first damage shot acquires lock
//   5. Lock break — sustained push away from target clears lock
//
// Gated on: [AimAssist] Enabled in INI, controller connected, singleplayer.
// =============================================================================

void aim_assist_load_config(const char* ini_path);
void aim_assist_install(uintptr_t exe_base);
void aim_assist_uninstall();
