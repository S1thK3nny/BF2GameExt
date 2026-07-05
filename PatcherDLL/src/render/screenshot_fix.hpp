#pragma once

#include <stdint.h>

// Screenshot::RequestScreenshot redirect — port of PrismaticFlower's upstream
// 9a6d4b9.  Print Screen crashes the retail (Steam/GoG) builds; the broken game
// function is replaced with our own backbuffer -> ScreenShots\*.tga writer.
// No-op on modtools (whose screenshot path works).

extern bool g_screenshotFixEnabled;

void screenshot_fix_install(uintptr_t exe_base);
void screenshot_fix_uninstall();
