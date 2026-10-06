#pragma once

#include <stdint.h>

// =============================================================================
// TrueWidescreen HUD files. A .hud file opts in with TrueWidescreen(1) in its
// FileInfo block. On a screen wider than 4:3, with one viewport, that file's
// pieces are laid out as on a 4:3 screen of the same height and drawn one
// layout pixel to one screen pixel, without the stock letterbox squash or the
// bitmap stretch, and each top-level piece is slid to keep its distance from
// the edge nearest where it is written: the left third, the middle or the
// right. Positions that follow the world (reticule, lock-on, target bar, command
// post markers) still land on their point. Files without the line, stock or
// modded, draw exactly as before, even beside an opted-in file. No INI key: the
// file decides. See hud_true_widescreen.cpp.
// =============================================================================

void hud_true_widescreen_install(uintptr_t exe_base);
