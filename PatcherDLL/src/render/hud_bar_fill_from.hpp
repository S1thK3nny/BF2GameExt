#pragma once

#include <cstdint>

// FillFrom on a HUD BarBitmap: which end the bar keeps.
//   "Left"    the stock fill: the left end stays, the right edge moves.
//   "Right"   the right end stays and the bar grows or shrinks at its left.
//   "Bottom"  the bottom stays and the bar grows upward.
//   "Top"     the top stays and the bar grows downward.
// Every mode shows the same part of the texture the full bar shows at each
// point, with no rotation. Inert unless a .hud uses the property; no INI
// setting.
//
// "Right" stores the bar end for end at load so the stock fill, flash and
// fades run unchanged from the right. The vertical modes take the stock fill
// off the bar at load and lay it out from SetValue instead; they have no
// flash. See hud_bar_fill_from_core.hpp for the arithmetic and
// docs/RE/HUDSystem.md for the addresses. Modtools, Steam and GOG.
void hud_bar_fill_from_install(uintptr_t exe_base);

// Called when the HUD opens (GameEvents::Open): forgets the last HUD's bars.
void hud_bar_fill_from_open();
