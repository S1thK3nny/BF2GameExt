#include "pch.h"
#include "debug_command_registry.hpp"

// -- Commands -----------------------------------------------------------------
#include "commands/debug_hover_springs.hpp"
// Add new command headers here
// -----------------------------------------------------------------------------

static constexpr uintptr_t kBase = 0x400000u;

typedef int(__cdecl* AddVariable_t)(const char* name, const void* multivar);
typedef int(__cdecl* AddCommand_t)(const char* name, DebugCommandRegistry::CommandCallback fn);

static AddVariable_t s_addVariable = nullptr;
static AddCommand_t  s_addCommand  = nullptr;

static constexpr uintptr_t kAddVariable = 0x007ed530;
static constexpr uintptr_t kAddCommand  = 0x007ed560;

// Phase 1: resolve engine pointers, install Detour hooks (early, DLL_PROCESS_ATTACH)
void DebugCommandRegistry::install(uintptr_t exe_base)
{
   s_addVariable = (AddVariable_t)((kAddVariable - kBase) + exe_base);
   s_addCommand  = (AddCommand_t) ((kAddCommand  - kBase) + exe_base);

   // Install hooks for all commands (Detours are safe during DLL init)
   debug_hover_springs_install(exe_base);
   // Add new command installs here
}

// Phase 2: register console commands (late, after engine is fully initialized)
void DebugCommandRegistry::lateInit()
{
   debug_hover_springs_late_init();
   // Add new command lateInits here
}

void DebugCommandRegistry::uninstall()
{
   debug_hover_springs_uninstall();
   // Add new command uninstalls here
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
