#pragma once

// =============================================================================
// Runtime build selection.
//
// One DLL supports modtools / Steam / GOG. At init apply_patches() identifies the
// executable and calls game_build_select(); g_addr then points at that build's
// address table. Addresses not yet reverse-engineered for a build are 0 in its
// table, so a feature can check g_addr->some_func before using it (see aim_assist,
// which selects its address set from g_build).
// =============================================================================

#include "game_addrs.hpp"
#include "entity_layout.hpp"

#include <stdint.h>

enum class GameBuild { Unknown, Modtools, Steam, GOG };

#include "game_addrs_table.inc" // struct GameAddrs { ... };

extern GameBuild        g_build; // active build (Unknown until detected)
extern const GameAddrs* g_addr;  // active build's address table (defaults to modtools)

void game_build_select(GameBuild build);

// Early-return from a hook installer unless we're on the modtools build. Used by
// the detour-hook layer (lua_hooks_install), whose targets/inline-patch sites are
// only mapped for modtools.
#define HOOK_REQUIRE_MODTOOLS() \
   do { if (g_build != GameBuild::Modtools) return; } while (0)
