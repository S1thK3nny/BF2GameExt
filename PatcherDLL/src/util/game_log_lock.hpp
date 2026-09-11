#pragma once

#include <stdint.h>

// =============================================================================
// Serialise RedWarning::LogMessage -- modtools only.
//
// The dev exe logs each message with a full fopen("a")/fprintf/fclose cycle and
// links the SINGLE-THREADED static CRT, whose stdio takes no locks at all. Two
// threads logging at once therefore corrupt the shared FILE, which crashes in
// the CRT rather than in the logger. See game_log_lock.cpp for the window.
//
// Install makes every entry into the engine's logger mutually exclusive, so the
// open/write/close cycle can no longer be interleaved. There is no INI key: a
// crash fix should not be optional, and the lock is uncontended in the common
// case (one thread logging) where it costs an interlocked increment.
//
// Retail is unaffected and not hooked: Steam/GOG import fprintf/fclose from
// MSVCR120.dll, the multithreaded CRT, which locks each stream already.
// =============================================================================

void game_log_lock_install(uintptr_t exe_base);
void game_log_lock_uninstall();

// Held across our own SetLogData + LogMessage pair (see core/resolve.hpp) so
// another thread cannot land between the two and steal the severity/file/line
// context our message is about to be stamped with. Recursive: the LogMessage
// detour re-enters the same lock on the same thread.
void game_log_lock_enter();
void game_log_lock_leave();
