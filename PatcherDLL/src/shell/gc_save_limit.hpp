#pragma once

#include <stdint.h>

struct lua_State;

// Galactic Conquest save limit.
//
// ScriptCB_SaveMetagameState packs its values into a fixed 512 byte zlib
// buffer. A state that did not fit was cut off and saved anyway, and loaded
// back as garbage or a crash. The buffer now holds 16 KB. A state that fits
// 512 bytes is still saved in the stock 516 byte .gc file, so the game without
// BF2GameExt loads it; a bigger one makes a bigger file that needs BF2GameExt.
// Saving and loading are bounded on every save type, and a state that does not
// fit is refused with an error instead of cut off. Always on.

void gc_save_limit_install(uintptr_t exe_base);
void gc_save_limit_uninstall();

// GetMetagameSaveSize(): compressed bytes of the current GC state, and the limit.
int lua_GetMetagameSaveSize(lua_State* L);
