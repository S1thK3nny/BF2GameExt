#include "pch.h"
#include "error_dialog_fix.hpp"
#include "core/resolve.hpp"

#include "../../resource.h"

// =============================================================================
// RedWarning::DialogBoxMessage fix — port of PrismaticFlower's upstream aefa406.
//
// RedWarning::DialogBoxMessage shows fatal errors via DialogBoxParamA with a
// dialog template that was compiled into the modtools exe but is MISSING from
// the GoG/Steam builds' resources — so the call fails and errors vanish
// silently.  The game's DLGPROC is fine; only the template is gone.
//
// Fix: rewrite the FF 15 [DialogBoxParamA] indirect call inside
// RedWarning::DialogBoxMessage to 90 E8 rel32 -> our __stdcall shim, which
// forwards to DialogBoxParamA with BF2GameExt.dll's HINSTANCE and the
// IDD_GAMEMESSAGEDIALOG template from our Resource.rc (same control IDs the
// game's DLGPROC expects: IDOK + edit control 1001).
// =============================================================================

extern "C" IMAGE_DOS_HEADER __ImageBase;

static int __stdcall RedWarning_DialogBoxParamA(void* /*hInstance*/, const char* /*lpTemplateName*/,
                                                void* hWndParent, void* lpDialogFunc, LPARAM dwInitParam)
{
    return (int)DialogBoxParamA((HINSTANCE)&__ImageBase, MAKEINTRESOURCEA(IDD_GAMEMESSAGEDIALOG),
                                (HWND)hWndParent, (DLGPROC)lpDialogFunc, dwInitParam);
}

bool g_errorDialogFixEnabled = true;

void error_dialog_fix_install(uintptr_t exe_base)
{
    if (!g_errorDialogFixEnabled) return;
    if (g_build == GameBuild::Unknown) return;

    // Modtools still ships the template — nothing to fix there (site unmapped).
    if (g_addr->red_warning_dialog_call_site == 0) return;

    uint8_t* site = (uint8_t*)resolve(exe_base, g_addr->red_warning_dialog_call_site);

    // Expect the original FF 15 imm32 (call [DialogBoxParamA]) — 6 bytes.
    if (site[0] != 0xFF || site[1] != 0x15) return;

    // Rewrite to 90 (nop) + E8 rel32 (call shim) — same 6 bytes, args untouched
    // (the shim keeps DialogBoxParamA's __stdcall 5-arg signature).
    const int32_t rel32 = (int32_t)((uintptr_t)&RedWarning_DialogBoxParamA - ((uintptr_t)site + 6));

    site[0] = 0x90;
    site[1] = 0xE8;
    memcpy(site + 2, &rel32, sizeof(rel32));
}
