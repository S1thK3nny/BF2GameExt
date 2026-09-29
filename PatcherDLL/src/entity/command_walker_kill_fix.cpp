#include "pch.h"
#include "command_walker_kill_fix.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <detours.h>

// See command_walker_kill_fix.hpp for the crash this guards.

// CommandWalker::Kill(Damageable* this) -- __thiscall, bare RET on every build.
// Taken as __fastcall so ECX arrives as the first parameter; EDX is unused by
// the original and is passed straight through.
typedef void(__fastcall* fn_Kill_t)(void* ecx, void* edx);

static fn_Kill_t g_origKill = nullptr;
static uintptr_t g_postOff  = 0;

static void __fastcall hooked_kill(void* ecx, void* edx)
{
   if (ecx && *(void**)((char*)ecx + g_postOff) == nullptr) return;  // already killed

   g_origKill(ecx, edx);
}

void command_walker_kill_fix_install(uintptr_t exe_base)
{
   if (g_addr->command_walker_kill == 0 || g_addr->command_walker_post_off == 0) return;

   g_postOff  = g_addr->command_walker_post_off;
   g_origKill = reinterpret_cast<fn_Kill_t>(resolve(exe_base, g_addr->command_walker_kill));

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG rd = DetourAttach(reinterpret_cast<PVOID*>(&g_origKill), hooked_kill);
   const LONG rc = DetourTransactionCommit();

   if (rd != NO_ERROR || rc != NO_ERROR) g_origKill = nullptr;
}

void command_walker_kill_fix_uninstall()
{
   if (!g_origKill) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(reinterpret_cast<PVOID*>(&g_origKill), hooked_kill);
   DetourTransactionCommit();
   g_origKill = nullptr;
}
