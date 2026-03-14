#pragma once

#include <stdint.h>

// =============================================================================
// WeaponDisguise Extension — Custom ODF Property
//
// Adds a new ODF property to WeaponDisguise classes:
//   DisguiseModel = modelname   (swap to a specific GameModel by name)
//   DisguiseModel = " "         (suppress model swap, keep original appearance)
//
// The model swap uses EntityGeometry::mModel (per-instance, no class clone).
// Vanilla drop (SetClass restore) handles cleanup automatically.
//
// NOTE: DisguiseAnimation was investigated but shelved — the SoldierAnimator
// caches all animation data at spawn and has no runtime refresh API.
//
// Call disguise_ext_install()   from lua_hooks_install().
// Call disguise_ext_uninstall() from lua_hooks_uninstall().
// Call disguise_ext_reset()     from hooked_init_state() (level transitions).
// =============================================================================

void disguise_ext_install(uintptr_t exe_base);
void disguise_ext_uninstall();
void disguise_ext_reset();
