// dllmain.cpp : Defines the entry point for BF2GameExt.dll
//
// This DLL contains all game patching and Lua hooking logic.
// It is loaded by the dinput8.dll proxy which calls BF2GameExt_Init
// with the exe base address and INI config path.

#include "pch.h"

#include "apply_patches.hpp"
#include "ini_config.hpp"
#include "lua_hooks.hpp"
#include "slim_vector.hpp"
#include "particle_renderer_patch.hpp"
#include "tentacle_patch.hpp"


static void install_patches(uintptr_t exe_base, const char* ini_path);

// ---------------------------------------------------------------------------
// Exported API
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) BOOL WINAPI BF2GameExt_Init(uintptr_t exe_base, const char* ini_path)
{
   install_patches(exe_base, ini_path);
   return TRUE;
}

extern "C" __declspec(dllexport) void WINAPI BF2GameExt_Shutdown()
{
   lua_hooks_uninstall();
   unpatch_particle_renderer();
   unpatch_tentacle_limit();
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/)
{
   switch (ul_reason_for_call) {
   case DLL_PROCESS_ATTACH:
      DisableThreadLibraryCalls(hModule);
      break;
   case DLL_THREAD_ATTACH:
   case DLL_THREAD_DETACH:
   case DLL_PROCESS_DETACH:
      break;
   }
   return TRUE;
}

// ---------------------------------------------------------------------------
// Patching
// ---------------------------------------------------------------------------

static void install_patches(uintptr_t exe_base, const char* ini_path)
{
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

   // Read INI config for patch/hook toggles
   ini_config cfg{ini_path};

   bool patches_enabled = cfg.get_bool("Patches", "HeapExtension", true)
                        || cfg.get_bool("Patches", "SoundLayerLimit", true)
                        || cfg.get_bool("Patches", "DLCMissionLimit", true)
                        || cfg.get_bool("Patches", "ParticleCacheIncrease", true);

   bool hooks_enabled = cfg.get_bool("Hooks", "LuaHooks", true);

   g_debugLogLevel = cfg.get_int("Debug", "LogLevel", 1);

   if (patches_enabled) {
      if (not apply_patches(exe_base, sections, ini_path)) {
         FatalAppExitA(0, "Failed to apply patches! Check \"BF2GameExt.log\" for more info.");
      }
   }

   if (cfg.get_bool("Patches", "ParticleCacheIncrease", true)) {
      patch_particle_renderer(exe_base);
   }

   // Tentacle bone limit patch — disabled until simulation reimplementation is debugged.
   // See memory/tentacle_rewrite.md for status and next steps.
   // if (cfg.get_bool("Patches", "TentacleBoneLimit", true)) {
   //    patch_tentacle_limit(exe_base);
   // }

   if (hooks_enabled) {
      lua_hooks_install(exe_base);
   }

   for (int i = 0; i < file_header.NumberOfSections; ++i) {
      if (not VirtualProtect(game_address + section_headers[i].VirtualAddress,
                              section_headers[i].Misc.VirtualSize, section_protection_values[i],
                              &section_protection_values[i])) {
         FatalAppExitA(0, "Failed to restore normal executable sections virtual protect!");
      }
   }

   if (hooks_enabled) {
      lua_hooks_post_install();
   }
}
