#pragma once

#include <stdint.h>

// EntityHover self-piloted crash fix.
//
// EntityHover::UpdateIndirect (the hover's AI obstacle-avoidance update) fetches
// the vehicle's pilot and immediately calls pilot->GetGameObject() with NO null
// check, to add it to a collision-raycast ignore list.  Every stock hover is
// soldier-entered (PilotType=vehicle) so the pilot is always valid — but a
// self-piloted hover (PilotType=self) has no separate pilot, the getter returns
// null, and the game dereferences it (AV read [pilot+0x18]).  Crashes in single-
// player and multiplayer alike (it's the AI-drive path, not netcode).
//
// Fix: redirect the getter CALL through a shim that substitutes the hover's own
// Controllable when the getter returns null, so GetGameObject() resolves to the
// hover itself.  Always on — no INI toggle.

void hover_pilot_null_fix_install(uintptr_t exe_base);
void hover_pilot_null_fix_uninstall();
