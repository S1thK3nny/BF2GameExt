#pragma once

#include <stdint.h>

// =============================================================================
// Camera shake: the stock shake redrawn as a smooth blast, and ODF-driven shake
// for what the local player does and what happens to them.
//
//   FireShake    weapon, soldier or vehicle   each shot
//   HitShake     any unit                     taking damage
//   LandShake    soldier, flyer               landing a jump or fall; touching down
//   RollShake    soldier, flyer               a combat roll (third person); a trick
//   SprintShake  soldier, flyer               sprinting (third person); boosting
//   BrakeShake   flyer                        slowing down, by how hard
//   BlastShake   any unit                     explosions, walker deaths and crashes
//                                             while Smooth is on; on by default
//
// Each takes the same detail properties after its name: Pitch, Yaw, Roll (in
// degrees; "min max" picks one each time), Push (metres back along the view),
// Length (seconds), Rise (share of the length), Rate (swings per second) and
// Limit (how far repeats pile up, in shakes' worth; for BlastShake, where it
// levels off). The reticule is kept on the unshaken view.
// Anything a class leaves out comes from the defaults in camera_shake_core.hpp.
// Every property inherits through ClassParent; apart from BlastShake, a shake
// does nothing until an ODF sets one of its properties.
//
// The shake moves only the rendered view, after the aim has been taken from the
// unshaken camera, so shots land where they would without it. Client-local, so
// it behaves the same online. Modtools, Steam and GOG. The research and every
// address are in docs/RE/CameraShake.md.
// =============================================================================

// The shakes, in the order of the INI's per-shake strengths.
enum CameraShakeChannel {
   kShakeFire,
   kShakeHit,
   kShakeLand,
   kShakeRoll,
   kShakeSprint,
   kShakeBrake,
   kShakeBlast,
   kCameraShakeChannels
};

// [CameraShake] INI settings, read before install.
extern bool  g_cameraShakeEnabled;   // Enabled:  the ODF-driven shakes
extern bool  g_cameraShakeSmooth;    // Smooth:   the stock shake redrawn as a blast
extern float g_cameraShakeStrength;  // Strength: multiplier on everything drawn here
extern float g_cameraShakeChannel[kCameraShakeChannels];   // FireStrength .. BlastStrength

void camera_shake_install(uintptr_t exe_base);
