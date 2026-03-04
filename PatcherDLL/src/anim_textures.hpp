#pragma once

#include "pch.h"

// AnimTexture lightsaber system — port of Xbox's AnimTexture1-3 ODF properties.
// Xbox SWBF2 has 3 extra properties on WeaponMelee blades that give lightsaber
// blades a 4-frame animated texture cycle. PC has no equivalent.
//
// This module intercepts SetProperty to capture AnimTexture hashes and hooks
// _RenderLightsabre to cycle the blade texture at render time.

void anim_textures_install(uintptr_t exe_base);
void anim_textures_uninstall();
