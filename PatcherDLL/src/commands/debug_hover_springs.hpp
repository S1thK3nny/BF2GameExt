#pragma once

#include <stdint.h>

// =============================================================================
// RenderHoverSprings — console debug command
//
// Draws wireframe spheres at each hover spring body, colored by compression
// (green=relaxed, red=compressed), with vertical lines showing spring length.
//
// Usage: type "RenderHoverSprings" in the ~ console to toggle.
// =============================================================================

// Phase 1 (early): resolve addresses, install Detour hooks.
void debug_hover_springs_install(uintptr_t exe_base);

// Phase 2 (late): register the console command with the engine.
void debug_hover_springs_late_init();

void debug_hover_springs_uninstall();
