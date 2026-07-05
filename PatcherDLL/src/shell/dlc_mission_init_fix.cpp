#include "pch.h"
#include "dlc_mission_init_fix.hpp"
#include "core/resolve.hpp"

#include <detours.h>

// =============================================================================
// DLC mission-list initialization fix — port of PrismaticFlower's upstream
// e8b6fa7.
//
// The addon/DLC mission list is populated (addme.script execution and all)
// during GameState::ShellState::Enter.  Launching a mission directly from the
// commandline enters MissionState without ever entering the shell, so the DLC
// list is empty and the game can't find mod mission scripts.
//
// Fix: track whether ShellState::Enter has ever run; when MissionState::Enter
// fires without it, run the shell state's full Enter + Exit first.  Running
// the real ShellState::Enter (rather than hand-populating the list) gives any
// exotic addme.script the complete, normal Lua context.
//
// Upstream shimmed the vtable Enter slots via MASM trampolines; we detour the
// Enter bodies (all calls dispatch through them), matching this fork's hook
// style.  GameState::State vtable layout: [0]=dtor [1]=Enter [2]=Update
// [3]=Exit (confirmed against the retail Steam exe: shell object @0x7eb998,
// vtable 0x79FB80, slot1=0x53B980 Enter / slot3=0x53BA50 Exit; no COMDAT
// folding of either Enter body).
//
// STATUS: EXPERIMENTAL, OFF BY DEFAULT.  On the retail Steam exe the
// mechanics were verified statically (ShellState::Enter -> 0x635d70 ->
// 0x48E830 addon scan, which resets/refills DownloadableContent::mNumMissions
// @0x1E30948), but a live /mission <addon mission> launch still failed with
// "Could not open MISSION\<name>.lvl" and a downstream null-deref @0x65159E
// (inside the mission-load fn 0x530ee0 called from MissionState::Enter+0x11).
// Open question: whether the load path needs DownloadableContent::
// SetCurrentMission(hash) in addition to the populated list.  INI:
// [Fixes] DLCMissionInitFix=1 to experiment.
// =============================================================================

using fn_StateEnter_t = void(__fastcall*)(void* self, void* edx);
using fn_StateVfn_t   = void(__thiscall*)(void* self);

static fn_StateEnter_t original_ShellStateEnter   = nullptr;
static fn_StateEnter_t original_MissionStateEnter = nullptr;
static void*           g_shellStateObject         = nullptr;

static bool g_shellStateEverEntered = false;

static constexpr size_t kVtableSlotEnter = 1;
static constexpr size_t kVtableSlotExit  = 3;

static void __fastcall hooked_ShellStateEnter(void* self, void* edx)
{
    g_shellStateEverEntered = true;

    original_ShellStateEnter(self, edx);
}

static void __fastcall hooked_MissionStateEnter(void* self, void* edx)
{
    if (!g_shellStateEverEntered) {
        // Call Enter/Exit through the object's vtable — Enter dispatches into
        // our hook above, which sets the flag before running the real thing.
        void** vtable = *(void***)g_shellStateObject;
        ((fn_StateVfn_t)vtable[kVtableSlotEnter])(g_shellStateObject);
        ((fn_StateVfn_t)vtable[kVtableSlotExit])(g_shellStateObject);
    }

    original_MissionStateEnter(self, edx);
}

// Off unless explicitly enabled — see STATUS above.
bool g_dlcMissionInitFixEnabled = false;

void dlc_mission_init_fix_install(uintptr_t exe_base)
{
    if (!g_dlcMissionInitFixEnabled) return;
    if (g_build == GameBuild::Unknown) return;
    if (g_addr->gamestate_shell_state == 0 ||
        g_addr->gamestate_shell_state_enter == 0 ||
        g_addr->gamestate_mission_state_enter == 0)
        return;

    original_ShellStateEnter   = (fn_StateEnter_t)resolve(exe_base, g_addr->gamestate_shell_state_enter);
    original_MissionStateEnter = (fn_StateEnter_t)resolve(exe_base, g_addr->gamestate_mission_state_enter);
    g_shellStateObject         = resolve(exe_base, g_addr->gamestate_shell_state);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)original_ShellStateEnter, hooked_ShellStateEnter);
    DetourAttach(&(PVOID&)original_MissionStateEnter, hooked_MissionStateEnter);
    DetourTransactionCommit();
}

void dlc_mission_init_fix_uninstall()
{
    if (!original_ShellStateEnter) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)original_ShellStateEnter, hooked_ShellStateEnter);
    DetourDetach(&(PVOID&)original_MissionStateEnter, hooked_MissionStateEnter);
    DetourTransactionCommit();
    original_ShellStateEnter = nullptr;
    original_MissionStateEnter = nullptr;
}
