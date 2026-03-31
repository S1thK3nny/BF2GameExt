#pragma once

#include <stdint.h>

// =============================================================================
// First-Person Animation Bank Override + FP Sprint Animation
//
// Hooks EntitySoldierClass::SetProperty to intercept a custom ODF property
// "FirstPersonAnimationBank" and FirstPersonRenderable::UpdateSoldier to swap
// the global mAnim[] array with custom bank animations before the FP state
// machine runs.
//
// FP Sprint: The engine's FP state machine treats sprint as regular run
// (state 1) with scaled playback. This hook detects sprint via entity+0x514
// and swaps the _run animation slot with _sprint (e.g. humanfp_rifle_sprint).
// Sprint animations are optional — if absent, _run plays as before.
// Custom banks also get sprint support: <bankname>_rifle_sprint, etc.
//
// Call fp_anim_bank_install()   from lua_hooks_install().
// Call fp_anim_bank_uninstall() from lua_hooks_uninstall().
// Call fp_anim_bank_reset()     from hooked_init_state() (level transitions).
// =============================================================================

void fp_anim_bank_install(uintptr_t exe_base);
void fp_anim_bank_uninstall();
void fp_anim_bank_reset();
