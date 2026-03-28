#pragma once

#include "core/resolve.hpp"
#include "core/game_addrs.hpp"

#include <stdint.h>

// =============================================================================
// DebugCommand — base class for all debug visualization commands.
//
// Centralizes engine function types, shared resolved pointers, and helpers.
// Each command inherits from this to access the shared engine interface.
// =============================================================================

class DebugCommand {
public:
   // ---- Engine Drawing -------------------------------------------------------

   using DrawLine3D_t    = void(__cdecl*)(float, float, float, float, float, float, uint32_t);
   using DrawSphere_t    = void(__cdecl*)(float, float, float, float, uint32_t);
   using Printf3D_t      = void(__cdecl*)(const float*, const char*, ...);

   // ---- Physics / Collision --------------------------------------------------

   using FindBody_t      = int(__fastcall*)(int collModel, void* edx, int bodyId);
   using GetWorldXform_t = void(__fastcall*)(int bodyPtr, void* edx, float* out, const float* entMat, int flags);
   using GetRadius_t     = float(__fastcall*)(int bodyPtr, void* edx);

   // ---- Shared resolved pointers (set by initEngine) -------------------------

   static DrawLine3D_t    drawLine3D;
   static DrawSphere_t    drawSphere;
   static Printf3D_t      printf3D;
   static FindBody_t      findBody;
   static GetWorldXform_t getWorldXform;
   static GetRadius_t     getRadius;

   // Resolve all shared engine pointers — call once from DebugCommandRegistry::install()
   static void initEngine(uintptr_t exe_base);

   // ---- Helpers --------------------------------------------------------------

   // RedColor: b | g<<8 | r<<16 | a<<24 on little-endian x86
   static uint32_t makeColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
   {
      return (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | ((uint32_t)a << 24);
   }
};
