#pragma once

// Vectored exception handler that appends fatal-exception reports
// (EIP + module-relative address, registers, stack scan) to
// BF2GameExt_crash.log next to the game exe.  Fires on FIRST-chance
// exceptions, so it captures crashes even when the game's own SEH
// handler swallows them and exits "gracefully".
void crash_logger_install();
