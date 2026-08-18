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
// Why addon movies are NOT emitted as "dc:"
// -----------------------------------------
// GameMovie::Open caps MakeFullName's output at 0x80, and GetContentDirectory
// returns an *absolute* path on a real install, so the "dc:" form spends most of
// the budget on the install directory:
//
//   C:\Program Files (x86)\Steam\steamapps\common\Star Wars Battlefront II
//   Classic\GameData\AddOn\mymod\Data\_lvl_pc\Movies\ingame.mvs      = 131 chars
//
// Past 127 the path is silently truncated and the open fails with nothing but
// the engine's "Unable to open movie file" — which prints the *unresolved* name,
// so the log gives no hint that a length limit was involved.  A default Steam
// install blows the cap on its own, before the mod name is even counted.
//
// So we emit the equivalent relative path instead, the same "..\\..\\" climb
// SetLoadDisplayLevel uses: "data\\_lvl_pc\\..\\..\\" collapses to the working
// directory (always GameData), which puts the same file at ~60 characters and
// takes the cap out of play.  The "dc:" form is kept only as a fallback for the
// case the climb cannot express — an addon directory outside the working
// directory, which a real install never has.
//
// GameMovie::Open's failure modes, for reference (Phantom 0x005d2540):
//   * PblFile::Open fails                 -> file is not at the resolved path
//   * RedMoviePlayerBase::OpenHeader fails -> no 'Info' chunk, i.e. not a munged
//                                             .mvs
// The segment is *not* validated there: OpenHeader takes the segment hash and
// never looks it up, it just selects segment 0.  A bad segment name fails later
// and silently, inside GameMovie::Play.  That is why the logging below reports
// on the file only.
// =============================================================================

namespace {

// MakeFullName's outSize at the GameMovie::Open call site, and the usable length
// under it.  Anything longer is truncated in place and cannot open.
constexpr size_t kMakeFullNameCap = 0x80;
constexpr size_t kMaxResolvedLen  = kMakeFullNameCap - 1;

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

// NULL or empty when no addon content is active.
const char* content_directory()
{
   if (!g_addr->dlc_get_content_directory) return nullptr;
   return ((GetContentDirectory_t)resolve(g_addr->dlc_get_content_directory))();
}

// PblFile::Exists is just FindFirstFileA, so GetFileAttributesA matches it and
// keeps this build-agnostic.
bool movie_file_exists(const char* path)
{
   const DWORD attrs = GetFileAttributesA(path);
   return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// Express the active addon directory relative to the working directory, which is
// always GameData — the directory every "data\_lvl_pc\..." path already resolves
// against.  Returns false when there is no addon, or when its absolute path is
// not under the working directory and so cannot be reached by climbing.
bool addon_dir_relative(char* out, size_t outSize, const char** outAbsolute)
{
   const char* dir = content_directory();
   if (outAbsolute) *outAbsolute = dir;
   if (!dir || !*dir) return false;

   char  cwd[MAX_PATH];
   DWORD cwdLen = GetCurrentDirectoryA(sizeof(cwd), cwd);
   if (cwdLen == 0 || cwdLen >= sizeof(cwd)) return false;

   if (_strnicmp(dir, cwd, cwdLen) == 0) {
      const char* rel = dir + cwdLen;
      while (*rel == '\\' || *rel == '/') ++rel;
      // "." keeps the climb well formed if the addon directory *is* the working
      // directory, which collapses back to the base game's own data tree.
      strncpy_s(out, outSize, *rel ? rel : ".", _TRUNCATE);
      return true;
   }

   // Already relative: usable as-is. An absolute path elsewhere is not.
   if (dir[1] == ':' || dir[0] == '\\') return false;

   strncpy_s(out, outSize, dir, _TRUNCATE);
   return true;
}

// -----------------------------------------------------------------------------
// Resolution result
// -----------------------------------------------------------------------------
// `name` is what goes into sInGameMovieFilename; `full` is the on-disk path
// MakeFullName will build from it, which is what a modder actually needs to see.
// `problem` is a finished sentence, empty when the movie is where we expect it.

enum class MovieRoot { Base, Addon, AddonViaPrefix };

struct MoviePath {
   char      name[128];
   char      full[300];
   size_t    fullLen;
   MovieRoot root;
   bool      found;
   bool      tooLong;
   char      problem[320];
};

void set_problem(MoviePath& mp, const char* fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   _vsnprintf_s(mp.problem, sizeof(mp.problem), _TRUNCATE, fmt, ap);
   va_end(ap);
}

// Fill in the base-game form: "Movies\<stem>" -> "data\_lvl_pc\Movies\<stem>".
void set_base_form(MoviePath& mp, const char* stem)
{
   mp.root = MovieRoot::Base;
   _snprintf_s(mp.name, sizeof(mp.name), _TRUNCATE, "Movies\\%s", stem);
   _snprintf_s(mp.full, sizeof(mp.full), _TRUNCATE, "data\\_lvl_pc\\%s", mp.name);
}

// True when <stem> is absent from the base game's movie directory but present in
// the active addon's, i.e. when a plain name can only have meant the addon copy.
bool addon_copy_exists(const char* stem)
{
   char rel[MAX_PATH];
   const char* dir = nullptr;

   if (addon_dir_relative(rel, sizeof(rel), &dir)) {
      char path[300];
      _snprintf_s(path, sizeof(path), _TRUNCATE,
                  "data\\_lvl_pc\\..\\..\\%s\\Data\\_lvl_pc\\Movies\\%s", rel, stem);
      return movie_file_exists(path);
   }

   if (!dir || !*dir) return false;

   char path[300];
   _snprintf_s(path, sizeof(path), _TRUNCATE,
               "%s\\Data\\_lvl_pc\\Movies\\%s", dir, stem);
   return movie_file_exists(path);
}

// Turn the filename a script passed into the pair of paths above, and say what
// is wrong with it if anything is.  Never refuses: an unresolvable request is
// still written through so the script's intent is visible in the engine's own
// warning, with `problem` explaining what we saw.
bool resolve_movie_path(const char* wanted, MoviePath& mp)
{
   memset(&mp, 0, sizeof(mp));

   char work[128];
   strncpy_s(work, sizeof(work), wanted, _TRUNCATE);
   for (char* c = work; *c; ++c)
      if (*c == '/') *c = '\\';

   const bool wantDC = (_strnicmp(work, "dc:", 3) == 0);
   const char* stem  = wantDC ? work + 3 : work;
   while (*stem == '\\') ++stem;
   if (!*stem) return false;

   bool useAddon = wantDC;
   if (!wantDC) {
      // A plain name means the base game unless only the addon has it.
      set_base_form(mp, stem);
      if (movie_file_exists(mp.full)) {
         mp.found   = true;
         mp.fullLen = strlen(mp.full);
         mp.tooLong = mp.fullLen > kMaxResolvedLen;
         return true;
      }
      useAddon = addon_copy_exists(stem);
   }

   if (useAddon) {
      char rel[MAX_PATH];
      const char* dir = nullptr;

      if (addon_dir_relative(rel, sizeof(rel), &dir)) {
         mp.root = MovieRoot::Addon;
         _snprintf_s(mp.name, sizeof(mp.name), _TRUNCATE,
                     "..\\..\\%s\\Data\\_lvl_pc\\Movies\\%s", rel, stem);
         _snprintf_s(mp.full, sizeof(mp.full), _TRUNCATE,
                     "data\\_lvl_pc\\%s", mp.name);
      }
      else if (dir && *dir) {
         // Addon directory is outside the working directory, so the relative
         // climb cannot name it. Hand the engine its own "dc:" form and accept
         // the 0x80 cap; the length check below will say if it does not fit.
         mp.root = MovieRoot::AddonViaPrefix;
         _snprintf_s(mp.name, sizeof(mp.name), _TRUNCATE, "dc:Movies\\%s", stem);
         _snprintf_s(mp.full, sizeof(mp.full), _TRUNCATE,
                     "%s\\Data\\_lvl_pc\\Movies\\%s", dir, stem);
         set_problem(mp, "addon directory \"%s\" is outside the working "
                         "directory, so the movie is reached through \"dc:\" and "
                         "is subject to the engine's %u-character path limit.",
                     dir, (unsigned)kMakeFullNameCap);
      }
      else {
         // "dc:" was asked for but nothing is mounted. Fall back to the base
         // game copy so a movie that is also there still plays.
         set_base_form(mp, stem);
         set_problem(mp, "no addon content is active, so the \"dc:\" prefix "
                         "cannot be resolved; looking in the base game instead.");
      }
   }

   mp.fullLen = strlen(mp.full);
   mp.tooLong = mp.fullLen > kMaxResolvedLen;
   mp.found   = movie_file_exists(mp.full);

   if (mp.tooLong) {
      set_problem(mp, "the resolved path is %u characters, over the %u the "
                      "engine can hold (GameMovie::Open caps it at 0x%X), so it "
                      "is truncated before the file is opened. Shorten the "
                      "install path, the addon folder name or the movie name.",
                  (unsigned)mp.fullLen, (unsigned)kMaxResolvedLen,
                  (unsigned)kMakeFullNameCap);
   }
   else if (!mp.found && !mp.problem[0]) {
      set_problem(mp, "\"%s\" is not there. A custom in-game movie belongs in "
                      "Data\\_LVL_PC\\Movies, and needs the \"dc:\" prefix when "
                      "it ships inside an addon.", mp.full);
   }

   return true;
}

const char* root_label(MovieRoot root)
{
   switch (root) {
   case MovieRoot::Addon:          return "addon";
   case MovieRoot::AddonViaPrefix: return "addon (dc:)";
   default:                        return "base game";
   }
}

// -----------------------------------------------------------------------------
// The hook
// -----------------------------------------------------------------------------

// Filename the movie now playing was started with, for the queue-push warning
// below. Single-threaded game; cleared implicitly by being overwritten.
char g_playingRequest[128] = {};

void rewrite_movie_filename(const char* wanted, const char* segment)
{
   MoviePath mp;
   if (!resolve_movie_path(wanted, mp)) {
      warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__,
                   "[Movie] ScriptCB_PlayInGameMovie(\"%s\", \"%s\"): no filename "
                   "left after the prefix, leaving the engine's own name in place.\n",
                   wanted, segment);
      return;
   }

   char* const dst = (char*)resolve(g_addr->ingame_movie_filename);

   // The engine's buffer is 256 bytes on all three builds, but MakeFullName
   // truncates at 0x80 downstream, so a name long enough to overflow 128 here
   // could never have resolved anyway — mp.fullLen already reported it.
   strncpy_s(dst, sizeof(mp.name), mp.name, _TRUNCATE);

   const bool willPlay = mp.found && !mp.tooLong;

   if (willPlay) {
      get_gamelog()("[Movie] \"%s\" segment \"%s\" -> %s (%s)\n",
                    wanted, segment, mp.full, root_label(mp.root));

      // Playable, but not by the route the script asked for — say so rather than
      // let a silent substitution read as a clean success.
      if (mp.problem[0])
         warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__,
                      "[Movie] \"%s\" did not resolve as written: %s\n",
                      wanted, mp.problem);
      return;
   }

   warn_gamelog(RED_SEVERITY_ERROR, SRC_FILE, __LINE__,
                "[Movie] cannot play \"%s\" (segment \"%s\"): %s\n",
                wanted, segment, mp.problem);

   // The engine's own complaint quotes the name it was handed, which is now our
   // rewritten one and looks nothing like what the script passed. Tie them
   // together so the two log entries are recognisably about the same call.
   warn_gamelog(RED_SEVERITY_ERROR, SRC_FILE, __LINE__,
                "[Movie] the engine will report this as \"Unable to open movie "
                "file %s:%s\"; that message prints the unresolved name, not the "
                "path above.\n", mp.name, segment);
}

typedef int (__cdecl* fn_play_ingame_movie_t)(lua_State* L);
fn_play_ingame_movie_t original_play_ingame_movie = nullptr;

int __cdecl hooked_play_ingame_movie(lua_State* L)
{
   // Snapshot both arguments before the original runs — it clobbers nothing on
   // the Lua stack, but reading first keeps the failure modes simple.
   char wanted[128];
   char segment[128];
   wanted[0] = '\0';
   segment[0] = '\0';

   if (L && g_lua.tolstring && g_lua.gettop && g_lua.gettop(L) >= 2) {
      if (const char* arg = g_lua.tolstring(L, 1, nullptr))
         strncpy_s(wanted, sizeof(wanted), arg, _TRUNCATE);
      if (const char* arg = g_lua.tolstring(L, 2, nullptr))
         strncpy_s(segment, sizeof(segment), arg, _TRUNCATE);
   }

   uint32_t* const state = (uint32_t*)resolve(g_addr->ingame_movie_player_state);
   const uint32_t stateBefore = *state;

   const int ret = original_play_ingame_movie(L);

   if (!wanted[0])
      return ret;

   // Only rewrite when this call actually started a movie.  If the player state
   // was already non-zero the original just queued a segment onto the movie
   // that is playing, and the filename global belongs to that earlier call.
   if (stateBefore != 0) {
      if (g_playingRequest[0] && _stricmp(wanted, g_playingRequest) != 0)
         warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__,
                      "[Movie] segment \"%s\" was queued onto the movie already "
                      "playing (\"%s\"), so \"%s\" is ignored — the engine keeps "
                      "one movie file open at a time.\n",
                      segment, g_playingRequest, wanted);
      return ret;
   }

   if (*state != 1)
      return ret;   // the callback declined to start anything

   if (is_stock_movie_name(wanted)) {
      // Left to the engine on purpose: its language table swaps in ingamefr.mvs
      // / ingamegr.mvs here, which is the whole reason these names pass through.
      // That also means the addon copy of a stock name is unreachable, which is
      // worth saying out loud when one exists.
      if (addon_copy_exists(wanted))
         warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__,
                      "[Movie] \"%s\" is a stock name, so the engine's own "
                      "language table picks the file and the copy in your addon "
                      "is not used. Pass \"dc:%s\" to play that one.\n",
                      wanted, wanted);
      else
         get_gamelog()("[Movie] \"%s\" segment \"%s\" -> stock name, left to the "
                       "engine's language table\n", wanted, segment);

      strncpy_s(g_playingRequest, sizeof(g_playingRequest), wanted, _TRUNCATE);
      return ret;
   }

   rewrite_movie_filename(wanted, segment);
   strncpy_s(g_playingRequest, sizeof(g_playingRequest), wanted, _TRUNCATE);
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
