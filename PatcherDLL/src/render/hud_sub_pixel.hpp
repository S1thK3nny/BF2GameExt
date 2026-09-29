#pragma once

#include <stdint.h>

// Sub-pixel interface drawing. The engine rounds every interface element, the
// HUD and the menus alike, to a whole pixel each time it is drawn, so an
// element that moves steadily across the screen jumps a pixel at a time. With
// this on, each element is drawn at its exact position instead: moving
// elements glide, and anything that comes to rest between two pixels is
// filtered across both, so it looks slightly softer.
//
// INI: [Features] HudSubPixel = 0 (stock rounding, the default) or 1.
// Stays set only if the draw was patched. GameExt's own position events (the
// command post markers and the floating target bar) read it: they round to
// whole pixels, as the draw does, while it is off.
extern bool g_hudSubPixel;

void hud_sub_pixel_install(uintptr_t exe_base);
