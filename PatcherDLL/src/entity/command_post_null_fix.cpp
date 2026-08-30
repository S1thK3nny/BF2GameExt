#include "pch.h"
#include "command_post_null_fix.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <detours.h>

// See command_post_null_fix.hpp for the crash this guards.

// CommandPost::SetTeam(CommandPost* this, int newTeam, int oldTeam) -- __thiscall,
// RET 8.  Taken as __fastcall so ECX arrives as the first parameter; EDX is
// unused by the original and is passed straight through.
typedef void(__fastcall* fn_SetTeam_t)(void* ecx, void* edx, int a, int b);

static fn_SetTeam_t g_origSetTeam = nullptr;

// The engine can retry the same bad script reference every capture tick, so a
// per-session counter keeps one crash's worth of content bug from filling the
// log with thousands of identical lines.
static uint32_t g_blocked = 0;
static constexpr uint32_t kMaxReports = 8;

static void __fastcall hooked_set_team(void* ecx, void* edx, int a, int b)
{
   if (!ecx) {
      ++g_blocked;
      if (g_blocked <= kMaxReports) {
         warn_gamelog(RED_SEVERITY_ERROR, SRC_FILE, __LINE__,
                      "[CommandPostNullFix] CommandPost::SetTeam called on a NULL command post "
                      "(team %d -> %d); ignoring what would have been a crash. A mission script "
                      "is almost certainly passing an entity that is not a command post -- check "
                      "the bf2log for \"is EntityProp, but need GameObject\" lines naming it.%s\n",
                      b, a, (g_blocked == kMaxReports) ? " (further reports suppressed)" : "");
      }
      return;
   }

   g_origSetTeam(ecx, edx, a, b);
}

void command_post_null_fix_install(uintptr_t exe_base)
{
   if (g_addr->command_post_set_team == 0) return; // not derived for this build

   g_origSetTeam = reinterpret_cast<fn_SetTeam_t>(resolve(exe_base, g_addr->command_post_set_team));

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG rd = DetourAttach(reinterpret_cast<PVOID*>(&g_origSetTeam), hooked_set_team);
   const LONG rc = DetourTransactionCommit();

   if (rd != NO_ERROR || rc != NO_ERROR) g_origSetTeam = nullptr;
}

void command_post_null_fix_uninstall()
{
   if (!g_origSetTeam) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(reinterpret_cast<PVOID*>(&g_origSetTeam), hooked_set_team);
   DetourTransactionCommit();
   g_origSetTeam = nullptr;
}
