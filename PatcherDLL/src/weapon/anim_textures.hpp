#pragma once

#include <stdint.h>

// Animated lightsaber blade textures — port of Xbox SWBF2's AnimTexture1-3
// WeaponMelee blade properties.  Xbox blades cycle through a 4-frame animated
// texture; PC uses a single static texture.  This intercepts WeaponMeleeClass
// property parsing to capture the AnimTexture hashes and hooks _RenderLightsabre
// to cycle the blade texture at render time.  The blade struct is not modified —
// all animation data lives in an external map keyed by the base texture hash.
//
// Always on (no INI toggle): AnimTexture1-3 are inert unless a WeaponMelee ODF
// declares them, so there is nothing to disable.
void anim_textures_install(uintptr_t exe_base);
void anim_textures_uninstall();
