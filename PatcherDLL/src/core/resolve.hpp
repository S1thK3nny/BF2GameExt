#pragma once

// =============================================================================
// Centralized address resolution for unrelocated (imagebase 0x400000) addresses.
// Include this instead of defining per-file kUnrelocatedBase / resolve / GameLog.
// =============================================================================

#include "game_addrs.hpp"
#include "game_build.hpp"
#include "util/game_log_lock.hpp"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

inline constexpr uintptr_t kUnrelocatedBase = 0x400000u;

inline void* resolve(uintptr_t exe_base, uintptr_t unrelocated_addr)
{
   return (void*)((unrelocated_addr - kUnrelocatedBase) + exe_base);
}

inline void* resolve(uintptr_t unrelocated_addr)
{
   return resolve((uintptr_t)GetModuleHandleW(nullptr), unrelocated_addr);
}

inline uintptr_t exe_base()
{
   return (uintptr_t)GetModuleHandleW(nullptr);
}

// Write into the exe image outside the install-time RW window (uninstall).
inline void protected_write(void* dst, const void* src, size_t len)
{
   DWORD oldProt;
   if (VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &oldProt)) {
      memcpy(dst, src, len);
      VirtualProtect(dst, len, oldProt, &oldProt);
   }
}

typedef void (__cdecl* GameLog_t)(const char* fmt, ...);

// RedWarning::LogMessage, unwrapped. Prefer get_gamelog().
inline GameLog_t get_gamelog_raw()
{
   if (g_addr->game_log == 0) return nullptr;
   return (GameLog_t)resolve(g_addr->game_log);
}

// Engine enum (PDB). FATAL precedes the engine's quit dialog, so never log it.
// NONE is a destination threshold, not a message severity.
enum RED_ERROR_SEVERITY {
   RED_SEVERITY_VERBOSE = 0,
   RED_SEVERITY_INFORM  = 1,
   RED_SEVERITY_WARNING = 2,
   RED_SEVERITY_ERROR   = 3,
   RED_SEVERITY_BUG     = 4,
   RED_SEVERITY_FATAL   = 5,
   RED_SEVERITY_NONE    = 6,
};

// __cdecl(severity, file, line, compileDate, compileTime)
typedef void (__cdecl* SetLogData_t)(RED_ERROR_SEVERITY, const char*, int, const char*, const char*);

inline SetLogData_t get_set_log_data()
{
   if (g_addr->red_warning_set_log_data == 0) return nullptr;
   return (SetLogData_t)resolve(g_addr->red_warning_set_log_data);
}

// Trim the absolute __FILE__ to start at the last "PatcherDLL".
namespace detail {
constexpr bool src_marker_at(const char* s)
{
   const char lit[] = "PatcherDLL";
   for (int i = 0; i < 10; ++i)
      if (s[i] != lit[i]) return false;
   return true;
}
constexpr const char* trim_src_path(const char* p)
{
   const char* best = p;
   for (const char* s = p; *s; ++s)
      if (*s == 'P' && src_marker_at(s)) best = s;
   return best;
}

// Call site captured by get_gamelog(). Shared globals: only safe because the
// shims hold the engine-log lock across SetLogData + LogMessage.
inline const char* g_logFile = "PatcherDLL";
inline int         g_logLine = 0;
} // namespace detail

#define SRC_FILE (detail::trim_src_path(__FILE__))

// SetLogData first, or the header inherits the last engine warning's file/line.
inline void __cdecl scheme_gamelog(const char* fmt, ...)
{
   GameLog_t fn_log = get_gamelog_raw();
   if (!fn_log) return;

   char msg[1024];
   va_list ap;
   va_start(ap, fmt);
   _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
   va_end(ap);

   game_log_lock_enter();
   if (SetLogData_t set_log_data = get_set_log_data()) {
      set_log_data(RED_SEVERITY_INFORM, detail::g_logFile, detail::g_logLine,
                   __DATE__, __TIME__);
   }
   fn_log("%s", msg);
   game_log_lock_leave();
}

inline GameLog_t get_gamelog_at(const char* file, int line)
{
   detail::g_logFile = file;
   detail::g_logLine = line;
   return &scheme_gamelog;
}

// Default logger, severity INFORM. Use warn_gamelog() for anything else.
#define get_gamelog() get_gamelog_at(SRC_FILE, __LINE__)

// Log with an explicit severity. Text goes through "%s" so a literal '%' is safe.
inline void warn_gamelog(RED_ERROR_SEVERITY severity, const char* srcFile, int srcLine,
                         const char* fmt, ...)
{
   GameLog_t fn_log = get_gamelog_raw();
   if (!fn_log) return;

   char msg[1024];
   va_list ap;
   va_start(ap, fmt);
   _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
   va_end(ap);

   game_log_lock_enter();
   if (SetLogData_t set_log_data = get_set_log_data())
      set_log_data(severity, srcFile, srcLine, __DATE__, __TIME__);
   fn_log("%s", msg);
   game_log_lock_leave();
}
