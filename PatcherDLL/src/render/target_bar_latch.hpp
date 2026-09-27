#pragma once

#include <stdint.h>

// Floating target bars + selection retention, using the engine's target.* data.
//
// EventPosition("player1.weaponN.target.position") opts a channel into this
// feature. Without a listener, the stock HUD behaves as before. The Vector3 is
// viewport-relative: top-centre of the world bounds, pixel-snapped. It is pinned
// inside the screen while the engine picks the target, so a big vehicle up close
// keeps its bar; held or fading, the bar leaves the screen with its target.
// Soldier bounds are non-animated, stance-sized collision bounds at the live
// centre; vehicles/props use model bounds transformed into a world AABB.
// Dimensions, artwork alignment, child offsets and labels belong to the .hud.
//
// SELECTION RETENTION:
//   * sample weapon->mTarget, else the native reticule handle, before lending;
//   * acquire/refresh only when the engine's filtered HUD target agrees;
//   * hold the most recently selected target for 0.5s after selection loss;
//   * a different natural target replaces it immediately, even a friendly,
//     except the other half of a vehicle and its exposed rider: that must
//     hold the engine's pick for 0.3s, since the two trade it back and forth;
//   * a lent/cached target cannot refresh its own hold;
//   * no affiliation check or fresh LOS/range/frustum gate;
//   * keep separate state for weapon1/2; clear on death/stale handles, weapon or
//     controlled-object changes, mission reset or removal of that listener.
//
// During the hold, lend the retained handle to the empty reticule slot only
// inside HUD::GameEvents::Update, restoring it immediately afterward. The
// engine continues publishing health/shield/name/colour with its native
// filtering and change detection. Other target.* consumers share the hold.
// The rider pair lends the same way, over the engine's pick; aim assist,
// lock-on and firing never see either loan.
//
// Hold expiry stops lending; native target.disable then starts the HUD fade.
// Use FadeInTime(0) and FadeOutTime(0.25) on the existing bar/glow/label elements
// for reference-style timing. Do NOT bind position to EventEnable, since its
// trailing updates would keep re-enabling the fade.
//
// Position/death-fade policy: follow living targets through the HUD fade;
// retain the last live WORLD bounds on death/despawn and reproject them using
// the current camera, never corpse bounds. This intentionally preserves the
// user's death fade, rather than adopting the reference's immediate zero-health
// hide. Wholly behind-camera targets hide, as before.
//
// INI: [Features] TargetBarLatchSeconds=0.5. Existing overrides still apply;
// zero retains the legacy no-timeout option. Fade duration stays in the .hud.
// This ports the retention behaviour, not the reference's autoaim selector.
// Multiplayer presentation only; no simulation or network state is changed.
//
// Supported builds: modtools, Steam and GOG. See docs/RE/HUDSystem.md.
extern float g_targetBarLatchSeconds;

// The same Open/Update hooks also drive horizonRotation, NumberMath, the class
// icons (hud_class_icons.hpp), the command post strip (hud_command_posts.hpp)
// and FillFrom's Open step. Independent HUD bindings opt into those features.
void target_bar_latch_install(uintptr_t exe_base);
void target_bar_latch_uninstall();
