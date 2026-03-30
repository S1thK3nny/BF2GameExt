#pragma once

// Build type enum — shared across all headers.
// Defined here so game struct headers can use build-specific accessors
// without pulling in lua_hooks.hpp.

enum class ExeType : int { UNKNOWN, MODTOOLS, STEAM, GOG };

extern ExeType g_exeType;
