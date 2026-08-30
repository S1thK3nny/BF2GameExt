#include "pch.h"
#include "command_post_overflow_fix.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <detours.h>

// See command_post_overflow_fix.hpp for the crash, the mechanism, and why the
// clamp reclaims a slot rather than refusing the registration.

bool g_commandPostOverflowFix = true;

namespace {

constexpr int kMaxPosts = 16;   // sPostArray is a fixed 16 entries on every build

// Resolved per build in _install.  Zero means "not derived for this build", in
// which case nothing is hooked at all.
uintptr_t s_hintVA     = 0;   // int, the one-shot forced index (-1 = none)
uintptr_t s_arrayVA    = 0;   // holds the array base address
uintptr_t s_countVA    = 0;   // holds the address of the live count
unsigned  s_classOff   = 0;   // CommandPost -> CommandPostClass*, build-specific

bool s_warned = false;        // one line per session, not per registration

int* hint()   { return reinterpret_cast<int*>(s_hintVA); }
void** slots(){ return *reinterpret_cast<void***>(s_arrayVA); }
int* count()  { return *reinterpret_cast<int**>(s_countVA); }

// Decide whether to force an index, and which one.  Runs BEFORE the original.
//
// Everything here is a read of engine state plus at most one write to the
// forced-index global -- the same global the network layer writes -- so the
// original function does all the real work either way.
void clamp_registration(const void* cpClass)
{
   if (!s_hintVA || !s_arrayVA || !s_countVA) return;

   __try {
      // A forced index is already pending: it came from a 4-bit ReadBits, so it
      // is 0..15 and cannot select slot 16.  Leave it alone.
      if (*hint() >= 0) return;

      // The engine only appends when the search found nothing; below the cap that
      // append is safe and we must not disturb it.
      const int n = *count();
      if (n < kMaxPosts) return;

      void** const arr = slots();
      if (!arr) return;

      // (1) Reclaim a slot leaked by ~CommandPost, which NULLs its entry without
      //     decrementing the count.  Handing stock a forced index at an empty slot
      //     makes it allocate a fresh post there through its own code path.
      for (int i = 0; i < kMaxPosts; ++i) {
         if (arr[i] == nullptr) {
            *hint() = i;
            return;
         }
      }

      // (2) All sixteen are genuinely live.  Bind to the best-matching existing
      //     post rather than crashing: prefer one of the same CommandPostClass,
      //     which is stock's own reuse rule minus its distance test.
      int pick = kMaxPosts - 1;
      if (s_classOff != 0) {
         for (int i = 0; i < kMaxPosts; ++i) {
            const void* const post = arr[i];
            if (!post) continue;
            if (*reinterpret_cast<const void* const*>(
                   reinterpret_cast<const char*>(post) + s_classOff) == cpClass) {
               pick = i;
               break;
            }
         }
      }
      *hint() = pick;

      if (!s_warned) {
         s_warned = true;
         warn_gamelog(RED_SEVERITY_ERROR, SRC_FILE, __LINE__,
                      "[CommandPostOverflow] all %d command post slots are live; "
                      "binding further registrations to slot %d instead of writing "
                      "past the end of sPostArray. Stock hands the neighbouring "
                      "global to CommandPost as `this` here and crashes.\n",
                      kMaxPosts, pick);
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A bad read here must not be worse than the bug we are fixing: fall through
      // and let the original run exactly as it would have.
   }
}

// ---------------------------------------------------------------------------
// The two build shapes.  modtools is __cdecl, Steam is __fastcall -- read from
// each build's own prologue, never ported across.
// ---------------------------------------------------------------------------
using fn_cdecl_t    = void*(__cdecl*)(void* entity, void* cpClass);
using fn_fastcall_t = void*(__fastcall*)(void* entity, void* cpClass);

fn_cdecl_t    s_origCdecl    = nullptr;
fn_fastcall_t s_origFastcall = nullptr;

void* __cdecl hooked_cdecl(void* entity, void* cpClass)
{
   if (g_commandPostOverflowFix) clamp_registration(cpClass);
   return s_origCdecl(entity, cpClass);
}

void* __fastcall hooked_fastcall(void* entity, void* cpClass)
{
   if (g_commandPostOverflowFix) clamp_registration(cpClass);
   return s_origFastcall(entity, cpClass);
}

} // namespace

void command_post_overflow_fix_install(uintptr_t exe_base)
{
   if (g_build == GameBuild::Unknown) return;
   if (g_addr->command_post_find_or_create == 0) return;   // not derived -> omit

   s_hintVA   = (uintptr_t)resolve(exe_base, g_addr->command_post_hint_index);
   s_arrayVA  = (uintptr_t)resolve(exe_base, g_addr->command_post_array_ptr);
   s_countVA  = (uintptr_t)resolve(exe_base, g_addr->command_post_count_ptr);
   s_classOff = (unsigned)g_addr->command_post_class_off;

   void* const target = resolve(exe_base, g_addr->command_post_find_or_create);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (g_build == GameBuild::Modtools) {
      s_origCdecl = (fn_cdecl_t)target;
      DetourAttach(&(PVOID&)s_origCdecl, hooked_cdecl);
   } else {
      s_origFastcall = (fn_fastcall_t)target;
      DetourAttach(&(PVOID&)s_origFastcall, hooked_fastcall);
   }
   DetourTransactionCommit();
}

void command_post_overflow_fix_uninstall()
{
   if (!s_origCdecl && !s_origFastcall) return;
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (s_origCdecl)    DetourDetach(&(PVOID&)s_origCdecl, hooked_cdecl);
   if (s_origFastcall) DetourDetach(&(PVOID&)s_origFastcall, hooked_fastcall);
   DetourTransactionCommit();
   s_origCdecl = nullptr;
   s_origFastcall = nullptr;
}
