#pragma once

#include <stdint.h>

// BlurEffect::Render downsize clamp — port of PrismaticFlower's upstream 1f8f618.
// Clamps the blur effect's downsized render-target resolution to 512px so the
// blur doesn't get sharper (and costlier) at high resolutions.

extern bool g_blurDownsizeClampEnabled;

void blur_downsize_clamp_install(uintptr_t exe_base);
void blur_downsize_clamp_uninstall();
