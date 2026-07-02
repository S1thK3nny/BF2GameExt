#include "pch.h"
#include "terrain_texture_fix.hpp"
#include "core/resolve.hpp"

#include <detours.h>

// =============================================================================
// Terrain RedTexture* cleanup — port of PrismaticFlower's upstream 4a8d0df.
//
// The terrain shader keeps two cached RedTexture* globals (the null detail map
// and the white fallback).  They're only assigned when a map actually has a
// terrain detail map, so a playlist that switches from a detail-map map to one
// without leaves a stale pointer -> garbage/crash.  Before each ReadTerrain we
// re-resolve both from the texture hash table, so they're always valid.
//
// Upstream patched the single call site; we detour ReadTerrain itself — its
// only external caller is that same site (the other xrefs are internal jumps),
// so the two are equivalent, and the lookups are idempotent besides.
//
// NOTE: upstream's shim used the "null_detailmap" hash for BOTH lookups (a
// copy-paste bug — the white slot got the detail texture).  Fixed here with
// the correct FNV-1a hashes.
// =============================================================================

using fn_ReadTerrain_t = void(__cdecl*)(void* reader);
using fn_HashFind_t    = void*(__cdecl*)(void* table, uint32_t tableSize, uint32_t hash);

static fn_ReadTerrain_t original_ReadTerrain = nullptr;
static fn_HashFind_t    fn_hashFind          = nullptr;
static void*            g_textureTable       = nullptr;
static void**           g_nullDetailTexture  = nullptr;
static void**           g_whiteTexture       = nullptr;

static constexpr uint32_t kTextureTableSize = 0x2000;
static constexpr uint32_t kHashNullDetailMap = 0xba44f106; // "null_detailmap"
static constexpr uint32_t kHashWhite         = 0xde020766; // "white"

static void __cdecl hooked_ReadTerrain(void* reader)
{
    *g_nullDetailTexture = fn_hashFind(g_textureTable, kTextureTableSize, kHashNullDetailMap);
    *g_whiteTexture      = fn_hashFind(g_textureTable, kTextureTableSize, kHashWhite);

    original_ReadTerrain(reader);
}

bool g_terrainTextureFixEnabled = true;

void terrain_texture_fix_install(uintptr_t exe_base)
{
    if (!g_terrainTextureFixEnabled) return;
    if (g_build == GameBuild::Unknown) return;
    if (g_addr->read_terrain == 0 || g_addr->pbl_hash_table_find == 0 ||
        g_addr->tex_hash_table == 0 || g_addr->terrain_null_detail_texture == 0 ||
        g_addr->terrain_white_texture == 0)
        return;

    original_ReadTerrain = (fn_ReadTerrain_t)resolve(exe_base, g_addr->read_terrain);
    fn_hashFind          = (fn_HashFind_t)resolve(exe_base, g_addr->pbl_hash_table_find);
    g_textureTable       = (void*)resolve(exe_base, g_addr->tex_hash_table);
    g_nullDetailTexture  = (void**)resolve(exe_base, g_addr->terrain_null_detail_texture);
    g_whiteTexture       = (void**)resolve(exe_base, g_addr->terrain_white_texture);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)original_ReadTerrain, hooked_ReadTerrain);
    DetourTransactionCommit();
}

void terrain_texture_fix_uninstall()
{
    if (!original_ReadTerrain) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)original_ReadTerrain, hooked_ReadTerrain);
    DetourTransactionCommit();
    original_ReadTerrain = nullptr;
}
