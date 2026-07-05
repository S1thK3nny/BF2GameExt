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
//
// Z-order: the game calls DialogBoxParamA with hWndParent = NULL (checked in
// both builds' RedWarning::DialogBoxMessage), and the retail game window is a
// topmost fullscreen window — an ownerless, non-topmost dialog opens BEHIND
// it and can never be raised above it (modtools' window isn't topmost, which
// is the only reason the stock dialog shows there).  Our template therefore
// carries DS_SETFOREGROUND + WS_EX_TOPMOST, and the DLGPROC below wraps the
// game's proc to force the dialog to the foreground on WM_INITDIALOG.
// =============================================================================

extern "C" IMAGE_DOS_HEADER __ImageBase;

// Only one fatal-error dialog can be up at a time (DialogBoxParamA is modal
// and RedWarning::DialogBoxMessage runs on the game thread) — a single static
// slot for the wrapped proc is fine.
static DLGPROC g_gameDialogProc = nullptr;

static INT_PTR CALLBACK ForegroundDialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    const INT_PTR result = g_gameDialogProc(hDlg, msg, wParam, lParam);

    if (msg == WM_INITDIALOG) {
        SetWindowPos(hDlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(hDlg);
    }

    return result;
}

static int __stdcall RedWarning_DialogBoxParamA(void* /*hInstance*/, const char* /*lpTemplateName*/,
                                                void* hWndParent, void* lpDialogFunc, LPARAM dwInitParam)
{
    g_gameDialogProc = (DLGPROC)lpDialogFunc;

    return (int)DialogBoxParamA((HINSTANCE)&__ImageBase, MAKEINTRESOURCEA(IDD_GAMEMESSAGEDIALOG),
                                (HWND)hWndParent, ForegroundDialogProc, dwInitParam);
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
