#pragma once

#include <stdint.h>

// Chunk section crash fixes.
//
// A CHUNKSECTION whose ChunkGeometryName is not loaded, or is loaded without any
// collision, crashed the game while the ODF loaded, and so did chunk properties
// in a section that never got a ChunkGeometryName. The same goes for an
// explosion's missing chunk geometry. Those chunks are now left out with an
// error in the log that names the geometry and section. Always on.

void chunk_section_guard_install(uintptr_t exe_base);
void chunk_section_guard_uninstall();
