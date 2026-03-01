#pragma once

#include <stdint.h>

// Expand TentacleSimulator from 4→9 tentacles using frozen-prefix architecture.
// Original struct (0x268 bytes) kept as-is; extended arrays appended at 0x268.
// New total: 0x778 bytes (0x268 frozen prefix + 0x288 tPos_ext + 0x288 oldPos_ext).
// Must be called while all executable sections are writable.
bool patch_tentacle_limit(uintptr_t exe_base);
