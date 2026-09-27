#pragma once

#include <stdarg.h>
#include <stdio.h>

// =============================================================================
// Install-time logger: appends one line to BF2GameExt.log through the CRT.
//
// This is the ONLY logger that may run while installers run. dllmain holds every
// exe section at PAGE_READWRITE (non-executable) for the whole installer
// sequence, so calling the engine's own logger (get_gamelog / RedWarning) jumps
// into non-executable .text and raises an EXEC access violation. On the
// DEP-enabled retail builds that surfaces as DLL_INIT_FAILED and the game will
// not start at all; modtools silently tolerates it. The CRT lives in this module
// and is safe. Runtime code normally uses get_gamelog() instead, but this is
// safe from any context, including the census and sound threads, and is what
// the diagnostics that only write to BF2GameExt.log use at runtime too.
//
// A newline is appended, so format strings should not end in "\n".
// =============================================================================

inline void install_log(const char* fmt, ...)
{
   FILE* f = nullptr;
   if (fopen_s(&f, "BF2GameExt.log", "a") != 0 || !f) return;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fclose(f);
}
