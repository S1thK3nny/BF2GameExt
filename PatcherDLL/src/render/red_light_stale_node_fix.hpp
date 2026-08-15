#pragma once

#include <stdint.h>

// =============================================================================
// Freecam light stale-pointer crash fix (+ a RedLight::Deactivate backstop).
//
// THE CRASH
//   `freecamlight.enable 1` -> reload the map -> `freecamlight.enable 0` is an
//   access violation.  The light's 0x120-byte block comes from
//   RedOmniLight::sMemoryPool and does not survive the level change, but
//   `s_pFreeCamLight` is never cleared: it is referenced from exactly two places
//   in the binary, SetFreeCamLightCallback and FreeCamera::Update, and neither
//   touches it on level load or unload.
//
//   So on the new map the disable path does
//
//       mov ecx,[s_pFreeCamLight]   ; still the old block
//       mov eax,[ecx]               ; "vtable", actually recycled memory
//       call [eax+8]                ; RedLight::Deactivate -> anywhere
//
//   Observed both ways in the wild: EAX = 0xCDCDCD00 (the block is fresh debug
//   -CRT heap fill) and EAX = block+0x120 (the block is on the pool free list,
//   so its first dword is the free-list link).  The re-enable is also a silent
//   no-op, because the enable path opens with `if (s_pFreeCamLight != 0) return;`.
//
// THE FIX
//   Zero `s_pFreeCamLight` on every mission start, from the same init_state hook
//   the other per-level resets use.  Disable becomes a clean no-op and enable
//   allocates a fresh light, so the light works again after a reload instead of
//   silently doing nothing.
//
//   modtools only: retail strips the whole freecamlight command family, and
//   SetFreeCamLightCallback with it, so the pointer can never become non-null
//   there.
//
// THE BACKSTOP
//   Separately, RedLight::Deactivate unlinks its first list node with no guard
//   beyond the light's own "linked" flag, which the engine's list drain does not
//   clear.  Any light that outlives a drain and is then deactivated writes two
//   pointers into freed memory.  Our saber lights are static DLL storage that
//   outlives a level change, which is exactly why lightsaber_illumination.cpp
//   carries its own still_linked() test; this installs the same test one level
//   lower so it also covers anything long-lived added later.  Byte-identical
//   function on all three builds, so it installs everywhere.
// =============================================================================

void red_light_stale_node_fix_install(uintptr_t exe_base);
void red_light_stale_node_fix_uninstall();

// Per-mission reset. Called from lua_hooks' init_state hook alongside the other
// stale-pointer resets.
void freecam_light_reset();
