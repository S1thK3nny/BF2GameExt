#pragma once

#include <stdint.h>

// =============================================================================
// First-Person Animation Bank Override
//
// Hooks EntitySoldierClass::SetProperty to intercept a custom ODF property
// "FirstPersonAnimationBank" and FirstPersonRenderable::UpdateSoldier to swap
// the global mAnim[] array with custom bank animations before the FP state
// machine runs.
//
// Call fp_anim_bank_install()   from lua_hooks_install().
// Call fp_anim_bank_uninstall() from lua_hooks_uninstall().
// Call fp_anim_bank_reset()     from hooked_init_state() (level transitions).
// =============================================================================

void fp_anim_bank_install(uintptr_t exe_base);
void fp_anim_bank_uninstall();
void fp_anim_bank_reset();
