#pragma once
#include <cstdint>

// Extra soldier override-texture slots: OverrideTexture3, OverrideTexture4,
// OverrideTexture5 (the stock engine hardcodes only OverrideTexture / OverrideTexture2).
// See soldier_override_texture.cpp for how it works.

void soldier_override_texture_install(uintptr_t exe_base);
void soldier_override_texture_uninstall();
void soldier_override_texture_reset();
