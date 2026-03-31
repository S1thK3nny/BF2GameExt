// DInput8Proxy — thin dinput8.dll proxy that forwards DirectInput8Create to
// the real system DLL and optionally loads BF2GameExt.dll for game patching.
//
// Chain-loading: scans for dinput8_*.dll in our directory (e.g. dinput8_reshade.dll)
// and forwards through it if found, otherwise falls back to system32\dinput8.dll.
//
// Configuration is read from BF2GameExt.ini next to the proxy DLL.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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
// BF2GameExt.dll loading
// ---------------------------------------------------------------------------

static HMODULE g_bf2gameext = nullptr;

using PFN_BF2GameExt_Init     = BOOL(WINAPI*)(uintptr_t exe_base, const char* ini_path);
using PFN_BF2GameExt_Shutdown  = void(WINAPI*)();

static PFN_BF2GameExt_Shutdown g_bf2gameext_shutdown = nullptr;

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/)
{
   switch (ul_reason_for_call) {
   case DLL_PROCESS_ATTACH: {
      DisableThreadLibraryCalls(hModule);

      // Determine our directory
      char myDir[MAX_PATH];
      GetModuleFileNameA(hModule, myDir, MAX_PATH);
      char* lastSlash = strrchr(myDir, '\\');
      if (lastSlash) *(lastSlash + 1) = '\0';

      // --- Chain-loading: scan for dinput8_*.dll ---
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

      // Fall back to system dinput8.dll
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

      // --- Read INI config ---
      char iniPath[MAX_PATH];
      wsprintfA(iniPath, "%sBF2GameExt.ini", myDir);

      // [General] Enabled — defaults to 1 if INI missing
      int enabled = GetPrivateProfileIntA("General", "Enabled", 1, iniPath);
      if (!enabled) break;

      // [General] DLLPath — defaults to BF2GameExt.dll
      char dllName[MAX_PATH];
      GetPrivateProfileStringA("General", "DLLPath", "BF2GameExt.dll", dllName, MAX_PATH, iniPath);

      // Build full path relative to our directory
      char dllPath[MAX_PATH];
      wsprintfA(dllPath, "%s%s", myDir, dllName);

      // --- Load BF2GameExt.dll ---
      // Signal to BF2GameExt's DllMain that the proxy is handling initialization.
      // Without this, DllMain would auto-init (for the exe patcher path) and
      // BF2GameExt_Init's INI config would be ignored.
      SetEnvironmentVariableA("BF2GAMEEXT_PROXY", "1");

      g_bf2gameext = LoadLibraryA(dllPath);
      if (!g_bf2gameext) break;

      auto initFn = (PFN_BF2GameExt_Init)GetProcAddress(g_bf2gameext, "BF2GameExt_Init");
      g_bf2gameext_shutdown = (PFN_BF2GameExt_Shutdown)GetProcAddress(g_bf2gameext, "BF2GameExt_Shutdown");

      if (initFn) {
         uintptr_t exe_base = (uintptr_t)GetModuleHandleW(nullptr);
         initFn(exe_base, iniPath);
      }
   } break;

   case DLL_THREAD_ATTACH:
   case DLL_THREAD_DETACH:
      break;

   case DLL_PROCESS_DETACH:
      if (g_bf2gameext_shutdown) {
         g_bf2gameext_shutdown();
         g_bf2gameext_shutdown = nullptr;
      }
      if (g_bf2gameext) {
         FreeLibrary(g_bf2gameext);
         g_bf2gameext = nullptr;
      }
      if (g_realDInput8) {
         FreeLibrary(g_realDInput8);
         g_realDInput8 = nullptr;
      }
      break;
   }
   return TRUE;
}
