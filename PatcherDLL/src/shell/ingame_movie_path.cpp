#include "pch.h"
#include "ingame_movie_path.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "lua/lua_hooks.hpp"

#include <detours.h>

#include <stdint.h>
#include <string.h>

// =============================================================================
// Custom in-game movie files (see the header for the user-facing summary)
// =============================================================================
//
// How the engine plays an in-game movie:
//
//   ScriptCB_PlayInGameMovie(file, segment)
//     -> StartInGameMoviePlay(file, segment)          [state 0 only; else queue]
//          sInGameMovieFilename = "Movies\\" + file   <- fixed 256-byte global
//          sInGameMovieIdName   = segment
//          sInGameMoviePlayerState = 1
//     -> (next frame) UpdatInGameMovie, state 1
//          GameMovie::Open(sInGameMovieFilename, sInGameMovieIdName, ...)
//               if (_strnicmp(name, "dc:", 3) == 0) { name += 3; useDLC = true; }
//               LoadUtil::MakeFullName(name, FILE_TYPE_NONE, out, 0x80, useDLC)
//                   useDLC ? "<contentDir>\\Data\\" : "data\\"
//                   ... + "_lvl_pc\\" + name
//
// So GameMovie::Open already understands a "dc:" prefix on every build — but the
// in-game path can never present one, because StartInGameMoviePlay pastes the
// literal "Movies\\" in front of the name first, leaving "Movies\\dc:foo.mvs"
// where the prefix test looks at offset 0.  And the caller never forwards the
// script's filename anyway; it substitutes one of three hardcoded names.
//
// Rather than reimplement the callback (which would pull in the sound-stop,
// rumble-stop and queue-push paths, four more addresses per build), we let the
// original run and then rewrite the filename global it just filled in.  Nothing
// reads that buffer until UpdatInGameMovie ticks on a later frame, so the edit
// lands well before GameMovie::Open sees it.
//
// The rewrite is "Movies\\<name>" or "dc:Movies\\<name>" — the latter is what
// GameMovie::Open needs in order to strip "dc:" and then route the *remaining*
// "Movies\\<name>" through the addon content directory.
// =============================================================================

namespace {

// Names the retail language table can produce.  A script passing one of these is
// asking for the stock behaviour, including the French/German substitution the
// original callback performs, so we leave those calls completely alone.
bool is_stock_movie_name(const char* name)
{
   return _stricmp(name, "ingame.mvs")   == 0 ||
          _stricmp(name, "ingamefr.mvs") == 0 ||
          _stricmp(name, "ingamegr.mvs") == 0;
}

typedef const char* (__cdecl* GetContentDirectory_t)();

// True when <name> is absent from the base game's movie directory but present in
// the active addon's, i.e. when a plain name can only have meant the addon copy.
// PblFile::Exists is just FindFirstFileA, so GetFileAttributesA matches it and
// keeps this build-agnostic.
bool prefer_addon_copy(const char* name)
{
   char path[MAX_PATH];

   _snprintf_s(path, sizeof(path), _TRUNCATE, "data\\_lvl_pc\\Movies\\%s", name);
   if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return false;

   if (!g_addr->dlc_get_content_directory) return false;

   const char* dir =
      ((GetContentDirectory_t)resolve(g_addr->dlc_get_content_directory))();
   if (!dir || !*dir) return false;

   // GetContentDirectory returns an absolute path on real installs; MakeFullName
   // uses it verbatim as the prefix, so build the exact same string here.
   _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\Data\\_lvl_pc\\Movies\\%s", dir, name);
   return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// Overwrite the engine's sInGameMovieFilename with the path the script asked for.
// The buffer is 256 bytes on every build; MakeFullName truncates its own output
// at 0x80 regardless, so anything past that could never have resolved anyway.
void rewrite_movie_filename(const char* wanted)
{
   char name[128];
   strncpy_s(name, sizeof(name), wanted, _TRUNCATE);

   for (char* c = name; *c; ++c)
      if (*c == '/') *c = '\\';

   bool useDLC = false;
   const char* stem = name;
   if (_strnicmp(stem, "dc:", 3) == 0) {
      stem += 3;
      useDLC = true;
   }
   else {
      useDLC = prefer_addon_copy(stem);
   }

   if (!*stem) return;

   char* const dst = (char*)resolve(g_addr->ingame_movie_filename);
   _snprintf_s(dst, 128, _TRUNCATE, "%sMovies\\%s", useDLC ? "dc:" : "", stem);

   auto fn_log = get_gamelog();
   if (fn_log) fn_log("[Movie] in-game movie file: %s", dst);
}

typedef int (__cdecl* fn_play_ingame_movie_t)(lua_State* L);
fn_play_ingame_movie_t original_play_ingame_movie = nullptr;

int __cdecl hooked_play_ingame_movie(lua_State* L)
{
   // Snapshot the requested filename before the original runs — it clobbers
   // nothing on the Lua stack, but reading first keeps the failure modes simple.
   char wanted[128];
   wanted[0] = '\0';

   if (L && g_lua.tolstring && g_lua.gettop && g_lua.gettop(L) >= 2) {
      if (const char* arg = g_lua.tolstring(L, 1, nullptr))
         strncpy_s(wanted, sizeof(wanted), arg, _TRUNCATE);
   }

   uint32_t* const state = (uint32_t*)resolve(g_addr->ingame_movie_player_state);
   const uint32_t stateBefore = *state;

   const int ret = original_play_ingame_movie(L);

   // Only rewrite when this call actually started a movie.  If the player state
   // was already non-zero the original just queued a segment onto the movie
   // that is playing, and the filename global belongs to that earlier call.
   if (stateBefore == 0 && *state == 1 && wanted[0] && !is_stock_movie_name(wanted))
      rewrite_movie_filename(wanted);

   return ret;
}

} // namespace

void ingame_movie_path_install(uintptr_t exe_base)
{
   if (!g_addr->scriptcb_play_ingame_movie) return;
   if (!g_addr->ingame_movie_filename) return;
   if (!g_addr->ingame_movie_player_state) return;

   original_play_ingame_movie =
      (fn_play_ingame_movie_t)resolve(exe_base, g_addr->scriptcb_play_ingame_movie);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_play_ingame_movie, hooked_play_ingame_movie);
   if (DetourTransactionCommit() != NO_ERROR)
      original_play_ingame_movie = nullptr;
}

void ingame_movie_path_uninstall()
{
   if (!original_play_ingame_movie) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_play_ingame_movie, hooked_play_ingame_movie);
   DetourTransactionCommit();
   original_play_ingame_movie = nullptr;
}
