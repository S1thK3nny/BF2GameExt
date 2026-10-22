#pragma once

#include "pch.h"

// =============================================================================
// SDL3 gamepads
// =============================================================================
// The engine reads pads through DirectInput, which gives every controller
// family its own button order and puts both triggers of an Xbox pad on one
// shared axis. With SDL3.dll next to BF2GameExt.dll, every pad SDL recognises
// reaches the engine as a DirectInput pad with one fixed layout instead, and
// the real device is hidden from the engine so it is not read twice:
//
//   Buttons  0 South (A)  1 East (B)  2 West (X)  3 North (Y)
//            4 LB  5 RB  6 Back  7 Start  8 L3  9 R3
//            10 Guide  11 Misc (Share, mic, capture)
//            12 right paddle 1  13 left paddle 1  14 right paddle 2
//            15 left paddle 2  16 touchpad click
//   POV 0    d-pad
//   Axes     X/Y left stick, RX/RY right stick,
//            Z left trigger (positive only), RZ right trigger (positive only)
//
// Pads that SDL does not know, and every pad when SDL3.dll is missing, keep
// the engine's own DirectInput path.

// Hook the function the game's DirectInput8Create import resolves to. All builds.
void gamepad_sdl_install(uintptr_t exe_base);

// True once the engine was handed SDL pads.
bool gamepad_sdl_active();

// Rumble the first pad, motors 0..65535. False when SDL pads are not in use.
bool gamepad_sdl_rumble(uint16_t low, uint16_t high);
