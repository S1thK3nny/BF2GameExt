#pragma once

#include <windows.h>

// =============================================================================
// Shader Patch detection
// =============================================================================
// Is Shader Patch's d3d9.dll wrapper loaded in this process?
//
// Detected by the export its author nominated for exactly this purpose, and
// undertook to keep exported even if the underlying functionality is removed:
//
//     ?prime_shader_cache@sp@@YAXXZ   ->  void sp::prime_shader_cache()
//
// It exists to build the shader cache at packaging time; we never call it, only
// ask whether it is there.  It comes from a __declspec(dllexport) in
// src/shader_cache_primer.cpp rather than from d3d9.def, so it does not depend
// on that file's export list.
//
// Testing for a d3d9.dll at all would not do: any wrapper, overlay or injector
// answers to that name, and if SP is absent the loaded module is the system
// d3d9.dll, which has no such export.  Testing for data\shaderpatch\ next to
// the exe would not do either — the folder can be left behind by an uninstall.
//
// Deliberately not resolved at install time.  d3d9.dll is loaded later than our
// DllMain, so a negative answer is only cached once the module is actually
// present; until then this keeps returning false without memoizing it.
inline bool shader_patch_present()
{
   static int s_present = -1;

   if (s_present < 0) {
      HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
      if (!d3d9) return false;
      s_present = GetProcAddress(d3d9, "?prime_shader_cache@sp@@YAXXZ") ? 1 : 0;
   }

   return s_present != 0;
}
