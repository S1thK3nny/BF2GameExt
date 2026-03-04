#pragma once

#include <stdint.h>

// Increase LOD manager heap counts and cost budgets for all 4 LOD classes,
// expand the far-scene object heap, and hook RenderFarObjects to use a
// larger static array (1024 vs vanilla 256).
// Must be called while all executable sections are writable.
bool patch_lod_limits(uintptr_t exe_base);

// Detach Detours hooks. Call from BF2GameExt_Shutdown().
void unpatch_lod_limits();
