// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"

#include "apply_patches.hpp"
#include "lua/lua_hooks.hpp"
#include "util/slim_vector.hpp"

static bool g_initialized = false;

static void install_patches_impl(uintptr_t exe_base, const char* ini_path);

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

   // Resolve Lua API addresses and register our custom functions into the live Lua state.
   lua_hooks_install(exe_base);

   for (int i = 0; i < file_header.NumberOfSections; ++i) {
      if (not VirtualProtect(game_address + section_headers[i].VirtualAddress,
                             section_headers[i].Misc.VirtualSize, section_protection_values[i],
                             &section_protection_values[i])) {
         FatalAppExitA(0, "Failed to restore normal executable sections virtual protect!");
      }
   }

   g_initialized = true;
}
