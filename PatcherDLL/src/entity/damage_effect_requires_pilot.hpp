#pragma once

#include <stdint.h>

// =============================================================================
// DamageEffectRequiresPilot: a damage effect that only plays while the vehicle
// has a driver.
//
//     DamageStartPercent        = 100.0
//     DamageStopPercent         = 0.0
//     DamageEffect              = "car_exhaust"
//     DamageAttachPoint         = "hp_exhaust"
//     DamageEffectRequiresPilot = 1
//
// A damage effect group that starts at 100% health is the usual way to give a
// hover or walker a permanent effect (exhaust, thrust), and it plays on a parked
// empty vehicle too. With the property the group is held off while the driver
// seat is empty and comes back when somebody takes it.
//
// How the engine runs damage effects (all builds):
//   GameObjectClass::SetProperty   DamageStartPercent opens a group in the first
//                                  free mDamageEffectClass[10] slot; the lines
//                                  that follow fill that group. A child class
//                                  starts with its parent's group pointers.
//   Damageable::HealthChangeCallback(bool, float)
//                                  for each group: show it when the health ratio
//                                  is at or under mPercent (and not under
//                                  mPercentStop), else stop it. Runs on every
//                                  health change and at spawn, never per frame.
//
// What this adds:
//   - ODF listeners that count the groups of each class the way SetProperty
//     does, so the property marks the group above it. Inherited through
//     ClassParent with the groups themselves.
//   - HealthChangeCallback hook: while a marked group's vehicle has no driver,
//     the group's mPercent reads -1 for that call, which no health ratio is
//     under, so the engine stops the effect and its sound itself. The group is
//     shared by every vehicle of the class, so the value is put back when the
//     call returns.
//   - Damageable::UpdateStatus hook (every vehicle's Update calls it once a
//     frame): when the driver seat of a marked vehicle fills or empties, the
//     base HealthChangeCallback(false, 0) runs again, which is the engine's own
//     way to re-evaluate (SetCurHealth without notify, EntityBuilding::ReadObject).
//
// "Has a driver" is the object's main Controllable (GameObject::GetControllable):
// mPilot set, or a pilot type that never takes one (PILOT_NONE, PILOT_SELF). An
// object without a Controllable always plays the group.
//
//                                         modtools    Steam       GOG
//   Damageable::HealthChangeCallback      0x004CE910  0x004897D0  0x004897D0
//   Damageable::UpdateStatus              0x004CEF40  0x00488910  0x00488910
//
// No INI key: the ODF property is the switch.
// =============================================================================

void damage_effect_requires_pilot_install(uintptr_t exe_base);
void damage_effect_requires_pilot_uninstall();
