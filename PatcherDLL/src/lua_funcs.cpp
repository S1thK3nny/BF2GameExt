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
// FUN_007e3d50, __cdecl, (const char* fmt, ...)
typedef void (__cdecl* GameLog_t)(const char* fmt, ...);

static int lua_RemoveUnitClass(lua_State* L)
{
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t addr) -> uintptr_t { return addr - 0x400000u + base; };
   const auto fn_GameLog = (GameLog_t)res(0x7E3D50);

   if (!g_lua.isnumber(L, 1)) return 0;
   const char* unitClass = g_lua.tolstring(L, 2, nullptr);
   if (!unitClass) return 0;

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

   // Step 1: Walk g_ClassDefList to find the classDef pointer for unitClass.
   // The head node pointer is stored at res(0xACD2C8).
   // Node layout: +0x04 = next node ptr, +0x0c = classDef ptr (null = end of list).
   // classDef layout: +0x18 = integer name hash (NOT a char* — do not dereference as string).
   //
   // Compute the target hash using the game's own HashString (FUN_007e1bd0).
   // HashString is __thiscall: ECX = 8-byte stack buffer, stack arg = class name string.
   // Returns EAX = ECX (the buffer); first dword of the buffer is the hash.
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
      if (*(int*)((char*)element + 0x18) == targetHash) {
         classDef = element;
         break;
      }
      node = *(uintptr_t*)(node + 0x04);
   }
   if (!classDef) {
      fn_GameLog("RemoveUnitClass(): class \"%s\" not found in global registry (check the side's .req file)\n", unitClass);
      return 0;
   }

   // Step 2: Find classDef in team->classDefArray (parallel array at team+0x50).
   const int classCount = *(int*)((char*)teamPtr + 0x48);
   void** classDefArr = *(void***)((char*)teamPtr + 0x50);
   int foundSlot = -1;
   for (int i = 0; i < classCount; ++i) {
      if (classDefArr[i] == classDef) { foundSlot = i; break; }
   }
   if (foundSlot < 0) {
      fn_GameLog("RemoveUnitClass(): class \"%s\" is not assigned to team %d\n", unitClass, teamIndex);
      return 0;
   }

   // Step 3: Left-shift removal.
   // Slide every entry after foundSlot one position to the left, preserving
   // order. The list is capped at classCapacity entries so this is always cheap.
   int* minArr = *(int**)((char*)teamPtr + 0x54);
   int* maxArr = *(int**)((char*)teamPtr + 0x58);
   const int lastSlot = classCount - 1;
   const auto fn_SetMinMax = (SetUnitClassMinMax_t)res(0x662C20);

   for (int i = foundSlot; i < lastSlot; ++i) {
      classDefArr[i] = classDefArr[i + 1];
      fn_SetMinMax(teamPtr, i, minArr[i + 1], maxArr[i + 1]);
   }

   // Clear the vacated last slot and decrement count.
   classDefArr[lastSlot] = nullptr;
   minArr[lastSlot] = 0;
   maxArr[lastSlot] = 0;
   *(int*)((char*)teamPtr + 0x48) = lastSlot;

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
   { "GetCharacterWeapon",    lua_GetCharacterWeapon },
   { "RemoveUnitClass",       lua_RemoveUnitClass },
   { nullptr, nullptr }
};

void register_lua_functions(lua_State* L)
{
   for (const lua_func_entry* entry = custom_functions; entry->name; ++entry)
      lua_register_func(L, entry->name, entry->func);
}