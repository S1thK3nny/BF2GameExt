#pragma once

#include <stdint.h>

// Expand TentacleSimulator from 4→9 tentacles using Detours hooks.
// Constructor and DoTentacles are reimplemented in C++; remaining binary
// patches (pool size, bitfield, render stacks) are applied as value replacements.
// Must be called while all executable sections are writable.
bool patch_tentacle_limit(uintptr_t exe_base);

// Detach Detours hooks. Call from BF2GameExt_Shutdown().
void unpatch_tentacle_limit();
