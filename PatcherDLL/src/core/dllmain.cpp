// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"

#include "apply_patches.hpp"
#include "resolve.hpp"
#include "lua/lua_hooks.hpp"
#include "controller/controller_support.hpp"
#include "controller/controller_rumble.hpp"
#include "controller/aim_assist.hpp"
#include "entity/soldier_prone.hpp"
#include "util/crash_logger.hpp"
#include "util/ini_config.hpp"
#include "util/slim_vector.hpp"

static bool g_initialized = false;

static void install_patches_impl(uintptr_t exe_base, const char* ini_path);

// ---------------------------------------------------------------------------
// CombatHelper::DeadBodyCheck (0x5b4ec0) toggles.
//
// Vanilla behaviour: Alliance units (Team::mSide == 1) walk to and shoot
// nearby soldier corpses. Two optional byte patches control this. They are
// modtools VAs and are guarded by their expected original bytes, so they
// silently no-op on the GOG/Steam builds (different addresses).
//
//   disableAll  — NOP the first guard's JGE so DeadBodyCheck always returns
//                 false: NOBODY shoots corpses (this overrides allFactions,
//                 since the function bails before reaching the side gate).
//                   0x5b4ed3: 7D 07  JGE 0x5b4edc -> 90 90
//                   (falls through to the 0x5b4ed5 XOR AL,AL / RET epilogue)
//
//   allFactions — NOP the mSide==1 JNZ so the side gate always passes: ALL
//                 factions shoot corpses (the team != 0 null-check above is
//                 left intact).
//                   0x5b4f06: 75 16  JNZ 0x5b4f1e -> 90 90
//
// Must run while the executable sections are still RW (before re-protect).
// ---------------------------------------------------------------------------
static void apply_deadbody_check_patches(uintptr_t exe_base, bool disableAll, bool allFactions)
{
   if (disableAll) {
      uint8_t* p = (uint8_t*)resolve(exe_base, 0x5b4ed3);
      if (p[0] == 0x7D && p[1] == 0x07) { p[0] = 0x90; p[1] = 0x90; }
   }
   if (allFactions) {
      uint8_t* p = (uint8_t*)resolve(exe_base, 0x5b4f06);
      if (p[0] == 0x75 && p[1] == 0x16) { p[0] = 0x90; p[1] = 0x90; }
   }
}

// ---------------------------------------------------------------------------
// Proxy path: BF2GameExt_Init / BF2GameExt_Shutdown
// Called by DInput8Proxy after LoadLibrary. The proxy sets the
// BF2GAMEEXT_PROXY env var before loading us, so DllMain skips auto-init.
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) BOOL WINAPI BF2GameExt_Init(uintptr_t exe_base, const char* ini_path)
{
   if (g_initialized) return TRUE;
   install_patches_impl(exe_base, ini_path);
   return TRUE;
}

extern "C" __declspec(dllexport) void WINAPI BF2GameExt_Shutdown()
{
   if (!g_initialized) return;
   lua_hooks_uninstall();
   g_initialized = false;
}

// ---------------------------------------------------------------------------
// Exe patcher path: DllMain auto-init (no INI, no proxy)
// ---------------------------------------------------------------------------

BOOL __declspec(dllexport) APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
   switch (ul_reason_for_call) {
   case DLL_PROCESS_ATTACH: {
      // If the proxy loaded us, it will call BF2GameExt_Init explicitly.
      // Skip auto-init so the INI config is respected.
      char buf[2];
      if (GetEnvironmentVariableA("BF2GAMEEXT_PROXY", buf, sizeof(buf)) > 0)
         break;

      uintptr_t exe_base = (uintptr_t)GetModuleHandleW(nullptr);
      install_patches_impl(exe_base, nullptr);
   } break;
   case DLL_THREAD_ATTACH:
   case DLL_THREAD_DETACH:
      break;
   case DLL_PROCESS_DETACH:
      if (g_initialized) {
         lua_hooks_uninstall();
         g_initialized = false;
      }
      break;
   }
   return TRUE;
}

void __declspec(dllexport) ExportFunction() {}

// ---------------------------------------------------------------------------
// Shared init logic
// ---------------------------------------------------------------------------

static void install_patches_impl(uintptr_t exe_base, const char* ini_path)
{
   // First thing, before any patching: capture first-chance fatal exceptions
   // to BF2GameExt_crash.log (the game's own SEH handler can otherwise swallow
   // the crash and exit without a trace).
   crash_logger_install();

   char* const game_address = (char*)exe_base;

   IMAGE_DOS_HEADER& dos_header = *(IMAGE_DOS_HEADER*)game_address;
   IMAGE_NT_HEADERS32& nt_headers = *(IMAGE_NT_HEADERS32*)(game_address + dos_header.e_lfanew);
   IMAGE_FILE_HEADER& file_header = nt_headers.FileHeader;
   IMAGE_OPTIONAL_HEADER32& optional_header = nt_headers.OptionalHeader;

   assert(dos_header.e_magic == 'ZM');
   assert(nt_headers.Signature == 'EP');
   assert(optional_header.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC);

   const size_t section_headers_offset =
      dos_header.e_lfanew + (sizeof(IMAGE_NT_HEADERS32) - sizeof(IMAGE_OPTIONAL_HEADER32)) +
      file_header.SizeOfOptionalHeader;

   IMAGE_SECTION_HEADER* section_headers = (IMAGE_SECTION_HEADER*)(game_address + section_headers_offset);

   slim_vector<DWORD> section_protection_values{file_header.NumberOfSections,
                                                slim_vector<DWORD>::alloc_tag{}};

   slim_vector<section_info> sections{file_header.NumberOfSections,
                                      slim_vector<section_info>::alloc_tag{}};

   for (int i = 0; i < file_header.NumberOfSections; ++i) {
      if (not VirtualProtect(game_address + section_headers[i].VirtualAddress,
                             section_headers[i].Misc.VirtualSize, PAGE_READWRITE,
                             &section_protection_values[i])) {
         FatalAppExitA(0, "Failed to make executable sections writable!");
      }
   }

   for (int i = 0; i < file_header.NumberOfSections; ++i) {
      sections[i] = {
         .memory_start = game_address + section_headers[i].VirtualAddress,
         .file_start = section_headers[i].PointerToRawData,
         .file_end = section_headers[i].PointerToRawData + section_headers[i].SizeOfRawData,
      };
   }

   if (not apply_patches(exe_base, sections, ini_path)) {
      FatalAppExitA(0, "Failed to apply patches! Check \"BF2GameExt.log\" for more info.");
   }

   // Read INI toggles before installing hooks (some hooks check config at install time).
   // Defaults: dead-body shooting disabled for everyone; all-factions toggle off.
   bool disableDeadBody       = true;
   bool deadBodyAllFactions   = false;
   if (ini_path) {
      ini_config cfg{ini_path};
      g_useBarrelFireOrigin = cfg.get_bool("Fixes", "BarrelFireOriginFix", true);
      g_proneEnabled = cfg.get_bool("Features", "Prone", true);
      g_controllerEnabled = cfg.get_bool("Controller", "Enabled", true);
      g_rumbleEnabled = cfg.get_bool("Controller", "Rumble", true);
      disableDeadBody     = cfg.get_bool("Features", "DisableDeadBodyShooting", true);
      deadBodyAllFactions = cfg.get_bool("Features", "DeadBodyShootingAllFactions", false);
      controller_set_ini_path(ini_path);
      aim_assist_load_config(ini_path);
   } else {
      g_useBarrelFireOrigin = true;
      g_proneEnabled = true;
      g_controllerEnabled = true;
      g_rumbleEnabled = true;
   }

   // Sections are still RW here (re-protected below) — safe to apply byte patches.
   apply_deadbody_check_patches(exe_base, disableDeadBody, deadBodyAllFactions);

   // Resolve Lua API addresses and register our custom functions into the live Lua state.
   lua_hooks_install(exe_base);

   // Build-aware installers (select their address set from g_build / g_addr),
   // so they run on every identified build — not just modtools.  Called here,
   // outside the modtools-only lua_hooks_install, while sections are still RW.
   aim_assist_install(exe_base);
   prone_system_install(exe_base);

   for (int i = 0; i < file_header.NumberOfSections; ++i) {
      if (not VirtualProtect(game_address + section_headers[i].VirtualAddress,
                             section_headers[i].Misc.VirtualSize, section_protection_values[i],
                             &section_protection_values[i])) {
         FatalAppExitA(0, "Failed to restore normal executable sections virtual protect!");
      }
   }

   g_initialized = true;
}
