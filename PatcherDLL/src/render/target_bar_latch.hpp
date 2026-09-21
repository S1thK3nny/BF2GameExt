#pragma once

#include <stdint.h>

// =============================================================================
// A target health bar that floats on the unit and stays on an enemy you hit.
//
// STOCK BEHAVIOUR.  player1.weaponN.target.* describe whatever is under the
// reticle and nothing else.  HUD::GameEvents::UpdateWeaponEvents compares the
// current aim target with a cached handle every tick, and the instant they
// differ it sends target.disable - so the bar fades the moment the reticle
// slips off, which during a fight is most of the time.  There is also no way to
// place a HUD element ON a unit: nothing publishes a unit's screen position.
//
// WHAT THIS ADDS.  Exactly one new HUD event per weapon,
//
//     player1.weapon1.target.position        type_Vector3, 0..1 viewport fractions
//     player1.weapon2.target.position
//
// produced by projecting ONE point: the top centre of the world bounding box.
// Soldiers use non-animated, stance-sized collision bounds with the live centre;
// vehicles/props use model bounds transformed into a world AABB. The anchor is
// pinned inside a built-in screen safe area and snapped to framebuffer
// pixels. Wholly behind-camera targets are hidden, not pinned through the view.
// No animation bones, distance/FOV-dependent sizes, scale events or POI logic.
// Bind EventPosition on the group holding the bar. All artwork sizes, child
// offsets, labels and alignment remain in the .hud file, unchanged by the DLL.
//
// It also makes the ENGINE's target sticky, rather than publishing a parallel
// family of health/name/colour events that would only duplicate target.*:
//
//   * a hit by the LOCAL player on an ENEMY latches that unit;
//   * while a latch is live and nothing is under the reticle, the latched handle
//     is written into Controllable::mReticuleTarget[channel] for the duration of
//     HUD::GameEvents::Update and restored immediately after.  The engine then
//     sees a target, never sends target.disable, and keeps every target.* event
//     flowing with its own shield handling, name lookup and change detection;
//   * aiming at ANY other unit cancels the latch, so the bar never snaps back to
//     someone who may be off screen.  Hitting another enemy transfers it;
//   * aiming at the latched unit keeps refreshing the hold, so the hold always
//     measures time since the unit was last hit OR last under the reticle;
//   * no line of sight, or behind the camera: not lent, but the latch is KEPT, so
//     the bar returns if the unit reappears inside the hold.
//
// The hold is NOT a fade timer.  It bounds how long a hit keeps the bar attached;
// the fade belongs to the .hud file. Living targets continue to be followed
// during that fade. Once the target dies or its handle expires, retain the last
// published SCREEN position instead of recomputing corpse bounds or hiding it
// mid-fade. A new target resets the cached anchor, as does a mission/listener reset.
// A live target with invalid/behind-camera projection still hides normally.
//
// OPT-IN BY DATA.  The whole feature is inert - no lending, no sticky target, no
// per-tick work - unless some .hud element actually binds target.position.  A
// stock HUD, or any mod HUD that does not use the event, behaves exactly as
// before.  EventClass keeps a self-linked handler list at +0x08, so "is anyone
// listening" is a single pointer comparison.
//
// FOR .hud AUTHORS.  Bind position ONLY through EventPosition, never EventEnable:
// the trailing position updates would switch the bar back on in the middle of
// its fade.  Enable and disable stay on the stock target.* events.  Everything
// else bound to target.* (a reticle tint on target.teamColor, say) becomes
// sticky for the hold as well - that is the engine's target, not just the bar's.
//
// MULTIPLAYER.  Presentation only: it reads local state and changes nothing in
// the simulation.  The latch is taken in Damageable::ApplyDamage and NOT in
// Character::RegisterHit, because on a multiplayer client ApplyDamageCommon
// diverts into the cosmetic ApplyNetClientDamage and RegisterHit never runs
// there; ApplyDamage itself runs for locally simulated hits in every role.
//
// Always available on supported builds; no enable toggle or inset INI settings.
// A HUD binding remains the opt-in. The tested screen safe area is built in;
// it reserves space around the anchor without setting bar size.
//   INI: [Features] TargetBarLatchSeconds=2.5      0 = never time out
//
// modtools, Steam and GOG.  Every address, offset and calling convention was
// read per build and independently re-read; the write-up and the per-build
// convention differences are in docs/RE/HUDSystem.md, "Floating elements".
// =============================================================================

extern float g_targetBarLatchSeconds;

// The shared GameEvents hooks also publish player1.reticule.horizonRotation
// (Vector3 degrees, Z only). Bind EventRotation on an unscaled reticule pivot;
// keep artwork sizing in a child. Independent of the target-bar/latch binding,
// with no INI option or controlled-unit requirement.
void target_bar_latch_install(uintptr_t exe_base);
void target_bar_latch_uninstall();
