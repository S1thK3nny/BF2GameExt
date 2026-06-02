#pragma once

// =============================================================================
// Centralized address resolution for unrelocated (imagebase 0x400000) addresses.
// Include this instead of defining per-file kUnrelocatedBase / resolve / GameLog.
// =============================================================================

#include "game_addrs.hpp"

#include <stdint.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

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

// Log a line to the bf2log via RedWarning::LogMessage but WITHOUT its
// "Message Severity: N\n<file>(line)" decoration: clear g_bFormatted for the
// duration so the raw text is emitted, then restore it. See
// memory/logging_redwarning_vs_console.md. Not for hot paths (resolves each call).
#pragma warning(push)
#pragma warning(disable: 4996) // _vsnprintf deprecation
inline void clean_gamelog(const char* fmt, ...)
{
   GameLog_t log = get_gamelog();
   if (!log) return;

   char buf[1024];
   va_list ap;
   va_start(ap, fmt);
   _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
   va_end(ap);
   buf[sizeof(buf) - 1] = '\0';

   uint8_t* fmtFlag = (uint8_t*)resolve(game_addrs::modtools::log_formatted_flag);
   uint8_t prev = fmtFlag ? *fmtFlag : 0;
   if (fmtFlag) *fmtFlag = 0;
   log("%s", buf);
   if (fmtFlag) *fmtFlag = prev;
}
#pragma warning(pop)
