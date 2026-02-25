#include "pch.h"
#include "lua_funcs.hpp"
#include "lua_hooks.hpp"
#include <wininet.h>
#pragma comment(lib, "wininet.lib")

// Game's printf-style debug logger — FUN_007e3d50, __cdecl, (const char* fmt, ...)
typedef void (__cdecl* GameLog_t)(const char* fmt, ...);

// PrintToLog(msg) - writes a string to Bfront2.log
static int lua_PrintToLog(lua_State* L)
{
   const char* msg = g_lua.tolstring(L, 1, nullptr);
   if (msg) {
      FILE* f = nullptr;
      if (fopen_s(&f, "Bfront2.log", "a") == 0 && f) {
         fputs(msg, f);
         fputs("\n", f);
         fclose(f);
      }
   }
   return 0;
}

// GetSystemTickCount() - returns Windows tick count in milliseconds
static int lua_GetSystemTickCount(lua_State* L)
{
   g_lua.pushnumber(L, (float)GetTickCount());
   return 1;
}

// ReadTextFile(path) - reads any file on disk, returns its contents as a string.
// Accepts absolute or relative paths. Returns nil if the file cannot be opened.
// Example: local cfg = ReadTextFile("C:\\Users\\me\\Desktop\\config.txt")
static int lua_ReadTextFile(lua_State* L)
{
   const char* path = g_lua.tolstring(L, 1, nullptr);
   if (!path) { g_lua.pushnil(L); return 1; }

   FILE* f = nullptr;
   if (fopen_s(&f, path, "rb") != 0 || !f) { g_lua.pushnil(L); return 1; }

   fseek(f, 0, SEEK_END);
   long size = ftell(f);
   rewind(f);

   char* buf = (char*)malloc(size + 1);
   if (!buf) { fclose(f); g_lua.pushnil(L); return 1; }

   size_t read = fread(buf, 1, size, f);
   fclose(f);
   buf[read] = '\0';

   g_lua.pushlstring(L, buf, read);
   free(buf);
   return 1;
}

// HttpGet(url) - performs a synchronous HTTP GET, returns response body as a string.
// Returns nil on failure. Supports http and https.
// Example: local body = HttpGet("http://example.com/data.txt")
static int lua_HttpGet(lua_State* L)
{
   const char* url = g_lua.tolstring(L, 1, nullptr);
   if (!url) { g_lua.pushnil(L); return 1; }

   HINTERNET hNet = InternetOpenA("BF2GameExt", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
   if (!hNet) { g_lua.pushnil(L); return 1; }

   HINTERNET hUrl = InternetOpenUrlA(hNet, url, nullptr, 0,
                                     INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE |
                                     INTERNET_FLAG_IGNORE_CERT_CN_INVALID |
                                     INTERNET_FLAG_IGNORE_CERT_DATE_INVALID, 0);
   if (!hUrl) { InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }

   // Read response into a growable buffer
   char chunk[4096];
   DWORD bytes_read = 0;
   size_t total = 0;
   size_t capacity = 65536;
   char* buf = (char*)malloc(capacity);
   if (!buf) { InternetCloseHandle(hUrl); InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }

   while (InternetReadFile(hUrl, chunk, sizeof(chunk), &bytes_read) && bytes_read > 0) {
      if (total + bytes_read > capacity) {
         capacity *= 2;
         char* newbuf = (char*)realloc(buf, capacity);
         if (!newbuf) { free(buf); InternetCloseHandle(hUrl); InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }
         buf = newbuf;
      }
      memcpy(buf + total, chunk, bytes_read);
      total += bytes_read;
   }

   InternetCloseHandle(hUrl);
   InternetCloseHandle(hNet);

   g_lua.pushlstring(L, buf, total);
   free(buf);
   return 1;
}

// HttpPut(url, body) - performs a synchronous HTTP PUT, returns response body as a string.
// Returns nil on failure. Supports http and https.
// Uses the lower-level WinINet API (InternetConnect + HttpOpenRequest) because
// InternetOpenUrlA does not support specifying an HTTP method.
// Example: local resp = HttpPut("http://example.com/api/data", "{\"key\":\"value\"}")
static int lua_HttpPut(lua_State* L)
{
   const char* url = g_lua.tolstring(L, 1, nullptr);
   if (!url) { g_lua.pushnil(L); return 1; }

   size_t bodyLen = 0;
   const char* body = g_lua.tolstring(L, 2, &bodyLen);
   // body may be nil (PUT with empty body is valid)

   // Parse URL into host, path, port, scheme
   URL_COMPONENTSA uc = {};
   uc.dwStructSize = sizeof(uc);
   char hostBuf[256]  = {};
   char pathBuf[2048] = {};
   uc.lpszHostName     = hostBuf;
   uc.dwHostNameLength = sizeof(hostBuf);
   uc.lpszUrlPath      = pathBuf;
   uc.dwUrlPathLength  = sizeof(pathBuf);
   if (!InternetCrackUrlA(url, 0, 0, &uc)) { g_lua.pushnil(L); return 1; }

   const BOOL   isHttps = (uc.nScheme == INTERNET_SCHEME_HTTPS);
   const DWORD  flags   = INTERNET_FLAG_RELOAD |
                          (isHttps ? INTERNET_FLAG_SECURE |
                                     INTERNET_FLAG_IGNORE_CERT_CN_INVALID |
                                     INTERNET_FLAG_IGNORE_CERT_DATE_INVALID : 0);
   const INTERNET_PORT port = uc.nPort ? uc.nPort
                            : (isHttps ? INTERNET_DEFAULT_HTTPS_PORT
                                       : INTERNET_DEFAULT_HTTP_PORT);

   HINTERNET hNet = InternetOpenA("BF2GameExt", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
   if (!hNet) { g_lua.pushnil(L); return 1; }

   HINTERNET hConn = InternetConnectA(hNet, hostBuf, port,
                                      nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
   if (!hConn) { InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }

   const char* path = pathBuf[0] ? pathBuf : "/";
   HINTERNET hReq = HttpOpenRequestA(hConn, "PUT", path,
                                     nullptr, nullptr, nullptr, flags, 0);
   if (!hReq) { InternetCloseHandle(hConn); InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }

   if (!HttpSendRequestA(hReq, nullptr, 0,
                         (LPVOID)body, body ? (DWORD)bodyLen : 0)) {
      InternetCloseHandle(hReq); InternetCloseHandle(hConn); InternetCloseHandle(hNet);
      g_lua.pushnil(L); return 1;
   }

   // Read response into a growable buffer
   char chunk[4096];
   DWORD bytes_read = 0;
   size_t total = 0;
   size_t capacity = 65536;
   char* buf = (char*)malloc(capacity);
   if (!buf) { InternetCloseHandle(hReq); InternetCloseHandle(hConn); InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }

   while (InternetReadFile(hReq, chunk, sizeof(chunk), &bytes_read) && bytes_read > 0) {
      if (total + bytes_read > capacity) {
         capacity *= 2;
         char* newbuf = (char*)realloc(buf, capacity);
         if (!newbuf) { free(buf); InternetCloseHandle(hReq); InternetCloseHandle(hConn); InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }
         buf = newbuf;
      }
      memcpy(buf + total, chunk, bytes_read);
      total += bytes_read;
   }

   InternetCloseHandle(hReq);
   InternetCloseHandle(hConn);
   InternetCloseHandle(hNet);

   g_lua.pushlstring(L, buf, total);
   free(buf);
   return 1;
}

// HttpPost(url, body) - performs a synchronous HTTP POST with Content-Type: application/json.
// Returns response body as a string, or nil on failure. Supports http and https.
// Example: local resp = HttpPost("http://example.com/api", "{\"key\":\"value\"}")
static int lua_HttpPost(lua_State* L)
{
   const char* url = g_lua.tolstring(L, 1, nullptr);
   if (!url) { g_lua.pushnil(L); return 1; }

   size_t bodyLen = 0;
   const char* body = g_lua.tolstring(L, 2, &bodyLen);

   URL_COMPONENTSA uc = {};
   uc.dwStructSize = sizeof(uc);
   char hostBuf[256]  = {};
   char pathBuf[2048] = {};
   uc.lpszHostName     = hostBuf;
   uc.dwHostNameLength = sizeof(hostBuf);
   uc.lpszUrlPath      = pathBuf;
   uc.dwUrlPathLength  = sizeof(pathBuf);
   if (!InternetCrackUrlA(url, 0, 0, &uc)) { g_lua.pushnil(L); return 1; }

   const BOOL  isHttps = (uc.nScheme == INTERNET_SCHEME_HTTPS);
   const DWORD flags   = INTERNET_FLAG_RELOAD |
                         (isHttps ? INTERNET_FLAG_SECURE |
                                    INTERNET_FLAG_IGNORE_CERT_CN_INVALID |
                                    INTERNET_FLAG_IGNORE_CERT_DATE_INVALID : 0);
   const INTERNET_PORT port = uc.nPort ? uc.nPort
                            : (isHttps ? INTERNET_DEFAULT_HTTPS_PORT
                                       : INTERNET_DEFAULT_HTTP_PORT);

   HINTERNET hNet = InternetOpenA("BF2GameExt", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
   if (!hNet) { g_lua.pushnil(L); return 1; }

   HINTERNET hConn = InternetConnectA(hNet, hostBuf, port,
                                      nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
   if (!hConn) { InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }

   const char* path = pathBuf[0] ? pathBuf : "/";
   HINTERNET hReq = HttpOpenRequestA(hConn, "POST", path,
                                     nullptr, nullptr, nullptr, flags, 0);
   if (!hReq) { InternetCloseHandle(hConn); InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }

   static const char headers[] = "Content-Type: application/json\r\n";
   if (!HttpSendRequestA(hReq, headers, (DWORD)(sizeof(headers) - 1),
                         (LPVOID)body, body ? (DWORD)bodyLen : 0)) {
      InternetCloseHandle(hReq); InternetCloseHandle(hConn); InternetCloseHandle(hNet);
      g_lua.pushnil(L); return 1;
   }

   char chunk[4096];
   DWORD bytes_read = 0;
   size_t total = 0;
   size_t capacity = 65536;
   char* buf = (char*)malloc(capacity);
   if (!buf) { InternetCloseHandle(hReq); InternetCloseHandle(hConn); InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }

   while (InternetReadFile(hReq, chunk, sizeof(chunk), &bytes_read) && bytes_read > 0) {
      if (total + bytes_read > capacity) {
         capacity *= 2;
         char* newbuf = (char*)realloc(buf, capacity);
         if (!newbuf) { free(buf); InternetCloseHandle(hReq); InternetCloseHandle(hConn); InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }
         buf = newbuf;
      }
      memcpy(buf + total, chunk, bytes_read);
      total += bytes_read;
   }

   InternetCloseHandle(hReq);
   InternetCloseHandle(hConn);
   InternetCloseHandle(hNet);

   g_lua.pushlstring(L, buf, total);
   free(buf);
   return 1;
}

// ---------------------------------------------------------------------------
// GetCharacterWeapon(charIndex [, channel]) - returns the ODF name of the
// currently selected weapon in a given weapon channel.
//
// @param #int  charIndex   Integer character unit index (0-based)
// @param #int  channel     Weapon channel (default 0).
//                          0 = primary weapon channel (key 1)
//                          1 = secondary weapon channel (key 2)
// @return #string          ODF name (e.g. "rep_weap_dc-15s_blaster_carbine"), or nil.
//
// Resolution chain:
//   mCharacterStructArray + charIndex * 0x1B0  → charSlot
//   *(charSlot + 0x148)                        → intermediate
//   intermediate + 0x18                        → Controllable*
//   *(Controllable + 0x4D8 + slotIdx*4)        → Weapon* (slot array, up to 8)
//   *(Weapon + 0x060)                          → WeaponClass*
//   WeaponClass + 0x30                         → char[] ODF name
//
// Weapon index tracking (confirmed via runtime testing):
//   ctrl+0x4F8 is an array of bytes, one per weapon channel.
//   Each byte is a direct index into the weapon slot array at ctrl+0x4D8.
//     ctrl+0x4F8 byte[0] = selected slot index for channel 0 (primary)
//     ctrl+0x4F9 byte[1] = selected slot index for channel 1 (secondary)
//   General: *(uint8_t*)(ctrl + 0x4F8 + channel) = slot index for that channel.
// ---------------------------------------------------------------------------
static int lua_GetCharacterWeapon(lua_State* L)
{
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - 0x400000u + base; };

   if (!g_lua.isnumber(L, 1)) { g_lua.pushnil(L); return 1; }

   const int charIndex = g_lua.tointeger(L, 1);
   const int maxChars  = *(int*)res(0xB939F4);
   if (charIndex < 0 || charIndex >= maxChars) { g_lua.pushnil(L); return 1; }

   const uintptr_t arrayBase = *(uintptr_t*)res(0xB93A08);
   if (!arrayBase) { g_lua.pushnil(L); return 1; }

   const int channel = (g_lua.gettop(L) >= 2 && g_lua.isnumber(L, 2))
                       ? g_lua.tointeger(L, 2) : 0;
   if (channel < 0 || channel > 7) { g_lua.pushnil(L); return 1; }

   __try {
      char* charSlot     = (char*)arrayBase + charIndex * 0x1B0;
      char* intermediate = *(char**)(charSlot + 0x148);
      if (!intermediate) { g_lua.pushnil(L); return 1; }

      char* ctrl = intermediate + 0x18;  // Controllable*

      // Read the slot index for the requested channel
      uint8_t slotIdx = 0;
      __try { slotIdx = *(uint8_t*)((uintptr_t)ctrl + 0x4F8 + channel); }
      __except (EXCEPTION_EXECUTE_HANDLER) { g_lua.pushnil(L); return 1; }

      if (slotIdx >= 8) { g_lua.pushnil(L); return 1; }

      // Read weapon pointer from slot array
      uintptr_t wpn = 0;
      __try { wpn = *(uintptr_t*)((uintptr_t)ctrl + 0x4D8 + slotIdx * 4); }
      __except (EXCEPTION_EXECUTE_HANDLER) { g_lua.pushnil(L); return 1; }
      if (!wpn || wpn == 0xCDCDCDCDu) { g_lua.pushnil(L); return 1; }

      // Read WeaponClass pointer
      uintptr_t wc = 0;
      __try { wc = *(uintptr_t*)(wpn + 0x060); }
      __except (EXCEPTION_EXECUTE_HANDLER) { g_lua.pushnil(L); return 1; }
      if (!wc) { g_lua.pushnil(L); return 1; }

      // Read ODF name from WeaponClass
      const char* odfName = (const char*)(wc + 0x30);
      g_lua.pushlstring(L, odfName, strlen(odfName));
      return 1;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      g_lua.pushnil(L);
      return 1;
   }
}

// ---------------------------------------------------------------------------
// SetCharacterWeapon(charIndex, odfName [, channel]) - replaces the currently
// active weapon in a channel with a different already-loaded weapon ODF.
//
// @param #int    charIndex   Integer character unit index (0-based)
// @param #string odfName     ODF name to switch to (must be loaded by the level)
// @param #int    channel     Weapon channel (default 0): 0=primary, 1=secondary
// @return #bool              true on success, nil on failure.
//
// Mechanism:
//   1. Resolve charIndex → ctrl (same chain as GetCharacterWeapon)
//   2. Get the active weapon slot for the given channel
//   3. Walk the global WeaponClass linked list (WeaponClass+0x008 = next)
//      starting from the current weapon's WeaponClass, looking for odfName
//   4. Swap weapon+0x060, +0x064, +0x068 to point at the found WeaponClass
//   5. Call Controllable::SetWeaponIndex (0x005E6F70) with the same slot index to
//      re-trigger the game's own PlayAnimation path for the newly-swapped WeaponClass.
//
// Limitations:
//   - Only works with ODFs already loaded in memory (ReadDataFile'd at level load)
//   - Does NOT call Weapon::Init — ammo count and heat are NOT reset to the new
//     weapon's defaults. The engine reads damage/projectile/model from WeaponClass
//     at fire time, so behavior changes immediately even without Init.
static int lua_SetCharacterWeapon(lua_State* L)
{
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - 0x400000u + base; };
   const auto fn_GameLog = (GameLog_t)res(0x7E3D50);

   if (!g_lua.isnumber(L, 1)) { g_lua.pushnil(L); return 1; }

   const int   charIndex = g_lua.tointeger(L, 1);
   const char* targetOdf = g_lua.tolstring(L, 2, nullptr);
   if (!targetOdf || targetOdf[0] == '\0') { g_lua.pushnil(L); return 1; }

   const int channel = (g_lua.gettop(L) >= 3 && g_lua.isnumber(L, 3))
                       ? g_lua.tointeger(L, 3) : 0;
   if (channel < 0 || channel > 7) { g_lua.pushnil(L); return 1; }

   const int maxChars = *(int*)res(0xB939F4);
   if (charIndex < 0 || charIndex >= maxChars) { g_lua.pushnil(L); return 1; }

   const uintptr_t arrayBase = *(uintptr_t*)res(0xB93A08);
   if (!arrayBase) { g_lua.pushnil(L); return 1; }

   __try {
      char* charSlot     = (char*)arrayBase + charIndex * 0x1B0;
      char* intermediate = *(char**)(charSlot + 0x148);
      if (!intermediate) { g_lua.pushnil(L); return 1; }

      char* ctrl = intermediate + 0x18;  // Controllable*

      // Get active weapon slot for this channel.
      uint8_t slotIdx = 0;
      __try { slotIdx = *(uint8_t*)((uintptr_t)ctrl + 0x4F8 + channel); }
      __except (EXCEPTION_EXECUTE_HANDLER) { g_lua.pushnil(L); return 1; }
      if (slotIdx >= 8) { g_lua.pushnil(L); return 1; }

      uintptr_t wpn = 0;
      __try { wpn = *(uintptr_t*)((uintptr_t)ctrl + 0x4D8 + slotIdx * 4); }
      __except (EXCEPTION_EXECUTE_HANDLER) { g_lua.pushnil(L); return 1; }
      if (!wpn || wpn == 0xCDCDCDCDu) { g_lua.pushnil(L); return 1; }

      uintptr_t startWc = 0;
      __try { startWc = *(uintptr_t*)(wpn + 0x060); }
      __except (EXCEPTION_EXECUTE_HANDLER) { g_lua.pushnil(L); return 1; }
      if (!startWc || startWc == 0xCDCDCDCDu) { g_lua.pushnil(L); return 1; }

      // Walk the WeaponClass global linked list.
      // Flink/Blink (WC+0x008/0x00C) store adjacentWC+0x004; subtract 4 when following.
      // Name matching: accept exact OR suffix so callers can omit faction prefixes.
      auto wcNameMatches = [](const char* wcName, const char* target) -> bool {
         if (_stricmp(wcName, target) == 0) return true;
         size_t wl = strlen(wcName), tl = strlen(target);
         return (wl > tl && _stricmp(wcName + wl - tl, target) == 0);
      };

      uintptr_t foundWc  = 0;
      uintptr_t searchWc = startWc;
      for (int guard = 0; guard < 512; guard++) {
         __try {
            const char* name = (const char*)(searchWc + 0x30);
            if (wcNameMatches(name, targetOdf)) { foundWc = searchWc; break; }
            uintptr_t linkRaw = *(uintptr_t*)(searchWc + 0x008);
            if (!linkRaw || linkRaw == 0xCDCDCDCDu || linkRaw < 0x01000000u) break;
            uintptr_t nextWc = linkRaw - 0x004;
            if (nextWc == startWc) break;
            searchWc = nextWc;
         }
         __except (EXCEPTION_EXECUTE_HANDLER) { break; }
      }

      if (!foundWc) {
         // Walk backwards in case the target is behind the start node.
         searchWc = startWc;
         for (int guard = 0; guard < 512; guard++) {
            __try {
               uintptr_t linkRaw = *(uintptr_t*)(searchWc + 0x00C);
               if (!linkRaw || linkRaw == 0xCDCDCDCDu || linkRaw < 0x01000000u) break;
               uintptr_t prevWc = linkRaw - 0x004;
               if (prevWc == startWc) break;
               const char* name = (const char*)(prevWc + 0x30);
               if (wcNameMatches(name, targetOdf)) { foundWc = prevWc; break; }
               searchWc = prevWc;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { break; }
         }
      }

      if (!foundWc) {
         fn_GameLog("SetCharacterWeapon: '%s' not found in loaded WeaponClass list.\n", targetOdf);
         g_lua.pushnil(L);
         return 1;
      }

      // Scan for a live weapon of the target type to borrow its OrdnanceClass* and vtable.
      uintptr_t sourceWpn = 0;
      int scanMax = 0;
      __try { scanMax = *(int*)res(0xB939F4); } __except(EXCEPTION_EXECUTE_HANDLER) {}
      const int scanLimit = (scanMax < 512) ? 512 : scanMax;
      __try {
         for (int ci = 0; ci < scanLimit && !sourceWpn; ci++) {
            if (ci == charIndex) continue;
            __try {
               char* cs2 = (char*)arrayBase + ci * 0x1B0;
               char* im2 = *(char**)(cs2 + 0x148);
               if (!im2 || im2 == (char*)0xCDCDCDCDu) continue;
               char* ct2 = im2 + 0x18;
               for (int si = 0; si < 8 && !sourceWpn; si++) {
                  __try {
                     uintptr_t w = *(uintptr_t*)((uintptr_t)ct2 + 0x4D8 + si * 4);
                     if (!w || w == 0xCDCDCDCDu) continue;
                     uintptr_t wc = *(uintptr_t*)(w + 0x060);
                     if (!wc || wc == 0xCDCDCDCDu) continue;
                     const char* wcName = (const char*)(wc + 0x30);
                     __try {
                        size_t wl = strlen(wcName), tl = strlen(targetOdf);
                        if ((_stricmp(wcName, targetOdf) == 0) ||
                            (wl > tl && _stricmp(wcName + wl - tl, targetOdf) == 0))
                           sourceWpn = w;
                     } __except(EXCEPTION_EXECUTE_HANDLER) {}
                  } __except(EXCEPTION_EXECUTE_HANDLER) {}
               }
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
         }
      } __except(EXCEPTION_EXECUTE_HANDLER) {}

      // Patch factory+0x18 (OrdnanceClass*) and factory+0x1c from the source weapon.
      if (sourceWpn) {
         uintptr_t srcFactory = 0, playerFactory = 0;
         __try { srcFactory    = *(uintptr_t*)(sourceWpn + 0x088); } __except(EXCEPTION_EXECUTE_HANDLER) {}
         __try { playerFactory = *(uintptr_t*)(wpn        + 0x088); } __except(EXCEPTION_EXECUTE_HANDLER) {}
         if (srcFactory && playerFactory) {
            uintptr_t ord18 = 0; uint32_t val1c = 0;
            __try { ord18 = *(uintptr_t*)(srcFactory + 0x018); } __except(EXCEPTION_EXECUTE_HANDLER) {}
            __try { val1c = *(uint32_t* )(srcFactory + 0x01c); } __except(EXCEPTION_EXECUTE_HANDLER) {}
            __try { *(uintptr_t*)(playerFactory + 0x018) = ord18; } __except(EXCEPTION_EXECUTE_HANDLER) {}
            __try { *(uint32_t* )(playerFactory + 0x01c) = val1c; } __except(EXCEPTION_EXECUTE_HANDLER) {}
         }

         // Swap vtable so virtual dispatch matches the target weapon type.
         uintptr_t srcVtable = 0;
         __try { srcVtable = *(uintptr_t*)(sourceWpn); } __except(EXCEPTION_EXECUTE_HANDLER) {}
         if (srcVtable && srcVtable != 0xCDCDCDCDu)
            __try { *(uintptr_t*)(wpn) = srcVtable; } __except(EXCEPTION_EXECUTE_HANDLER) {}
      }

      // Write all three WC pointers in the Weapon instance.
      __try { *(uintptr_t*)(wpn + 0x060) = foundWc; } __except(EXCEPTION_EXECUTE_HANDLER) {}
      __try { *(uintptr_t*)(wpn + 0x064) = foundWc; } __except(EXCEPTION_EXECUTE_HANDLER) {}
      __try { *(uintptr_t*)(wpn + 0x068) = foundWc; } __except(EXCEPTION_EXECUTE_HANDLER) {}

      // TODO: trigger per-character animation bank update here.

      fn_GameLog("SetCharacterWeapon: char %d ch %d slot[%d] -> '%s' (newWc=0x%08x src=%s)\n",
                 charIndex, channel, slotIdx, targetOdf,
                 (unsigned)foundWc, sourceWpn ? "found" : "none");

      g_lua.pushnumber(L, 1);
      return 1;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      g_lua.pushnil(L);
      return 1;
   }
}


// ---------------------------------------------------------------------------
// RemoveUnitClass(teamIndex, unitClass) - removes a unit class from a team.
//
// Reverse of AddUnitClass. Finds the class in the global class def list,
// locates it in the team's parallel arrays, then left-shifts remaining entries
// to preserve order and keep the arrays compact.
//
// @param #int    teamIndex   Index of team (0-based)
// @param #string unitClass   ODF class name (e.g. "imp_inf_trooper")
// ---------------------------------------------------------------------------

// Team::SetUnitClassMinMax — __thiscall on team object.
// Writes min/max into their respective parallel arrays and fires the change
// notification (thunk_FUN_00661e00).
typedef void (__thiscall* SetUnitClassMinMax_t)(void* team, int slot, int min, int max);

// Game's printf-style debug logger — same one vanilla scripts call for error output.

static int lua_RemoveUnitClass(lua_State* L)
{
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t addr) -> uintptr_t { return addr - 0x400000u + base; };
   const auto fn_GameLog = (GameLog_t)res(0x7E3D50);

   if (!g_lua.isnumber(L, 1)) return 0;

   // arg2 can be a string (ODF name) or a number (0-based slot index within the team).
   const bool byIndex   = g_lua.isnumber(L, 2) != 0;
   const char* unitClass = byIndex ? nullptr : g_lua.tolstring(L, 2, nullptr);
   if (!byIndex && !unitClass) return 0;

   const int teamIndex = g_lua.tointeger(L, 1);
   if (teamIndex < 0 || teamIndex >= 8) {
      fn_GameLog("RemoveUnitClass(): teamIndex %d out of range (0-7)\n", teamIndex);
      return 0;
   }

   // Get team pointer from g_ppTeams[teamIndex].
   // 0xAD5D64 is a pointer variable whose value is the team array base — two dereferences needed.
   const uintptr_t teamArrayBase = *(uintptr_t*)res(0xAD5D64);
   void* teamPtr = *(void**)(teamArrayBase + (uintptr_t)teamIndex * 4);
   if (!teamPtr) {
      fn_GameLog("RemoveUnitClass(): team %d is null\n", teamIndex);
      return 0;
   }

   const int classCount = *(int*)((char*)teamPtr + 0x48);
   void** classDefArr   = *(void***)((char*)teamPtr + 0x50);

   int foundSlot = -1;

   if (byIndex) {
      // Direct slot index — no global registry lookup needed.
      const int classIndex = g_lua.tointeger(L, 2);
      if (classIndex < 0 || classIndex >= classCount) {
         fn_GameLog("RemoveUnitClass(): class index %d out of range (team %d has %d classes)\n",
                    classIndex, teamIndex, classCount);
         return 0;
      }
      foundSlot = classIndex;
   } else {
      // ODF name — walk g_ClassDefList to get the classDef pointer, then match by pointer.
      // Node layout: +0x04 = next, +0x0c = classDef ptr (null = end of list).
      // classDef+0x18 = integer name hash (NOT a char* — do not dereference as string).
      //
      // HashString: __thiscall, ECX = 8-byte stack buffer, stack arg = name string.
      // buf[0] is the resulting integer hash.
      typedef void* (__thiscall* HashString_t)(void* buf, const char* name);
      const auto fn_HashString = (HashString_t)res(0x7E1BD0);
      alignas(4) int hashBuf[2] = {};
      fn_HashString(hashBuf, unitClass);
      const int targetHash = hashBuf[0];

      uintptr_t node = *(uintptr_t*)res(0xACD2C8);
      void* classDef = nullptr;
      for (int guard = 0; guard < 1024; ++guard) {
         void* element = *(void**)(node + 0x0c);
         if (!element) break;
         if (*(int*)((char*)element + 0x18) == targetHash) { classDef = element; break; }
         node = *(uintptr_t*)(node + 0x04);
      }
      if (!classDef) {
         fn_GameLog("RemoveUnitClass(): class \"%s\" not found in global registry (check the side's .req file)\n", unitClass);
         return 0;
      }

      for (int i = 0; i < classCount; ++i) {
         if (classDefArr[i] == classDef) { foundSlot = i; break; }
      }
      if (foundSlot < 0) {
         fn_GameLog("RemoveUnitClass(): class \"%s\" is not assigned to team %d\n", unitClass, teamIndex);
         return 0;
      }
   }

   // Left-shift removal: slide every entry after foundSlot one position left,
   // preserving spawn menu order. classCount is decremented so the freed slot
   // is immediately available for a subsequent AddUnitClass call.
   //
   // NOTE: the spawner (FUN_006470f0) pre-caches slot indices for characters
   // already queued to spawn. After the shift, any character cached on the old
   // lastSlot index will call Character::SetClass(lastSlot) → null → a one-time
   // "Trying to spawn a character with no class" warning in the log. That
   // character is skipped for that spawn tick and respawns normally on the next
   // cycle. The warning is harmless and the alternative (tombstoning) prevents
   // slot reuse, breaking the add/remove cycle.
   int* minArr = *(int**)((char*)teamPtr + 0x54);
   int* maxArr = *(int**)((char*)teamPtr + 0x58);
   const int lastSlot = classCount - 1;

   for (int i = foundSlot; i < lastSlot; ++i) {
      classDefArr[i] = classDefArr[i + 1];
      minArr[i]      = minArr[i + 1];
      maxArr[i]      = maxArr[i + 1];
   }

   classDefArr[lastSlot] = nullptr;
   minArr[lastSlot]      = 0;
   maxArr[lastSlot]      = 0;
   *(int*)((char*)teamPtr + 0x48) = lastSlot;

   // Return the ODF name when called by name; nil when called by index
   // (classDef+0x18 is only a hash — the name string offset is unconfirmed).
   if (unitClass) {
      g_lua.pushlstring(L, unitClass, strlen(unitClass));
      return 1;
   }
   return 0;
}

// ---------------------------------------------------------------------------
// Async HTTP infrastructure — fire-and-forget background threads
//
// Each Async variant copies the URL/body onto the heap, spawns a thread,
// and returns immediately. The thread does the request, discards the response,
// frees the work struct, and exits. The game thread is never blocked.
// ---------------------------------------------------------------------------

struct HttpAsyncWork {
   char*  url;
   char*  body;       // null = no body
   size_t bodyLen;
   char   method[8];  // "GET", "PUT", "POST", …
   char*  headers;    // null = no extra headers
};

static void http_async_free(HttpAsyncWork* w)
{
   free(w->url);
   free(w->body);
   free(w->headers);
   free(w);
}

static DWORD WINAPI http_async_worker(LPVOID param)
{
   HttpAsyncWork* w = (HttpAsyncWork*)param;

   if (strcmp(w->method, "GET") == 0) {
      // High-level path: InternetOpenUrlA is fine for GET.
      HINTERNET hNet = InternetOpenA("BF2GameExt", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
      if (hNet) {
         HINTERNET hUrl = InternetOpenUrlA(hNet, w->url, nullptr, 0,
                                           INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE |
                                           INTERNET_FLAG_IGNORE_CERT_CN_INVALID |
                                           INTERNET_FLAG_IGNORE_CERT_DATE_INVALID, 0);
         if (hUrl) {
            char chunk[4096]; DWORD br = 0;
            while (InternetReadFile(hUrl, chunk, sizeof(chunk), &br) && br > 0) {}
            InternetCloseHandle(hUrl);
         }
         InternetCloseHandle(hNet);
      }
   } else {
      // Low-level path: InternetConnect + HttpOpenRequest (required to set method / headers).
      URL_COMPONENTSA uc = {};
      uc.dwStructSize = sizeof(uc);
      char hostBuf[256] = {}, pathBuf[2048] = {};
      uc.lpszHostName     = hostBuf; uc.dwHostNameLength = sizeof(hostBuf);
      uc.lpszUrlPath      = pathBuf; uc.dwUrlPathLength  = sizeof(pathBuf);

      if (InternetCrackUrlA(w->url, 0, 0, &uc)) {
         const BOOL  isHttps = (uc.nScheme == INTERNET_SCHEME_HTTPS);
         const DWORD flags   = INTERNET_FLAG_RELOAD |
                               (isHttps ? INTERNET_FLAG_SECURE |
                                          INTERNET_FLAG_IGNORE_CERT_CN_INVALID |
                                          INTERNET_FLAG_IGNORE_CERT_DATE_INVALID : 0);
         const INTERNET_PORT port = uc.nPort ? uc.nPort
                                  : (isHttps ? INTERNET_DEFAULT_HTTPS_PORT
                                             : INTERNET_DEFAULT_HTTP_PORT);

         HINTERNET hNet = InternetOpenA("BF2GameExt", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
         if (hNet) {
            HINTERNET hConn = InternetConnectA(hNet, hostBuf, port,
                                               nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
            if (hConn) {
               const char* path = pathBuf[0] ? pathBuf : "/";
               HINTERNET hReq = HttpOpenRequestA(hConn, w->method, path,
                                                 nullptr, nullptr, nullptr, flags, 0);
               if (hReq) {
                  DWORD hdrLen = w->headers ? (DWORD)strlen(w->headers) : 0;
                  if (HttpSendRequestA(hReq, w->headers, hdrLen,
                                       (LPVOID)w->body, w->body ? (DWORD)w->bodyLen : 0)) {
                     char chunk[4096]; DWORD br = 0;
                     while (InternetReadFile(hReq, chunk, sizeof(chunk), &br) && br > 0) {}
                  }
                  InternetCloseHandle(hReq);
               }
               InternetCloseHandle(hConn);
            }
            InternetCloseHandle(hNet);
         }
      }
   }

   http_async_free(w);
   return 0;
}

// Allocates a work item, copies all strings, spawns the thread, and detaches it.
// Returns true if the thread was created successfully.
static bool http_fire_and_forget(const char* method, const char* url,
                                 const char* body, size_t bodyLen,
                                 const char* headers)
{
   HttpAsyncWork* w = (HttpAsyncWork*)malloc(sizeof(HttpAsyncWork));
   if (!w) return false;
   memset(w, 0, sizeof(*w));

   w->url = _strdup(url);
   if (!w->url) { http_async_free(w); return false; }

   if (body && bodyLen > 0) {
      w->body = (char*)malloc(bodyLen);
      if (!w->body) { http_async_free(w); return false; }
      memcpy(w->body, body, bodyLen);
      w->bodyLen = bodyLen;
   }

   if (headers) {
      w->headers = _strdup(headers);
      if (!w->headers) { http_async_free(w); return false; }
   }

   strncpy_s(w->method, sizeof(w->method), method, _TRUNCATE);

   HANDLE hThread = CreateThread(nullptr, 0, http_async_worker, w, 0, nullptr);
   if (!hThread) { http_async_free(w); return false; }
   CloseHandle(hThread);  // detach — thread frees w on exit
   return true;
}

// HttpGetAsync(url) - fire-and-forget HTTP GET. Returns immediately.
static int lua_HttpGetAsync(lua_State* L)
{
   const char* url = g_lua.tolstring(L, 1, nullptr);
   if (url) http_fire_and_forget("GET", url, nullptr, 0, nullptr);
   return 0;
}

// HttpPutAsync(url, body) - fire-and-forget HTTP PUT. Returns immediately.
static int lua_HttpPutAsync(lua_State* L)
{
   const char* url = g_lua.tolstring(L, 1, nullptr);
   if (!url) return 0;
   size_t bodyLen = 0;
   const char* body = g_lua.tolstring(L, 2, &bodyLen);
   http_fire_and_forget("PUT", url, body, bodyLen, nullptr);
   return 0;
}

// HttpPostAsync(url, body) - fire-and-forget HTTP POST with Content-Type: application/json.
// Returns immediately. Ideal for Discord webhooks and other event notifications.
static int lua_HttpPostAsync(lua_State* L)
{
   const char* url = g_lua.tolstring(L, 1, nullptr);
   if (!url) return 0;
   size_t bodyLen = 0;
   const char* body = g_lua.tolstring(L, 2, &bodyLen);
   http_fire_and_forget("POST", url, body, bodyLen, "Content-Type: application/json\r\n");
   return 0;
}


// ---------------------------------------------------------------------------
// ReapplyAnimations() - calls SoldierAnimatorClass::AssignAnimations (0x00581AF0).
// Re-wires all weapon animation banks for every soldier in the level.
// Call this after SetCharacterWeapon if animations haven't updated visually.
// ---------------------------------------------------------------------------
static int lua_ReapplyAnimations(lua_State* L)
{
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - 0x400000u + base; };

   typedef void (__fastcall* AssignAnimations_t)(void*);
   const auto fn_assign = (AssignAnimations_t)res(0x581AF0);
   void* animInst = *(void**)res(0xB8D3C4);
   if (!animInst) { g_lua.pushnil(L); return 1; }

   __try { fn_assign(animInst); } __except(EXCEPTION_EXECUTE_HANDLER) { g_lua.pushnil(L); return 1; }

   g_lua.pushnumber(L, 1);
   return 1;
}


// ---------------------------------------------------------------------------
// OnCharacterExitVehicle / Name / Team / Class / Release
//
// Custom Lua C functions that store callbacks in the Lua registry and track
// filter metadata in g_cevCallbacks[].  The C++ hook in lua_hooks.cpp scans
// the character array, resolves name/team/class, and fires matching callbacks.
//
// Returns a lightuserdata handle (pointer to the g_cevCallbacks slot).
// ReleaseCharacterExitVehicle(handle) clears the slot and nils the registry entry.
// ---------------------------------------------------------------------------

// Helper: store the value at Lua stack top into globals[key].
// Pops the value.  Returns the key.
// Uses LUA_GLOBALSINDEX (-10001) with negative integer keys to avoid
// conflicts with luaL_ref positive keys in LUA_REGISTRYINDEX (-10000).
static int cev_store_ref(lua_State* L)
{
   int key = g_cevNextKey--;
   // Stack: [..., value]
   g_lua.pushnumber(L, (float)key);
   g_lua.insert(L, -2);
   // Stack: [..., key, value]
   g_lua.settable(L, -10001);   // _G[key] = value
   return key;
}

// Helper: remove a globals reference.
static void cev_remove_ref(lua_State* L, int key)
{
   g_lua.pushnumber(L, (float)key);
   g_lua.pushnil(L);
   g_lua.settable(L, -10001);   // _G[key] = nil
}

// OnCharacterExitVehicle(callback) -> handle
static int lua_OnCEV(lua_State* L)
{
   for (int i = 0; i < CEV_MAX_CBS; i++) {
      if (g_cevCallbacks[i].regKey == 0) {
         int key = cev_store_ref(L);
         g_cevCallbacks[i].regKey     = key;
         g_cevCallbacks[i].filterType = CEV_PLAIN;
         g_lua.pushlightuserdata(L, (void*)&g_cevCallbacks[i]);
         return 1;
      }
   }
   g_lua.pushnil(L);
   return 1;
}

// OnCharacterExitVehicleName(callback, nameStr) -> handle
static int lua_OnCEVName(lua_State* L)
{
   const char* name = g_lua.tolstring(L, 2, nullptr);
   if (!name) { g_lua.pushnil(L); return 1; }

   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - 0x400000u + base; };
   typedef void* (__thiscall* HashString_t)(void* buf, const char* s);
   const auto fn_Hash = (HashString_t)res(0x7E1BD0);
   alignas(4) int hashBuf[2] = {};
   fn_Hash(hashBuf, name);
   const uint32_t nameHash = (uint32_t)hashBuf[0];

   for (int i = 0; i < CEV_MAX_CBS; i++) {
      if (g_cevCallbacks[i].regKey == 0) {
         g_cevCallbacks[i].nameHash   = nameHash;
         g_lua.settop(L, 1);
         int key = cev_store_ref(L);
         g_cevCallbacks[i].regKey     = key;
         g_cevCallbacks[i].filterType = CEV_NAME;
         g_lua.pushlightuserdata(L, (void*)&g_cevCallbacks[i]);
         return 1;
      }
   }
   g_lua.pushnil(L);
   return 1;
}

// OnCharacterExitVehicleTeam(callback, teamIndex) -> handle
static int lua_OnCEVTeam(lua_State* L)
{
   if (!g_lua.isnumber(L, 2)) { g_lua.pushnil(L); return 1; }
   int team = g_lua.tointeger(L, 2);

   for (int i = 0; i < CEV_MAX_CBS; i++) {
      if (g_cevCallbacks[i].regKey == 0) {
         g_cevCallbacks[i].teamFilter = team;
         g_lua.settop(L, 1);
         int key = cev_store_ref(L);
         g_cevCallbacks[i].regKey     = key;
         g_cevCallbacks[i].filterType = CEV_TEAM;
         g_lua.pushlightuserdata(L, (void*)&g_cevCallbacks[i]);
         return 1;
      }
   }
   g_lua.pushnil(L);
   return 1;
}

// OnCharacterExitVehicleClass(callback, classStr) -> handle
static int lua_OnCEVClass(lua_State* L)
{
   const char* cls = g_lua.tolstring(L, 2, nullptr);
   if (!cls) { g_lua.pushnil(L); return 1; }

   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - 0x400000u + base; };
   typedef void* (__thiscall* HashString_t)(void* buf, const char* s);
   const auto fn_Hash = (HashString_t)res(0x7E1BD0);
   const auto fn_GameLog = (GameLog_t)res(0x7E3D50);

   // Hash the class name and walk the EntityClass global registry to resolve it
   // to a live EntityClass pointer. Registration fails if the class isn't loaded.
   alignas(4) int hashBuf[2] = {};
   fn_Hash(hashBuf, cls);
   const uint32_t targetHash = (uint32_t)hashBuf[0];

   void* classPtr = nullptr;
   uintptr_t node = *(uintptr_t*)res(0xACD2C8);
   for (int guard = 0; guard < 4096; ++guard) {
      void* ec = *(void**)(node + 0x0C);
      if (!ec) break;
      if (*(uint32_t*)((char*)ec + 0x18) == targetHash) { classPtr = ec; break; }
      node = *(uintptr_t*)(node + 0x04);
   }

   if (!classPtr) {
      fn_GameLog("OnCharacterExitVehicleClass: class '%s' not found in EntityClass registry\n", cls);
      g_lua.pushnil(L);
      return 1;
   }

   for (int i = 0; i < CEV_MAX_CBS; i++) {
      if (g_cevCallbacks[i].regKey == 0) {
         g_cevCallbacks[i].classPtr   = classPtr;
         g_lua.settop(L, 1);
         int key = cev_store_ref(L);
         g_cevCallbacks[i].regKey     = key;
         g_cevCallbacks[i].filterType = CEV_CLASS;
         g_lua.pushlightuserdata(L, (void*)&g_cevCallbacks[i]);
         return 1;
      }
   }
   g_lua.pushnil(L);
   return 1;
}

// ReleaseCharacterExitVehicle(handle)
static int lua_ReleaseCEV(lua_State* L)
{
   void* handle = g_lua.touserdata(L, 1);
   if (!handle) return 0;

   CEVCallback* cb = (CEVCallback*)handle;
   if (cb >= g_cevCallbacks && cb < g_cevCallbacks + CEV_MAX_CBS && cb->regKey != 0) {
      cev_remove_ref(L, cb->regKey);
      memset(cb, 0, sizeof(*cb));
   }
   return 0;
}

// SetBarrelFireOrigin(enable) - toggle barrel-origin fire position.
// When true, projectiles originate from the weapon's barrel hardpoint
// (mFirePointMatrix) instead of the default aimer position.
// Swaps the WeaponCannon vtable entry between our hook and vanilla.
// Accepts both number (0/1) and boolean (true/false) — in Lua 5.0
// the number 0 is truthy, so we check isnumber first.
extern void** g_cannonOverrideAimerSlot;
extern void*  g_cannonOverrideAimerOrig;
extern void*  g_cannonOverrideAimerHook;

static int lua_SetBarrelFireOrigin(lua_State* L)
{
   if (g_lua.isnumber(L, 1))
      g_useBarrelFireOrigin = g_lua.tonumber(L, 1) != 0.0f;
   else
      g_useBarrelFireOrigin = g_lua.toboolean(L, 1) != 0;

   if (g_cannonOverrideAimerSlot) {
      DWORD oldProt;
      if (VirtualProtect(g_cannonOverrideAimerSlot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
         *g_cannonOverrideAimerSlot = g_useBarrelFireOrigin
            ? g_cannonOverrideAimerHook
            : g_cannonOverrideAimerOrig;
         VirtualProtect(g_cannonOverrideAimerSlot, sizeof(void*), oldProt, &oldProt);
      }
   }
   return 0;
}

// DumpAimerInfo(charIndex [, channel]) - diagnostic: logs aimer positions to Bfront2.log.
// Dumps mFirePos, mMountPos, mBarrelPoseMatrix[0..3] trans, and mCurrentBarrel.
static int lua_DumpAimerInfo(lua_State* L)
{
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - 0x400000u + base; };

   if (!g_lua.isnumber(L, 1)) return 0;

   const int charIndex = g_lua.tointeger(L, 1);
   const int maxChars  = *(int*)res(0xB939F4);
   if (charIndex < 0 || charIndex >= maxChars) return 0;

   const uintptr_t arrayBase = *(uintptr_t*)res(0xB93A08);
   if (!arrayBase) return 0;

   const int channel = (g_lua.gettop(L) >= 2 && g_lua.isnumber(L, 2))
                       ? g_lua.tointeger(L, 2) : 0;

   __try {
      char* charSlot     = (char*)arrayBase + charIndex * 0x1B0;
      char* intermediate = *(char**)(charSlot + 0x148);
      if (!intermediate) return 0;
      char* ctrl = intermediate + 0x18;

      // Get weapon from channel
      uint8_t slotIdx = *(uint8_t*)((uintptr_t)ctrl + 0x4F8 + channel);
      if (slotIdx >= 8) return 0;
      uintptr_t wpn = *(uintptr_t*)((uintptr_t)ctrl + 0x4D8 + slotIdx * 4);
      if (!wpn) return 0;

      // Weapon::mAimer at weapon+0x70
      void* aimer = *(void**)(wpn + 0x70);
      if (!aimer) return 0;

      FILE* f = nullptr;
      if (fopen_s(&f, "Bfront2.log", "a") != 0 || !f) return 0;

      float* firePos  = (float*)((char*)aimer + 0x88);
      float* mountPos = (float*)((char*)aimer + 0x54);
      float* rootPos  = (float*)((char*)aimer + 0x70);
      int barrel      = *(int*)((char*)aimer + 0x204);

      // Weapon::mFirePointMatrix at weapon+0x20 (PblMatrix)
      float* fpm = (float*)(wpn + 0x20);
      // Weapon::mFirePos at weapon+0x7C (PblVector3)
      float* wpnFirePos = (float*)(wpn + 0x7C);

      float wpnZoom = *(float*)(wpn + 0xBC);

      // Dump bytes around where mIsAiming might be on the Controllable
      uintptr_t owner = *(uintptr_t*)(wpn + 0x6C);

      fprintf(f, "=== Aimer Dump (char %d, ch %d) ===\n", charIndex, channel);
      fprintf(f, "  Weapon::mZoom (+0xBC): %.6f\n", wpnZoom);
      if (owner) {
         // Dump wider range to find mIsAiming by comparing zoomed vs unzoomed
         for (int row = 0x100; row < 0x300; row += 0x20) {
            fprintf(f, "  owner+0x%03X: ", row);
            for (int i = 0; i < 0x20; ++i)
               fprintf(f, "%02X ", *(unsigned char*)(owner + row + i));
            fprintf(f, "\n");
         }
      }
      fprintf(f, "  Weapon::mFirePointMatrix trans: %.3f, %.3f, %.3f\n",
              fpm[12], fpm[13], fpm[14]);
      fprintf(f, "  Weapon::mFirePos: %.3f, %.3f, %.3f\n",
              wpnFirePos[0], wpnFirePos[1], wpnFirePos[2]);
      fprintf(f, "  mFirePos:    %.3f, %.3f, %.3f\n", firePos[0], firePos[1], firePos[2]);
      fprintf(f, "  mMountPos:   %.3f, %.3f, %.3f\n", mountPos[0], mountPos[1], mountPos[2]);
      fprintf(f, "  mRootPos:    %.3f, %.3f, %.3f\n", rootPos[0], rootPos[1], rootPos[2]);
      fprintf(f, "  mCurrentBarrel: %d\n", barrel);

      for (int b = 0; b < 4; ++b) {
         float* mat = (float*)((char*)aimer + 0xF0 + b * 0x40);
         fprintf(f, "  mBarrelPoseMatrix[%d]:\n", b);
         fprintf(f, "    right:   %.3f, %.3f, %.3f, %.3f\n", mat[0], mat[1], mat[2], mat[3]);
         fprintf(f, "    up:      %.3f, %.3f, %.3f, %.3f\n", mat[4], mat[5], mat[6], mat[7]);
         fprintf(f, "    forward: %.3f, %.3f, %.3f, %.3f\n", mat[8], mat[9], mat[10], mat[11]);
         fprintf(f, "    trans:   %.3f, %.3f, %.3f, %.3f\n", mat[12], mat[13], mat[14], mat[15]);
      }

      // Also dump mMountPoseMatrix (aimer+0xB0)
      float* mpm = (float*)((char*)aimer + 0xB0);
      fprintf(f, "  mMountPoseMatrix:\n");
      fprintf(f, "    right:   %.3f, %.3f, %.3f, %.3f\n", mpm[0], mpm[1], mpm[2], mpm[3]);
      fprintf(f, "    up:      %.3f, %.3f, %.3f, %.3f\n", mpm[4], mpm[5], mpm[6], mpm[7]);
      fprintf(f, "    forward: %.3f, %.3f, %.3f, %.3f\n", mpm[8], mpm[9], mpm[10], mpm[11]);
      fprintf(f, "    trans:   %.3f, %.3f, %.3f, %.3f\n", mpm[12], mpm[13], mpm[14], mpm[15]);

      fprintf(f, "===\n");
      fclose(f);
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return 0;
   }
   return 0;
}

// SetLoadDisplayLevel(path) - overrides the level loaded by LoadDisplay::EnterState.
// The default is "Load\\load" (vanilla load screen level).
// Call from Script root or ScriptPreInit
// Example: SetLoadDisplayLevel("relative path to custom load.lvl")
// E.g. SetLoadDisplayLevel("..\\..\\addon\\995\\data\\_LVL_PC\\Load")
static int lua_SetLoadDisplayLevel(lua_State* L)
{
   const char* path = g_lua.tolstring(L, 1, nullptr);
   if (path)
      strncpy_s(g_loadDisplayPath, sizeof(g_loadDisplayPath), path, _TRUNCATE);
   return 0;
}

struct lua_func_entry {
   const char* name;
   lua_CFunction func;
};

static const lua_func_entry custom_functions[] = {
   { "PrintToLog",            lua_PrintToLog },
   { "GetSystemTickCount",    lua_GetSystemTickCount },
   { "ReadTextFile",          lua_ReadTextFile },
   { "HttpGet",               lua_HttpGet },
   { "HttpPut",               lua_HttpPut },
   { "HttpPost",              lua_HttpPost },
   { "GetCharacterWeapon",    lua_GetCharacterWeapon },
   { "SetCharacterWeapon",    lua_SetCharacterWeapon },
   { "HttpGetAsync",          lua_HttpGetAsync },
   { "HttpPutAsync",          lua_HttpPutAsync },
   { "HttpPostAsync",         lua_HttpPostAsync },
   { "RemoveUnitClass",       lua_RemoveUnitClass },
   { "ReapplyAnimations",     lua_ReapplyAnimations },
   { "OnCharacterExitVehicle",       lua_OnCEV },
   { "OnCharacterExitVehicleName",   lua_OnCEVName },
   { "OnCharacterExitVehicleTeam",   lua_OnCEVTeam },
   { "OnCharacterExitVehicleClass",  lua_OnCEVClass },
   { "ReleaseCharacterExitVehicle",  lua_ReleaseCEV },
   { "SetBarrelFireOrigin",   lua_SetBarrelFireOrigin },
   { "DumpAimerInfo",         lua_DumpAimerInfo },
   { "SetLoadDisplayLevel",   lua_SetLoadDisplayLevel },
   { nullptr, nullptr }
};

void register_lua_functions(lua_State* L)
{
   for (const lua_func_entry* entry = custom_functions; entry->name; ++entry)
      lua_register_func(L, entry->name, entry->func);
}
