#pragma once

#include <stdint.h>

// =============================================================================
// Lightsaber illumination - give ignited saber blades a real dynamic light.
//
//   [Lightsaber]
//   LightsaberIllumination     = 1     ; master switch (on by default)
//   LightsaberLightRadius      = 4.0   ; metres, at full blade extension
//   LightsaberLightIntensity   = 1.0   ; multiplier on the blade colour
//
// Stock blades are additive particle billboards only - they emit nothing. This
// attaches a RedOmniLight to each ignited blade, coloured from that blade's own
// LightSaberTrailColor, so no ODF edits are needed for stock content.
//
// Modtools only for now; the retail address sets are unfilled, so the installer
// no-ops there.
// =============================================================================

void lightsaber_illumination_install(uintptr_t exe_base);
void lightsaber_illumination_uninstall();

extern bool  g_lightsaberIlluminationEnabled;
extern float g_lightsaberLightRadius;
extern float g_lightsaberLightIntensity;
