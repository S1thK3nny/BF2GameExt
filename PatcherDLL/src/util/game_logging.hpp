#pragma once

#include <stdint.h>

// Enables the engine's own BFront2.log file logging on retail builds
// (Steam/GOG), where it ships compiled in but disabled.  Port of
// PrismaticFlower's "Logging Enablement" (upstream 3664782), gated by
// [Features] GameLogging in the INI instead of the /log commandline flag.
// No-ops on modtools (which always logs) and unidentified builds.

extern bool g_gameLoggingEnabled;

void game_logging_install(uintptr_t exe_base);
void game_logging_uninstall();
