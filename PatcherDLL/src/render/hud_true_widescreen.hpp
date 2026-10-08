#pragma once

#include <stdint.h>

// =============================================================================
// TrueWidescreen HUD files. A .hud file opts in with AuthoredRatio(w, h) in its
// FileInfo block, the screen shape its numbers were written for, or with
// TrueWidescreen(1), which is AuthoredRatio(4, 3). With one viewport, that
// file's pieces are laid out as on a screen of that ratio and the real height
// and drawn one layout pixel to one screen pixel, without the stock letterbox
// squash or the bitmap stretch, and each top-level piece is slid to keep its
// distance from the edge nearest where it is written: the left third, the
// middle or the right, out on a wider screen and in on a narrower one. A piece
// with ScreenAnchor("Left", "Center" or "Right"), or a share from 0 to 1, moves
// by that instead, wherever it sits. A 4:3
// file is left to the stock layout on a screen no wider than 4:3, which is
// already its own. Positions that follow the world (reticule, lock-on, target
// bar, command post markers) still land on their point. Files without either
// line, stock or modded, draw exactly as before, even beside an opted-in file.
// No INI key: the file decides. See hud_true_widescreen.cpp.
// =============================================================================

void hud_true_widescreen_install(uintptr_t exe_base);
