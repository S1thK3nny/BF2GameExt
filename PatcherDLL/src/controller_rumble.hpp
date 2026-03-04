#pragma once

#include "pch.h"

// =============================================================================
// Controller Rumble — hooks vanilla rumble output stubs + weapon recoil
// =============================================================================
// BF2 (2005) has a complete rumble pipeline from the PS2/Xbox port (regions,
// explosions, weapon recoil state machine) but the two hardware output
// functions are empty RET 4 stubs on PC. We Detours-hook those stubs to
// route the vanilla system's intensity values to XInput vibration.
//
// Weapon fire recoil is handled separately because the vanilla rumble tick
// may not process WeaponClass ODF recoil fields on PC. rumble_on_fire reads
// those per-weapon values and adds them on top of vanilla output.

// Initialize XInput (dynamic load) and install Detours hooks on vanilla
// rumble output stubs + Weapon::Update for charge rumble.
// Requires g_game addresses to be filled first.
// exe_base = loaded image base for address resolution.
void rumble_init(uintptr_t exe_base);

// Unhook, zero motors, and release XInput library.
void rumble_shutdown();

// Per-shot recoil pulse — call from SignalFire hook after debounce.
// Reads WeaponClass ODF recoil fields and triggers a one-shot rumble
// matching the Xbox Weapon_TriggerRecoilRumble behavior.
void rumble_on_signal_fire(void* weapon);

// Global config (set from INI before rumble_init)
extern bool  g_rumbleEnabled;
extern float g_rumbleScale;
