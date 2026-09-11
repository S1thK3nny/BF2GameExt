#include "pch.h"
#include "game_log_lock.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <detours.h>

// =============================================================================
// The crash this fixes, as observed (dev exe, 2026-09-11) and then reproduced on
// demand:
//
//   EXCEPTION C0000005   AV: WRITE addr 00000000
//   EIP = 0x008D9D6C, inside __flsbuf:  MOV byte ptr [EAX],CL
//   EAX = 0 (FILE::_base), ECX = 0x0A (the newline ending the message),
//   ESI = an _iob[] slot, EBX = its fd
//
// RedWarning::LogMessage (0x007E3D50) writes every message with
// fopen(name,"a") / fprintf / fclose -- one open-write-close per line -- and
// this exe links the SINGLE-THREADED CRT: _fprintf (0x008D56A0) is bare
// _stbuf/_output/_ftbuf and _fclose (0x008D55B6) is bare, with no _lock_str in
// either, so _iob[] is unsynchronised global state. Meanwhile the Snd library
// runs worker threads and logs from them (the burst that exposed it was
// GameSoundEngine.cpp(276) "ASSERT : playState_invalid", raised by
// SourceTransport::GetCurrentPlayState 0x0088B200).
//
// The window, read off __flsbuf: it loads FILE::_base, flushes the buffer with
// write(), and only then stores the pending character to *_base. A second
// thread running fclose in between reaches _freebuf, which frees the buffer and
// NULLs _base -- so the first thread returns from the syscall and stores to 0.
// __getbuf (0x008E286B) can never leave _base NULL (a failed malloc falls back
// to FILE::_charbuf), which is what rules out "out of memory" and pins it on
// concurrent mutation.
//
// The fix is to make the whole fopen/fprintf/fclose cycle mutually exclusive,
// which means serialising the function that performs it.
//
// FORWARDING A VARARGS FUNCTION.  RedWarning::LogMessage is
// `void __cdecl(const char* fmt, ...)`, so the hook cannot name its arguments.
// Rather than hand-roll a naked thunk that swaps return addresses, the detour
// takes a FIXED arity of one format plus 15 dwords and passes all of them on.
// This is sound for __cdecl: the caller cleans the stack, the callee consumes
// only what its format string names, and the extra dwords are read from the
// caller's own frame (higher addresses than ESP, always committed stack), so
// reading them cannot fault. 15 covers every call site in the exe; a format
// wanting more arguments than its caller passed was already undefined before we
// got here.
// =============================================================================

namespace {

using fn_LogMessage_t = void(__cdecl*)(const char* fmt, ...);

// Same shape as above, but with the arguments spelled out so the compiler emits
// the pushes for us.
using fn_LogForward_t = void(__cdecl*)(const char*, uintptr_t, uintptr_t, uintptr_t,
                                       uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                       uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                       uintptr_t, uintptr_t, uintptr_t, uintptr_t);

fn_LogForward_t original_LogMessage = nullptr;

// Lazily initialised: our own code logs through the engine before this module's
// install runs (apply_patches reports every patch set), so the lock has to be
// usable from the first call rather than from install time.
CRITICAL_SECTION s_cs;
volatile LONG    s_csState = 0; // 0 = untouched, 1 = being initialised, 2 = ready

void ensure_cs()
{
   for (;;) {
      const LONG st = InterlockedCompareExchange(&s_csState, 1, 0);
      if (st == 0) {
         // Spin briefly before sleeping: log lines are short, and the whole
         // point is that the holder is inside a file write.
         InitializeCriticalSectionAndSpinCount(&s_cs, 1000);
         InterlockedExchange(&s_csState, 2);
         return;
      }
      if (st == 2) return;
      Sleep(0); // another thread is mid-initialise
   }
}

void __cdecl hooked_LogMessage(const char* fmt, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                               uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7,
                               uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11,
                               uintptr_t a12, uintptr_t a13, uintptr_t a14, uintptr_t a15)
{
   game_log_lock_enter();
   original_LogMessage(fmt, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15);
   game_log_lock_leave();
}

} // namespace

void game_log_lock_enter()
{
   ensure_cs();
   EnterCriticalSection(&s_cs);
}

void game_log_lock_leave()
{
   if (s_csState != 2) return; // never entered, nothing to release
   LeaveCriticalSection(&s_cs);
}

void game_log_lock_install(uintptr_t exe_base)
{
   // Retail's stdio comes from MSVCR120.dll and locks each stream on its own.
   if (g_build != GameBuild::Modtools) return;
   if (g_addr->game_log == 0) return;

   ensure_cs();

   original_LogMessage = (fn_LogForward_t)resolve(exe_base, g_addr->game_log);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_LogMessage, hooked_LogMessage);
   if (DetourTransactionCommit() != NO_ERROR)
      original_LogMessage = nullptr;
}

void game_log_lock_uninstall()
{
   if (!original_LogMessage) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_LogMessage, hooked_LogMessage);
   DetourTransactionCommit();
   original_LogMessage = nullptr;

   // The critical section itself is left initialised: game_log_lock_enter() is
   // reachable from resolve.hpp for as long as the DLL is loaded, and deleting
   // it here would leave those callers entering freed state.
}
