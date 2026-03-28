#pragma once

#include "debug_command.hpp"

// =============================================================================
// HoverSprings — console debug command
//
// Draws wireframe spheres at each hover spring body, colored by compression
// (green=relaxed, red=compressed), with vertical lines showing spring length.
//
// Usage: type "RenderHoverSprings" in the ~ console to toggle.
// =============================================================================

class HoverSprings : public DebugCommand {
public:
   static void install(uintptr_t exe_base);
   static void lateInit();
   static void uninstall();
};
