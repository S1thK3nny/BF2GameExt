#include "pch.h"
#include "lua_funcs.hpp"
#include "lua_hooks.hpp"
#include <wininet.h>
#pragma comment(lib, "wininet.lib")

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

// ---------------------------------------------------------------------------
// GetCharacterWeapon(unit) - returns the ODF class name of the active weapon
// held by a character unit.
// Pass the return value of GetCharacterUnit() directly as the argument.
// GetCharacterUnit() returns a light userdata (Controllable*); we read that
// pointer, call Controllable::GetCurWpn, then read the ODF name string.
// Returns nil if the unit is invalid or has no active weapon.
// Example:
//   local unit = GetCharacterUnit("myHero")
//   local wpn  = GetCharacterWeapon(unit)
// ---------------------------------------------------------------------------

// Controllable::GetCurWpn — __thiscall, ECX = Controllable*, returns Weapon*
typedef void* (__thiscall* GetCurWpn_t)(void* controllable);
static const GetCurWpn_t fn_GetCurWpn = (GetCurWpn_t)0x005e70a0;

static int lua_GetCharacterWeapon(lua_State* L)
{
   // GetCharacterUnit() pushes a light userdata (Controllable*) onto the Lua
   // stack. lua_touserdata returns the raw pointer directly — no float cast.
   void* controllable = g_lua.touserdata(L, 1);
   if (!controllable) { g_lua.pushnil(L); return 1; }

   // Call Controllable::GetCurWpn(__thiscall, ECX = controllable ptr)
   void* weapon = fn_GetCurWpn(controllable);
   if (!weapon) { g_lua.pushnil(L); return 1; }

   // weapon + 0x18 = util::BaseArray<char>; first field (+0x00) is char* data
   const char* odf = *(const char**)((char*)weapon + 0x18);
   if (!odf || odf[0] == '\0') { g_lua.pushnil(L); return 1; }

   g_lua.pushlstring(L, odf, strlen(odf));
   return 1;
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
   { "GetCharacterWeapon",    lua_GetCharacterWeapon },
   { nullptr, nullptr }
};

void register_lua_functions(lua_State* L)
{
   for (const lua_func_entry* entry = custom_functions; entry->name; ++entry)
      lua_register_func(L, entry->name, entry->func);
}