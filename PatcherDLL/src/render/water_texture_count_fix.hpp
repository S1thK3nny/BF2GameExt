#pragma once

#include <stdint.h>

// RedWater animated-texture count overflow fix.  Clamps the frame count taken
// from a world's Water() NormalMapTextures / BumpMapTextures /
// SpecularMaskTextures properties to the 50 entries the engine's static tables
// actually hold.  See docs/RE/RedWaterTextureArrays.md.

void water_texture_count_fix_install(uintptr_t exe_base);
void water_texture_count_fix_uninstall();
