#pragma once

#include <cstdint>

// FillFrom("Right") on a HUD BarBitmap: the bar keeps its right end and grows
// or shrinks at its left, showing the same part of its texture the full bar
// shows there. FillFrom("Left") is the stock behaviour. Inert unless a .hud
// uses the property; no INI setting.
//
// The .hud reader learns the property, and at the end of the bar's load-time
// setup the bar is stored end for end so the stock fill, flash and fades run
// unchanged from the right. See hud_bar_fill_from_core.hpp for the arithmetic
// and docs/RE/HUDSystem.md for the addresses. Modtools, Steam and GOG.
void hud_bar_fill_from_install(uintptr_t exe_base);
