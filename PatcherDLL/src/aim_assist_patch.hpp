#pragma once

#include "pch.h"

// Aim assist / sticky reticle — ported from Xbox SWBF2.
// Hooks PlayerController::Update to apply aim friction (slowdown near targets)
// and steering (pull toward locked target) when using a controller.
//
// Gated on: [AimAssist] Enabled=1 in INI, controller enabled, singleplayer/host only.

void aim_assist_install(uintptr_t exe_base);
void aim_assist_uninstall();

// Called from dllmain after INI is loaded to read [AimAssist] config
void aim_assist_load_config(const char* ini_path);
