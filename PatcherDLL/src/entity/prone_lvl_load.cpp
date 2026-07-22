#include "pch.h"
#include "prone_lvl_load.hpp"
#include "soldier_prone.hpp"
#include "core/resolve.hpp"
#include "lua/lua_hooks.hpp"

#include <string.h>
#include <detours.h>

// =============================================================================
// Prone LVL auto-load — see prone_lvl_load.hpp for the rationale.
//
// Two engine functions are involved:
//
//   Lua_Callbacks::ReadDataFile  (modtools 0x0046a790 / Steam 0x0058ac50)
//     The lua_CFunction bound to the script-side ReadDataFile().  Plain cdecl
//     on both builds, so it detours normally.
//
//   LoadUtil::ReadDataFile       (modtools 0x004538b0 / Steam 0x00579c30)
//     bool ReadDataFile(name, heapArg, hashes, hashCount, lvlNames, lvlCount)
//     — the actual level reader; returns false when the file can't be opened
//     (clean failure, no fatal dialog: it just tears down its PblFile and
//     returns).  Calling conventions differ:
//       modtools — cdecl, six stack args, all zero for a plain whole-file read.
//       Steam    — LTCG: name arrives in ECX, four caller-cleaned stack args
//                  follow (the first of which the function never reads), and
//                  it RETs 0.  Needs the naked thunk below; declaring it
//                  __fastcall would make the compiler assume callee cleanup
//                  and leak 16 bytes of stack per call.
// =============================================================================

static const char kIngameLvl[] = "ingame.lvl";
static const char kProneLvl[]  = "prone.lvl";

using fn_lua_cb_t = int(__cdecl*)(lua_State* L);
using fn_read_data_file_mt_t =
    bool(__cdecl*)(const char* name, int a2, int a3, void* a4, unsigned a5, void* a6);

static fn_lua_cb_t original_lua_read_data_file = nullptr;
static uintptr_t   s_read_data_file            = 0;   // LoadUtil::ReadDataFile

// [Features] Prone as configured in the INI.  g_proneEnabled is the *live*
// flag and we clear it when prone.lvl is missing, so the configured value has
// to be remembered separately to allow a later mission to re-enable prone.
static bool s_proneConfigured = false;

// ---------------------------------------------------------------------------
// LoadUtil::ReadDataFile — per-build call shims
// ---------------------------------------------------------------------------

__declspec(naked) static bool __cdecl call_read_data_file_steam(const char* /*name*/)
{
   __asm {
      push ebp
      mov  ebp, esp
      push 0                          // lvlNames
      push 0                          // lvlCount
      push 0                          // hashes
      push 0                          // (unused by the callee)
      mov  ecx, [ebp + 8]             // name
      call dword ptr [s_read_data_file]
      add  esp, 16                    // caller-cleaned
      mov  esp, ebp
      pop  ebp
      ret
   }
}

static bool read_data_file(const char* name)
{
   if (!s_read_data_file) return false;

   if (g_build == GameBuild::Modtools)
      return ((fn_read_data_file_mt_t)s_read_data_file)(name, 0, 0, nullptr, 0, nullptr);

   return call_read_data_file_steam(name);
}

// ---------------------------------------------------------------------------
// hooked_lua_read_data_file — read prone.lvl on the heels of every ingame.lvl
// ---------------------------------------------------------------------------

static int __cdecl hooked_lua_read_data_file(lua_State* L)
{
   // Read argument 1 before running the original: the callback leaves the
   // stack intact, but reading it up front keeps us independent of that.
   const char* name = (L && g_lua.tolstring) ? g_lua.tolstring(L, 1, nullptr) : nullptr;
   const bool  isIngame = name && _stricmp(name, kIngameLvl) == 0;

   const int result = original_lua_read_data_file(L);

   if (isIngame && s_proneConfigured) {
      const bool ok = read_data_file(kProneLvl);
      g_proneEnabled = ok;

      if (auto fn_log = get_gamelog()) {
         if (ok)
            fn_log("[Prone] Loaded %s alongside %s\n", kProneLvl, kIngameLvl);
         else
            fn_log("[Prone] %s not found (data\\_lvl_pc\\) — prone disabled for this mission\n",
                   kProneLvl);
      }
   }

   return result;
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------

void prone_lvl_load_install(uintptr_t exe_base)
{
   if (g_build == GameBuild::Unknown) return;
   if (g_addr->lua_read_data_file == 0 || g_addr->load_util_read_data_file == 0) return;

   // Snapshot the INI setting, then hold prone off until prone.lvl is proven
   // present.  Without the animations the state machine has nothing to play.
   s_proneConfigured = g_proneEnabled;
   if (!s_proneConfigured) return;
   g_proneEnabled = false;

   s_read_data_file = (uintptr_t)resolve(exe_base, g_addr->load_util_read_data_file);
   original_lua_read_data_file =
       (fn_lua_cb_t)resolve(exe_base, g_addr->lua_read_data_file);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_lua_read_data_file, hooked_lua_read_data_file);
   DetourTransactionCommit();
}

void prone_lvl_load_uninstall()
{
   if (!original_lua_read_data_file) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_lua_read_data_file, hooked_lua_read_data_file);
   DetourTransactionCommit();
}
