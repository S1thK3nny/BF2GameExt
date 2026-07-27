#pragma once

// =============================================================================
// Centralized address resolution for unrelocated (imagebase 0x400000) addresses.
// Include this instead of defining per-file kUnrelocatedBase / resolve / GameLog.
// =============================================================================

#include "game_addrs.hpp"
#include "game_build.hpp"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
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

// The engine's RedWarning::LogMessage, unwrapped. Prefer get_gamelog() below
// unless you specifically want whatever RedWarning header context is currently set.
inline GameLog_t get_gamelog_raw()
{
   if (g_addr->game_log == 0) return nullptr;
   return (GameLog_t)resolve(g_addr->game_log);
}

// RedWarning severity levels, as passed to RedWarning::SetLogData. The bf2log
// formatter prints the number in the "Message Severity: N" header.
enum RedSeverity {
   RED_SEVERITY_INFO    = 1,
   RED_SEVERITY_WARNING = 2,
   RED_SEVERITY_ERROR   = 3,
};

// __cdecl(severity, file, line, compileDate, compileTime)
typedef void (__cdecl* SetLogData_t)(int, const char*, int, const char*, const char*);

inline SetLogData_t get_set_log_data()
{
   if (g_addr->red_warning_set_log_data == 0) return nullptr;
   return (SetLogData_t)resolve(g_addr->red_warning_set_log_data);
}

// MSBuild expands __FILE__ to an absolute path, which puts the build machine's
// directory layout into every log line a modder sees. Trim it to the last
// "PatcherDLL" so the bf2log shows "PatcherDLL\src\lua\lua_funcs.cpp" instead.
// Falls back to the untrimmed path if the marker is absent.
namespace detail {
constexpr bool src_marker_at(const char* s)
{
   const char lit[] = "PatcherDLL";
   // lit has no interior NUL, so a short s mismatches before it can overrun.
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

// Call-site context captured by the get_gamelog() macro just below, so the shim
// can stamp the caller's own file and line rather than inheriting whatever the
// last engine warning left in the RedWarning globals. Single-threaded game.
inline const char* g_logFile = "PatcherDLL";
inline int         g_logLine = 0;
} // namespace detail

#define SRC_FILE (detail::trim_src_path(__FILE__))

// Log a line in the engine's own format:
//
//     Message Severity: 1
//     PatcherDLL\src\controller\controller_support.cpp(210)
//     [Controller] 1 joystick(s) detected, setting up bindings...
//
// The header is built by the bf2log formatter from RedWarning's globals, which
// nothing ever resets — so a bare LogMessage call inherits whatever the previous
// engine warning left behind. That is why our lines used to come out stamped
// "pcRedTexture.cpp(553)" or "[NULL](0)". Setting the context first makes them
// ours; severity defaults to 1 (info) since most of our output is informational.
inline void __cdecl scheme_gamelog(const char* fmt, ...)
{
   GameLog_t fn_log = get_gamelog_raw();
   if (!fn_log) return;

   char msg[1024];
   va_list ap;
   va_start(ap, fmt);
   _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
   va_end(ap);

   if (SetLogData_t set_log_data = get_set_log_data()) {
      set_log_data(RED_SEVERITY_INFO, detail::g_logFile, detail::g_logLine,
                   __DATE__, __TIME__);
   }
   fn_log("%s", msg);
}

inline GameLog_t get_gamelog_at(const char* file, int line)
{
   detail::g_logFile = file;
   detail::g_logLine = line;
   return &scheme_gamelog;
}

// Default logger for BF2GameExt code. Existing callers are unchanged --
// `auto fn_log = get_gamelog(); fn_log("...", ...)` still works -- but the macro
// captures each call site so the header carries that file and line.
// Use warn_gamelog() when a line needs a severity other than info.
#define get_gamelog() get_gamelog_at(SRC_FILE, __LINE__)

// Log one line to the bf2log with an explicit RedWarning severity, matching what
// the engine's own RWASSERT/RedWarning macros emit:
//
//     Message Severity: 3
//     <file>(<line>)
//     <your message>
//
// Without the SetLogData call the message still reaches the log, but inherits
// whatever stale severity/file context the last engine warning left behind (see
// docs — this is why raw game_log() calls sometimes show an unrelated
// "Weapon.cpp(82)" header).
//
// The text is formatted into a buffer first and passed as a "%s" argument, so a
// message containing a literal '%' cannot be mistaken for a format specifier.
inline void warn_gamelog(int severity, const char* srcFile, int srcLine,
                         const char* fmt, ...)
{
   GameLog_t fn_log = get_gamelog_raw();
   if (!fn_log) return;

   char msg[1024];
   va_list ap;
   va_start(ap, fmt);
   _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
   va_end(ap);

   if (SetLogData_t set_log_data = get_set_log_data())
      set_log_data(severity, srcFile, srcLine, __DATE__, __TIME__);
   fn_log("%s", msg);
}
