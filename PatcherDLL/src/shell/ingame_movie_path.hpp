#pragma once

#include <stdint.h>

// =============================================================================
// Custom in-game movie files for ScriptCB_PlayInGameMovie
// =============================================================================
// Stock Lua:
//     ScriptCB_PlayInGameMovie("ingame.mvs", "hotmon01")
//
// The dev build honoured that first argument.  Every shipping build (modtools,
// Steam, GOG) rewrote the callback to *ignore* it and pick the movie file from a
// hardcoded language table instead — ingame.mvs, or ingamefr.mvs / ingamegr.mvs
// on French / German.  Only argument 2 (the segment) is read.  That is why a
// custom in-game movie has only ever worked by overwriting the stock ingame.mvs:
// no other filename could ever reach GameMovie::Open.
//
// This hook restores the argument, and adds addon ("dc:") resolution on top:
//
//     ScriptCB_PlayInGameMovie("mymovie.mvs",    "seg")  -- Data\_LVL_PC\Movies
//     ScriptCB_PlayInGameMovie("dc:mymovie.mvs", "seg")  -- <addon>\Data\_LVL_PC\Movies
//
// A bare custom name that is missing from the base game but present in the
// active addon is redirected to the addon automatically, so a mod can just ship
// its movie next to its own level data and pass the plain name.
//
// Addon movies are handed to the engine as a relative climb out of
// data\_LVL_PC rather than as a "dc:" path, because GameMovie::Open caps the
// resolved path at 128 characters and the absolute addon directory alone can
// exceed that on a default Steam install. See the .cpp for the full reasoning.
//
// The three stock names are still routed the old way, so the French and German
// campaign movies keep working exactly as before.
//
// Every call is logged to BF2GameExt.log with the path the engine will actually
// open, and an unopenable movie is reported with the reason — missing file,
// no addon mounted, or a path over the engine's limit. The engine's own
// "Unable to open movie file" prints the *unresolved* name and never says which
// of those it hit.
//
// Installed for all three builds; no-ops where the addresses are unknown.

void ingame_movie_path_install(uintptr_t exe_base);
void ingame_movie_path_uninstall();
