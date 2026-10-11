#include "pch.h"
#include "dlc_mission_init_fix.hpp"
#include "core/resolve.hpp"
#include "lua/lua_hooks.hpp"
#include "util/install_log.hpp"

#include <detours.h>
#include <string.h>

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

// =============================================================================
// Unofficial v1.3 patch mission cap.
//
// The v1.3 patch's addme (addon\AAA-v1.3patch, sorts first) wraps the
// AddDownloadableContent global in Lua: it counts every call and silently drops
// the call once the count reaches the global __max_missions__, which it sets
// to 500. That cap holds even after the DLC Mission Limit Extension raises the
// engine table, so mod maps past the 499th never show up.
//
// Every call the wrapper lets through still lands in the engine binding, so we
// hook that and raise __max_missions__ there. Nothing happens without v1.3: its
// addme is the only thing that sets __LISTENING_TO_ADDDOWNLOADABLECONTENT__ and
// __max_missions__. The wrapper reads the global per call, so the new value
// covers every later addme. One past the table size lets through exactly the
// calls the engine can store, so v1.3's own "too many missions" popup (in
// ifs_login, `count >= __max_missions__`) still fires when one is refused.
// =============================================================================

using fn_AddDownloadableContent_t = int(__cdecl*)(lua_State* L);

static fn_AddDownloadableContent_t original_AddDownloadableContent = nullptr;

static constexpr float kV13MissionCap = (float)(kDlcMissionLimit + 1);

static void raise_v13_mission_cap(lua_State* L)
{
   if (!L || !g_lua.pushlstring || !g_lua.gettable || !g_lua.settable || !g_lua.toboolean ||
       !g_lua.isnumber || !g_lua.tonumber || !g_lua.pushnumber || !g_lua.gettop || !g_lua.settop)
      return;

   static const char kListening[] = "__LISTENING_TO_ADDDOWNLOADABLECONTENT__";
   static const char kMaxMissions[] = "__max_missions__";

   float oldCap = 0.0f;
   const int top = g_lua.gettop(L);
   __try {
      g_lua.pushlstring(L, kListening, sizeof(kListening) - 1);
      g_lua.gettable(L, -10001);
      if (g_lua.toboolean(L, -1)) {
         g_lua.pushlstring(L, kMaxMissions, sizeof(kMaxMissions) - 1);
         g_lua.gettable(L, -10001);
         if (g_lua.isnumber(L, -1) && g_lua.tonumber(L, -1) < kV13MissionCap) {
            oldCap = g_lua.tonumber(L, -1);
            g_lua.pushlstring(L, kMaxMissions, sizeof(kMaxMissions) - 1);
            g_lua.pushnumber(L, kV13MissionCap);
            g_lua.settable(L, -10001);
         }
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      oldCap = 0.0f;
   }
   g_lua.settop(L, top);

   if (oldCap > 0.0f)
      get_gamelog()("[DLC] Unofficial v1.3 patch detected, raised its mission cap from %d to %u\n",
                    (int)oldCap, kDlcMissionLimit);
}

static int __cdecl hooked_AddDownloadableContent(lua_State* L)
{
   raise_v13_mission_cap(L);
   return original_AddDownloadableContent(L);
}

bool dlc_v13_mission_cap_install(uintptr_t exe_base)
{
   // Optional support: returning false would drop the whole table extension,
   // so every decline below logs and still returns true.
   if (!g_addr->add_downloadable_content) {
      install_log("[DLC] v1.3 mission cap hook NOT installed: no address for this build");
      return true;
   }

   // Retail prologue: push ebp / mov ebp,esp / push ecx. Modtools is frame-less:
   // push ecx / mov eax,[L]. Both then save ebx/esi/edi and start reading arg 1.
   static const uint8_t kRetailHead[]   = {0x55, 0x8B, 0xEC, 0x51};
   static const uint8_t kModtoolsHead[] = {0x51, 0xA1};
   static const uint8_t kTail[]         = {0x53, 0x56, 0x57, 0x6A, 0x00, 0x6A, 0x01};

   const uint8_t* fn = (const uint8_t*)resolve(exe_base, g_addr->add_downloadable_content);
   const bool modtools = g_build == GameBuild::Modtools;
   const bool match = modtools ? !memcmp(fn, kModtoolsHead, sizeof(kModtoolsHead)) && !memcmp(fn + 6, kTail, sizeof(kTail))
                               : !memcmp(fn, kRetailHead, sizeof(kRetailHead)) && !memcmp(fn + 4, kTail, sizeof(kTail));
   if (!match) {
      install_log("[DLC] v1.3 mission cap hook NOT installed: signature mismatch at %08X",
                  (unsigned)g_addr->add_downloadable_content);
      return true;
   }

   original_AddDownloadableContent = (fn_AddDownloadableContent_t)fn;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_AddDownloadableContent, hooked_AddDownloadableContent);
   if (DetourTransactionCommit() != NO_ERROR) {
      install_log("[DLC] v1.3 mission cap hook NOT installed: detour failed");
      original_AddDownloadableContent = nullptr;
   }
   return true;
}
