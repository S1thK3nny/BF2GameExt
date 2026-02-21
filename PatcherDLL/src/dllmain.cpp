// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"

#include "apply_patches.hpp"
#include "lua_hooks.hpp"
#include "slim_vector.hpp"

void install_patches();

BOOL __declspec(dllexport) APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
   switch (ul_reason_for_call) {
   case DLL_PROCESS_ATTACH: {
      install_patches();
   } break;
   case DLL_THREAD_ATTACH:
   case DLL_THREAD_DETACH:
      break;
   case DLL_PROCESS_DETACH:
      lua_hooks_uninstall();
      break;
   }
   return TRUE;
}

void __declspec(dllexport) ExportFunction() {}

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
}
