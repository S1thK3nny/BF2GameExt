#pragma once

// Vectored exception handler that appends fatal-exception reports
// (EIP + module-relative address, registers, stack scan) to
// BF2GameExt_crash.log next to the game exe.  Fires on FIRST-chance
// exceptions, so it captures crashes even when the game's own SEH
// handler swallows them and exits "gracefully".
void crash_logger_install();

// Scoped "the next fault here is expected" marker.
//
// Some engine entry points we deliberately poke fault by design and are already
// contained by a __try/__except (the carrier's cargo vtable[5] activation is the
// standing example: the engine dereferences Controllable::mPilot unguarded, and
// cargo has no pilot).  Because the handler above is FIRST-chance it still logs
// those, which fills BF2GameExt_crash.log with reports for a game that never
// crashed.  Wrap such a call in this guard to keep it out of the crash log;
// everything else on the thread is still reported normally.
//
// Cheap and non-TLS (thread id + depth), so it is safe to use from a hook.
//
// Use the plain begin/end pair inside a function that has a __try block — MSVC
// rejects objects requiring unwinding there (C2712).  The RAII wrapper is for
// everywhere else.
void crash_logger_begin_expected_fault();
void crash_logger_end_expected_fault();

struct expected_fault_scope {
   expected_fault_scope()  { crash_logger_begin_expected_fault(); }
   ~expected_fault_scope() { crash_logger_end_expected_fault(); }
   expected_fault_scope(const expected_fault_scope&) = delete;
   expected_fault_scope& operator=(const expected_fault_scope&) = delete;
};
