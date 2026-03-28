#pragma once

// =============================================================================
// Centralized address resolution for unrelocated (imagebase 0x400000) addresses.
// Include this instead of defining per-file kUnrelocatedBase / resolve / GameLog.
// =============================================================================

#include "game_addrs.hpp"

#include <stdint.h>
#include <windows.h>

inline constexpr uintptr_t kUnrelocatedBase = 0x400000u;

// Resolve unrelocated address using a pre-cached exe base (for hot paths / batch lookups)
inline void* resolve(uintptr_t exe_base, uintptr_t unrelocated_addr)
{
   return (void*)((unrelocated_addr - kUnrelocatedBase) + exe_base);
}

// Resolve unrelocated address
inline void* resolve(uintptr_t unrelocated_addr)
{
   return resolve((uintptr_t)GetModuleHandleW(nullptr), unrelocated_addr);
}

// Cache the exe base for batch resolution
inline uintptr_t exe_base()
{
   return (uintptr_t)GetModuleHandleW(nullptr);
}

// Game's printf-style debug logger
typedef void (__cdecl* GameLog_t)(const char* fmt, ...);

inline GameLog_t get_gamelog()
{
   return (GameLog_t)resolve(game_addrs::modtools::game_log);
}
