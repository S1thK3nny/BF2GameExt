// dllmain.cpp : Defines the entry point for the DLL application.
//
// This DLL serves double duty:
//   1. dinput8.dll proxy — the game imports DirectInput8Create from dinput8.dll.
//      Placing this DLL (named dinput8.dll) in the game directory causes it to
//      load instead of the system DLL. We forward the real call transparently.
//   2. BF2GameExt — all patching/hooking runs in DllMain after Steam DRM
//      has unpacked the real game code.
#include "pch.h"

#include "apply_patches.hpp"
#include "lua_hooks.hpp"
#include "slim_vector.hpp"

void install_patches();

// ---------------------------------------------------------------------------
// Real dinput8.dll forwarding
// ---------------------------------------------------------------------------

static HMODULE g_realDInput8 = nullptr;

using PFN_DirectInput8Create = HRESULT(WINAPI*)(
   HINSTANCE hinst, DWORD dwVersion, REFIID riidltf,
   void** ppvOut, void* punkOuter);

static PFN_DirectInput8Create g_realDI8Create = nullptr;

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
   HINSTANCE hinst, DWORD dwVersion, REFIID riidltf,
   void** ppvOut, void* punkOuter)
{
   if (!g_realDI8Create) return E_FAIL;
   return g_realDI8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/)
{
   switch (ul_reason_for_call) {
   case DLL_PROCESS_ATTACH: {
      DisableThreadLibraryCalls(hModule);

      // Chain: scan for dinput8_*.dll in our directory (e.g. dinput8_reshade.dll).
      // If found, load it and forward DirectInput8Create through it.
      // Otherwise fall back to the real system dinput8.dll.
      char myDir[MAX_PATH];
      GetModuleFileNameA(hModule, myDir, MAX_PATH);
      char* lastSlash = strrchr(myDir, '\\');
      if (lastSlash) *(lastSlash + 1) = '\0';

      char searchPath[MAX_PATH];
      wsprintfA(searchPath, "%sdinput8_*.dll", myDir);

      WIN32_FIND_DATAA fd;
      HANDLE hFind = FindFirstFileA(searchPath, &fd);
      if (hFind != INVALID_HANDLE_VALUE) {
         char chainPath[MAX_PATH];
         wsprintfA(chainPath, "%s%s", myDir, fd.cFileName);
         g_realDInput8 = LoadLibraryA(chainPath);
         FindClose(hFind);
      }

      if (!g_realDInput8) {
         char sysDir[MAX_PATH];
         GetSystemDirectoryA(sysDir, MAX_PATH);
         char realPath[MAX_PATH];
         wsprintfA(realPath, "%s\\dinput8.dll", sysDir);
         g_realDInput8 = LoadLibraryA(realPath);
      }

      if (g_realDInput8) {
         g_realDI8Create = (PFN_DirectInput8Create)
            GetProcAddress(g_realDInput8, "DirectInput8Create");
      }

      install_patches();
   } break;
   case DLL_THREAD_ATTACH:
   case DLL_THREAD_DETACH:
      break;
   case DLL_PROCESS_DETACH:
      lua_hooks_uninstall();
      if (g_realDInput8) {
         FreeLibrary(g_realDInput8);
         g_realDInput8 = nullptr;
      }
      break;
   }
   return TRUE;
}

void install_patches()
{
   char* const game_address = (char*)GetModuleHandleW(nullptr);

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

   if (not apply_patches((uintptr_t)game_address, sections)) {
      FatalAppExitA(0, "Failed to apply patches! Check \"BF2GameExt.log\" for more info.");
   }

   // Resolve Lua API addresses and register our custom functions into the live Lua state.
   lua_hooks_install((uintptr_t)game_address);

   for (int i = 0; i < file_header.NumberOfSections; ++i) {
      if (not VirtualProtect(game_address + section_headers[i].VirtualAddress,
                             section_headers[i].Misc.VirtualSize, section_protection_values[i],
                             &section_protection_values[i])) {
         FatalAppExitA(0, "Failed to restore normal executable sections virtual protect!");
      }
   }

   // Re-verify and re-patch IAT after protections are restored
   lua_hooks_post_install();
}
