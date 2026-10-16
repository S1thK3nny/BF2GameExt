#include "pch.h"
#include "lua_coroutines.hpp"
#ifndef LUA_COROUTINES_TEST
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <detours.h>
#endif

// =============================================================================
// Read on each build:
//
//                          modtools     Steam        GOG
//   lua_resume             0x007BFAD0   0x006A3420   0x006A44B0   detoured
//     cdecl(lua_State* co, int nargs) -> int status, plain RET. It names
//     "cannot resume dead coroutine" and "cannot resume non-suspended
//     coroutine". Its one caller is auxresume (modtools 0x007BD7E0, Steam
//     0x006A0850, GOG 0x006A18E0), which coroutine.resume and the functions
//     coroutine.wrap makes both go through, right after lua_xmove has moved
//     the arguments onto the coroutine.
//   LuaHelper::mState      g_lua_state_ptr (0x00B35A58, 0x01E579A0, 0x01E58E50)
//     what every stock binding reads its arguments from and pushes onto
//
// While a coroutine runs, LuaHelper::mState is that coroutine, so the game's
// functions read the coroutine's stack and leave their results on it. It goes
// back to what it was when lua_resume returns, whether the coroutine yielded,
// finished or failed: lua_resume catches a Lua error itself
// (luaD_rawrunprotected), so nothing jumps past the stub. A coroutine resuming
// another saves and restores in turn. Event callbacks a stock call fires from
// inside a coroutine run on the coroutine's stack, which Lua allows; yielding
// from inside one was never possible and still is not.
// =============================================================================

namespace lua_coroutines {

void** s_mState = nullptr;   // &LuaHelper::mState
void*  s_resume = nullptr;   // lua_resume, a trampoline once detoured

// cdecl(co, nargs): the global state is the coroutine for the call, and the
// one it was before after it. EAX carries the status back; ECX and EDX are
// kept as they were, and nothing else is touched.
__declspec(naked) int resume_stub()
{
   __asm {
      push ecx
      push edx
      mov  ecx, [s_mState]
      push dword ptr [ecx]          // what the global was
      mov  edx, [esp + 16]          // co
      mov  [ecx], edx
      push dword ptr [esp + 20]     // nargs
      push edx                      // co
      call dword ptr [s_resume]
      add  esp, 8
      mov  ecx, [s_mState]
      pop  dword ptr [ecx]          // the global back
      pop  edx
      pop  ecx
      ret
   }
}

} // namespace lua_coroutines

#ifndef LUA_COROUTINES_TEST

namespace {

bool s_installed = false;

bool guard(uintptr_t base, uintptr_t va, const char* bytes, const char* mask)
{
   const auto* code = static_cast<const unsigned char*>(resolve(base, va));
   for (size_t i = 0; mask[i]; ++i)
      if (mask[i] == 'x' && code[i] != static_cast<unsigned char>(bytes[i])) return false;
   return true;
}

} // namespace

void lua_coroutines_install(uintptr_t base)
{
   using namespace lua_coroutines;
   const bool modtools = g_build == GameBuild::Modtools;
   if (!modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;
   if (!g_addr->lua_resume || !g_addr->g_lua_state_ptr) {
      install_log("[LuaCoroutines] NOT installed: no address set for this build");
      return;
   }
   // lua_resume up to its dead-coroutine test: the state's ci against its
   // base_ci (+0x14, +0x28).
   if (!guard(base, g_addr->lua_resume,
              modtools ? "\x56\x8B\x74\x24\x08\x8B\x46\x14\x3B\x46\x28\x57"
                       : "\x55\x8B\xEC\x56\x8B\x75\x08\x8B\x46\x14\x3B\x46\x28",
              modtools ? "xxxxxxxxxxxx" : "xxxxxxxxxxxxx")) {
      install_log("[LuaCoroutines] NOT installed: lua_resume 0x%08X is not as expected",
                  (unsigned)g_addr->lua_resume);
      return;
   }
   s_mState = static_cast<void**>(resolve(base, g_addr->g_lua_state_ptr));
   s_resume = resolve(base, g_addr->lua_resume);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG r = DetourAttach(&(PVOID&)s_resume, resume_stub);
   if (r != NO_ERROR || DetourTransactionCommit() != NO_ERROR) {
      if (r != NO_ERROR) DetourTransactionAbort();
      install_log("[LuaCoroutines] NOT installed: the lua_resume detour failed (%ld)", (long)r);
      return;
   }
   s_installed = true;
   install_log("[LuaCoroutines] installed: the game's Lua functions work inside coroutines (lua_resume 0x%08X)",
               (unsigned)g_addr->lua_resume);
}

void lua_coroutines_uninstall()
{
   using namespace lua_coroutines;
   if (!s_installed) return;
   s_installed = false;
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)s_resume, resume_stub);
   DetourTransactionCommit();
}

#endif // LUA_COROUTINES_TEST
