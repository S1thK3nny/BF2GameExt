#pragma once

// =============================================================================
// Centralized address resolution for unrelocated (imagebase 0x400000) addresses.
// Include this instead of defining per-file kUnrelocatedBase / resolve / GameLog.
// =============================================================================

#include "game_addrs.hpp"
#include "game_build.hpp"

#include <stdint.h>
#include <string.h>
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

// Write into the exe image outside the install-time RW window (uninstall /
// restore paths). Sections are re-protected after install, so a plain store
// into .text/.rdata access-violates — wrap the write in VirtualProtect.
inline void protected_write(void* dst, const void* src, size_t len)
{
   DWORD oldProt;
   if (VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &oldProt)) {
      memcpy(dst, src, len);
      VirtualProtect(dst, len, oldProt, &oldProt);
   }
}

// Game's printf-style debug logger
typedef void (__cdecl* GameLog_t)(const char* fmt, ...);

inline GameLog_t get_gamelog()
{
   if (g_addr->game_log == 0) return nullptr;
   return (GameLog_t)resolve(g_addr->game_log);
}
