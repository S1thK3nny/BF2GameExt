#include "pch.h"
#include "enable_sound_warnings.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <detours.h>
#include <string.h>

// =============================================================================
// Enable sound warnings ([Features] EnableSoundWarnings)
// =============================================================================
// GameSoundEngine::gEnableSoundWarnings is a runtime-read-only bool that gates
// the "GameSound::SetID - Unable to find sound property" family of RedWarnings.
// The image default is 0.  We simply write 1 so missing-sound warnings surface.
//
// The write lands in a writable data section and happens inside dllmain's
// section-RW window, so no VirtualProtect dance is needed.  Modtools-only: the
// retail builds stripped this warning code (and its strings), leaving no flag to
// enable — see enable_sound_warnings.hpp.
//
// Formatting fix (modtools): every sound warning is routed through
// Snd::PrintDebugString, which wraps the message as "Sound (%s)".  The upstream
// Snd::PrintDebugMessage unconditionally appends a '\n' on top of format strings
// that already end in one, so the "%s" injects "\n\n" *inside* the parens:
//
//     Sound (Unable to find ParameterGraph 0xc0b4a0df
//
//     )
//
// Detour PrintDebugString and strip trailing newlines from the (caller-owned,
// mutable stack) buffer before the original runs, collapsing each warning onto
// a single tidy line:  "Sound (Unable to find ParameterGraph 0xc0b4a0df)".
// =============================================================================

bool g_enableSoundWarnings = false;

using fn_PrintDebugString_t = void(__cdecl*)(char*);
static fn_PrintDebugString_t original_PrintDebugString = nullptr;

static void __cdecl hooked_PrintDebugString(char* msg)
{
    if (msg) {
        size_t len = strlen(msg);
        while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
            msg[--len] = '\0';
    }
    original_PrintDebugString(msg);
}

void enable_sound_warnings_install(uintptr_t exe_base)
{
    if (!g_enableSoundWarnings) return;
    if (g_build != GameBuild::Modtools) return; // retail has no such flag

    uint8_t* p = (uint8_t*)resolve(exe_base, game_addrs::modtools::g_enable_sound_warnings);
    *p = 1;

    // Strip the doubled trailing newline the engine bakes into each warning.
    original_PrintDebugString =
        (fn_PrintDebugString_t)resolve(exe_base, game_addrs::modtools::snd_print_debug_string);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)original_PrintDebugString, hooked_PrintDebugString);
    if (DetourTransactionCommit() != NO_ERROR)
        original_PrintDebugString = nullptr;
}

void enable_sound_warnings_uninstall()
{
    if (!original_PrintDebugString) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)original_PrintDebugString, hooked_PrintDebugString);
    DetourTransactionCommit();
    original_PrintDebugString = nullptr;
}
