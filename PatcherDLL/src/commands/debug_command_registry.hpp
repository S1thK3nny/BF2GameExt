#pragma once

#include <stdint.h>

// =============================================================================
// DebugCommandRegistry
//
// Thin wrapper around RedCommandConsole::AddVariable / AddCommand.
// Resolves the engine functions once, then lets any module register
// console commands without knowing the addresses.
//
// Two-phase init:
//   install()      — early (DLL_PROCESS_ATTACH): resolve addrs, install hooks
//   lateInit()     — late  (after engine init):  register console commands
//   uninstall()    — cleanup
// =============================================================================

class DebugCommandRegistry {
public:
   // Phase 1: resolve engine function pointers, install Detour hooks.
   // Safe to call during DLL_PROCESS_ATTACH.
   static void install(uintptr_t exe_base);

   // Phase 2: register console commands with the engine.
   // Must be called AFTER the engine's RedCommandConsole is initialized
   // (e.g. from hooked_init_state).
   static void lateInit();

   static void uninstall();

   // Register a bool that toggles via the ~ console (e.g. "RenderHoverSprings").
   static void addBool(const char* name, bool* var);

   // Register a callback command via the ~ console.
   using CommandCallback = int(__cdecl*)(void* console, unsigned int id, const char* args);
   static void addCommand(const char* name, CommandCallback fn);
};
