#include "pch.h"
#include "command_registry.hpp"
#include "debug_command.hpp"
#include "core/game_addrs.hpp"

#include <detours.h>

// -- Commands -----------------------------------------------------------------
#include "hover_springs.hpp"
#include "weapon_ranges.hpp"
// Add new command headers here
// -----------------------------------------------------------------------------

typedef int(__cdecl* AddVariable_t)(const char* name, const void* multivar);
typedef int(__cdecl* AddCommand_t)(const char* name, DebugCommandRegistry::CommandCallback fn);

static AddVariable_t s_addVariable = nullptr;
static AddCommand_t  s_addCommand  = nullptr;

// ---------------------------------------------------------------------------
// Piggyback hook: the engine registers its own console variables from static
// init functions. We hook one of them (the one that registers
// "render_soldier_colliding") so our addBool/addCommand calls run at exactly
// the same timing, on the same heap, with the console fully initialized.
// ---------------------------------------------------------------------------

typedef void(__cdecl* EngineConsoleReg_t)();
static EngineConsoleReg_t s_origEngineConsoleReg = nullptr;

static void __cdecl hooked_EngineConsoleReg()
{
   // Let the engine register its own variable first
   s_origEngineConsoleReg();

   // Now register ours — same heap, same console state
   static bool s_done = false;
   if (s_done) return;
   s_done = true;

   HoverSprings::lateInit();
   WeaponRanges::lateInit();
   // Add new command lateInits here
}

// Phase 1: resolve engine pointers, install Detour hooks (early, DLL_PROCESS_ATTACH)
void DebugCommandRegistry::install(uintptr_t exe_base)
{
   using namespace game_addrs::modtools;

   s_addVariable = (AddVariable_t)resolve(exe_base, console_add_variable);
   s_addCommand  = (AddCommand_t) resolve(exe_base, console_add_command);

   s_origEngineConsoleReg = (EngineConsoleReg_t)resolve(exe_base, engine_console_reg);

   DebugCommand::initEngine(exe_base);

   // Install hooks for all commands
   HoverSprings::install(exe_base);
   WeaponRanges::install(exe_base);
   // Add new command installs here

   // Hook the engine's console registration to piggyback our commands
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)s_origEngineConsoleReg, hooked_EngineConsoleReg);
   DetourTransactionCommit();
}

// Phase 2: no-op — registration happens via the piggyback hook
void DebugCommandRegistry::lateInit()
{
}

void DebugCommandRegistry::uninstall()
{
   HoverSprings::uninstall();
   WeaponRanges::uninstall();
   // Add new command uninstalls here

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (s_origEngineConsoleReg) DetourDetach(&(PVOID&)s_origEngineConsoleReg, hooked_EngineConsoleReg);
   DetourTransactionCommit();
}

void DebugCommandRegistry::addBool(const char* name, bool* var)
{
   if (!s_addVariable) return;
   struct { uint32_t type; void* ptr; } mv = { 0x11, var };
   s_addVariable(name, &mv);
}

void DebugCommandRegistry::addCommand(const char* name, CommandCallback fn)
{
   if (!s_addCommand) return;
   s_addCommand(name, fn);
}
