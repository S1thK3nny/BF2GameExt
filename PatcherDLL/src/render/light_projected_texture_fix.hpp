#pragma once

#include <stdint.h>

// =============================================================================
// EntityLightClass animated ProjectedTexture fix.
//
// A "light" class label object can project a texture, and that texture can be
// animated the same way water surfaces animate: give the name a frame count and
// a frame rate, and the name becomes a prefix the game appends a frame number
// to.
//
//     ProjectedTexture = "water_specularmask_ 25"
//     FrameRate        = 25
//
// EntityLightClass::SetProperty (PblHash 0x99E6DBFE) allocates the frame array
// and then fills it in a loop -- but the number it hands to the sprintf that
// builds each name is the frame COUNT, not the loop index, so every entry of
// the array resolves to the same texture:
//
//     water_specularmask_25
//     water_specularmask_25
//     water_specularmask_25
//
// instead of _0, _1, _2 ...  The loop index is already sitting in a register
// (EDI on modtools, ESI on retail) and is used two instructions later to pick
// the array slot to store into, so the fix is to feed that register to the
// sprintf instead of the count.  Six bytes, same length, no stub needed.
//
// Credit: found and first fixed by Sleepy in upstream GameExt.
//
// Always on: this is a correctness fix for a feature that cannot work at all
// without it, so there is no INI switch.  A light with a single-frame
// ProjectedTexture never enters the loop and is unaffected either way.
//
// OdfMunge does not pick the frames up for you -- each texture still has to be
// listed in the .req by hand, exactly as with water textures.
// =============================================================================

void light_projected_texture_fix_install(uintptr_t exe_base);
void light_projected_texture_fix_uninstall();
