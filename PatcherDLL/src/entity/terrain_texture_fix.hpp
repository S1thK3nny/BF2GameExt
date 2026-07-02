#pragma once

#include <stdint.h>

extern bool g_terrainTextureFixEnabled;

// Re-resolves the terrain shader's cached detail/white RedTexture* on every
// terrain load, fixing stale pointers when a playlist map lacks a detail map
// (crash/garbage terrain).  Port of PrismaticFlower's "Terrain RedTexture*
// Cleanup" (upstream 4a8d0df).  Detours ReadTerrain (its single external
// caller matches upstream's call-site patch).  Build-aware; no-ops where the
// addresses aren't mapped.
void terrain_texture_fix_install(uintptr_t exe_base);
void terrain_texture_fix_uninstall();
