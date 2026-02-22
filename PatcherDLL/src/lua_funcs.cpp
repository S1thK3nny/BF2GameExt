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
   { "HttpGetAsync",          lua_HttpGetAsync },
   { "HttpPutAsync",          lua_HttpPutAsync },
   { "HttpPostAsync",         lua_HttpPostAsync },
{ "RemoveUnitClass",       lua_RemoveUnitClass },
   { nullptr, nullptr }
};

void register_lua_functions(lua_State* L)
{
   for (const lua_func_entry* entry = custom_functions; entry->name; ++entry)
      lua_register_func(L, entry->name, entry->func);
}