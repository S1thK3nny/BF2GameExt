#pragma once

#include <stdint.h>

// Missing-bone crash in EntitySoldier::Render.
//
// A soldier class can pin an effect to a named bone (SmolderEffect/SmolderBone
// and its siblings).  Every frame, Render looks the bone up on the model by name
// hash and copies its 4x4 world matrix out of the result.  The lookup returns
// null when the model has no bone by that name, and the copy is not guarded, so
// the game reads 64 bytes from address 0 and dies.
//
// Stock content never hits this because the stock classes only name bones their
// own models have.  A mod does: put an effect on "bone_l_forearm" and let it
// play on a character whose skeleton is shaped differently (General Grievous is
// the reported case) and the game crashes the instant that soldier is drawn.
//
// Fix: skip the slot when the bone is not on the model, which is exactly what
// Render already does for an empty slot two instructions earlier - the guard
// jumps to the engine's own skip target.  The effect simply does not draw on a
// character that has no such bone.  Always on - no INI toggle.

void soldier_bone_effect_null_fix_install(uintptr_t exe_base);
void soldier_bone_effect_null_fix_uninstall();
