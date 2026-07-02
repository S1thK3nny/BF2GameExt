#include "pch.h"
#include "game_logging.hpp"
#include "core/resolve.hpp"

#include <detours.h>

// =============================================================================
// Logging enablement — port of PrismaticFlower's upstream 3664782.
//
// Retail builds initialize RedWarning with the file destination muted and the
// engine's pcLoggingEnabled flag false.  The modtools build enables both in
// its own RedWarning::Init.  Upstream shims the RedWarning::Init call site;
// we detour the function itself (same timing: the DLL loads via the import
// table before any exe code runs, so the hook is in place before Init fires).
//
// After the original runs: destination 2 (the log file) gets min severity 0
// and pcLoggingEnabled is set — from then on the engine writes its own log,
// which also makes RedWarning::LogMessage (our game_log) output visible on
// retail builds.
// =============================================================================

bool g_gameLoggingEnabled = false;

using fn_RedWarningInit_t = void(__cdecl*)();
using fn_SetDestMinSev_t  = void(__cdecl*)(int destination, int minSeverity);

static fn_RedWarningInit_t original_RedWarningInit = nullptr;
static fn_SetDestMinSev_t  fn_setDestMinSeverity   = nullptr;
static bool*               g_pcLoggingEnabledVar   = nullptr;

static constexpr int kFileDestination = 2;

static void __cdecl hooked_RedWarningInit()
{
    original_RedWarningInit();

    fn_setDestMinSeverity(kFileDestination, 0);
    *g_pcLoggingEnabledVar = true;
}

void game_logging_install(uintptr_t exe_base)
{
    if (!g_gameLoggingEnabled) return;
    if (g_build == GameBuild::Unknown) return;

    // Modtools always logs and leaves these unmapped — skip cleanly.
    if (g_addr->red_warning_init == 0 ||
        g_addr->red_warning_set_dest_min_severity == 0 ||
        g_addr->pc_logging_enabled == 0)
        return;

    original_RedWarningInit = (fn_RedWarningInit_t)resolve(exe_base, g_addr->red_warning_init);
    fn_setDestMinSeverity   = (fn_SetDestMinSev_t)resolve(exe_base, g_addr->red_warning_set_dest_min_severity);
    g_pcLoggingEnabledVar   = (bool*)resolve(exe_base, g_addr->pc_logging_enabled);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)original_RedWarningInit, hooked_RedWarningInit);
    DetourTransactionCommit();
}

void game_logging_uninstall()
{
    if (!original_RedWarningInit) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)original_RedWarningInit, hooked_RedWarningInit);
    DetourTransactionCommit();
    original_RedWarningInit = nullptr;
}
