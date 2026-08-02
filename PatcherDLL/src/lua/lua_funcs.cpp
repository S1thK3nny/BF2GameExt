#include "pch.h"
#include "lua_funcs.hpp"
#include "lua_hooks.hpp"
#include "core/resolve.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/lvl_read.hpp"
#include "entity/flyer_carrier_fixes.hpp"
#include <detours.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")

// ReadTextFile(path) - reads any file on disk, returns its contents as a string.
// Accepts absolute or relative paths. Returns nil if the file cannot be opened.
// Example: local cfg = ReadTextFile("C:\\Users\\me\\Desktop\\config.txt")
//
// WARNING: This allows reading arbitrary files from the filesystem with no
// sandboxing or path restrictions. Disabled until a safe path policy is in place.
#if 0
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
#endif

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
   auto res = [=](uintptr_t a) -> uintptr_t { return a - kUnrelocatedBase + base; };

   if (!g_addr->char_array_base || !g_addr->max_chars) { g_lua.pushnil(L); return 1; }
   if (!g_lua.isnumber(L, 1)) { g_lua.pushnil(L); return 1; }

   const int charIndex = g_lua.tointeger(L, 1);
   const int maxChars  = *(int*)res(g_addr->max_chars);
   if (charIndex < 0 || charIndex >= maxChars) { g_lua.pushnil(L); return 1; }

   const uintptr_t arrayBase = *(uintptr_t*)res(g_addr->char_array_base);
   if (!arrayBase) { g_lua.pushnil(L); return 1; }

   const int channel = (g_lua.gettop(L) >= 2 && g_lua.isnumber(L, 2))
                       ? g_lua.tointeger(L, 2) : 0;
   if (channel < 0 || channel > 7) { g_lua.pushnil(L); return 1; }

   __try {
      char* charSlot = (char*)arrayBase + charIndex * 0x1B0;
      // The pointer at slot+0x148 is the soldier's Controllable sub-object
      // (struct+0x240) — the "entity" view all g_soldier offsets are based on.
      // (The historical "+0x18 ctrl view" read the same fields at aliased
      // offsets; direct entity offsets are what the per-build layout stores.)
      uintptr_t entity = *(uintptr_t*)(charSlot + 0x148);
      if (!entity) { g_lua.pushnil(L); return 1; }

      // Read the slot index for the requested channel (mWeaponIndex[channel])
      uint8_t slotIdx = 0;
      __try { slotIdx = *(uint8_t*)(entity + g_soldier->weaponIndexMap + channel); }
      __except (EXCEPTION_EXECUTE_HANDLER) { g_lua.pushnil(L); return 1; }

      if (slotIdx >= 8) { g_lua.pushnil(L); return 1; }

      // Read weapon pointer from mWeapon[slotIdx]
      uintptr_t wpn = 0;
      __try { wpn = *(uintptr_t*)(entity + g_soldier->weaponArray + slotIdx * 4); }
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
// @return #bool              1 on success, nil on failure.
//
// v6 (2026-07-18) — engine-native rebuild. See docs/CharacterWeaponSystem.md
// "SetCharacterWeapon v6" for the full RE trail (Phantom-build PDB).
//
// TODO:
// - Melee-family swaps are untested and likely unsafe even with the animmap
//   guard: WeaponMeleeThrow caches m_pPrimaryWeapon (its WeaponMelee partner)
//   once at build time and derefs it unconditionally, so swapping a hero's
//   saber out dangles the throw weapon's back-pointer. Refuse melee swaps if
//   this ever matters.
//
// Do exactly what EntitySoldier's constructor does for one slot
// (modtools ctor 0x5339d0, weapon-build loop at 0x533e20):
//   1. Resolve entity (EntitySoldier's Controllable sub-object, struct+0x240).
//   2. Find the target WeaponClass in the Factory list (unchanged walk).
//   3. Fill a stack WeaponDesc {owner, aimer, trigger, reload, numClips, 0, 0}
//      with the same pointers the ctor passes.
//   4. newWpn = foundWc->vtbl[+0x8](&desc)  — WeaponClass::Build. The Weapon
//      ctor allocates its own AmmoCounter/EnergyBar, binds the soldier's
//      aimer to itself, and resolves its own animation MAP from the owner's
//      animation bank (Weapon.cpp:0x60 "Weapon failed to find animmap %s_%s").
//   5. Swap the new pointer into the soldier's mWeapon slot (entity+0x4F0[slot],
//      the same memory GetCharacterWeapon reads) and the slot-0 cache.
//   6. Re-run the ctor's aimer fixup: Aimer::SetWeapon(mWeapon[weaponIndex[0]]).
//   7. Destroy the old weapon via its own virtual deleting dtor (vtbl[0](1)) —
//      identical to ~EntitySoldier's teardown. Frees back to Weapon pool.
// Net pool delta: +1 Build, -1 destroy = zero. No vtable hacks, no donor
// weapon scan, no MAP bookkeeping.
//
// Guardrails:
//   - Refused in multiplayer (weapon rebuild is not replicated).
//   - Refused for slots using WeaponShareAmmo / WeaponShareEnergy (the desc
//     would need the shared refcounted counter wired through; not done yet).
//   - Refused when the unit's animation bank has no animmap for the new
//     weapon (Weapon ctor leaves MAP=-1 → anim system crash); the freshly
//     built weapon is destroyed again and the old one stays in the slot.
//   - Only works with ODFs already loaded in memory (in a .lvl the level read).
static int lua_SetCharacterWeapon(lua_State* L)
{
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - kUnrelocatedBase + base; };

   if (!g_addr->char_array_base || !g_addr->max_chars || !g_addr->game_log ||
       !g_addr->net_in_shell || !g_addr->aimer_set_weapon) {
      g_lua.pushnil(L); return 1;
   }
   const auto fn_GameLog = (GameLog_t)res(g_addr->game_log);
   const SoldierLayout& lay = *g_soldier;

   if (!g_lua.isnumber(L, 1)) { g_lua.pushnil(L); return 1; }

   const int   charIndex = g_lua.tointeger(L, 1);
   const char* targetOdf = g_lua.tolstring(L, 2, nullptr);
   if (!targetOdf || targetOdf[0] == '\0') { g_lua.pushnil(L); return 1; }

   // Only channels 0/1 exist: the engine's mWeaponIndex map (struct+0x750) and
   // the trigger array the WeaponDesc points into (struct+0x278) are 2 entries.
   const int channel = (g_lua.gettop(L) >= 3 && g_lua.isnumber(L, 3))
                       ? g_lua.tointeger(L, 3) : 0;
   if (channel < 0 || channel > 1) { g_lua.pushnil(L); return 1; }

   // Multiplayer guard — the rebuild is purely local and would desync peers.
   // Exact game idiom (EntitySoldier ctor @0x534001): netEnabledNext only counts
   // while in the shell; in-game the authority is netEnabled. A plain OR of both
   // flags wrongly blocked SP missions when netEnabledNext lingered from the shell.
   {
      const uint8_t inShell = *(uint8_t*)res(g_addr->net_in_shell);
      const uint8_t net = inShell ? *(uint8_t*)res(g_addr->net_enabled_next)
                                  : *(uint8_t*)res(g_addr->net_enabled);
      if (net) {
         fn_GameLog("SetCharacterWeapon: disabled in multiplayer (inShell=%d).\n", (int)inShell);
         g_lua.pushnil(L);
         return 1;
      }
   }

   const int maxChars = *(int*)res(g_addr->max_chars);
   if (charIndex < 0 || charIndex >= maxChars) {
      fn_GameLog("SetCharacterWeapon: charIndex %d out of range (max %d).\n", charIndex, maxChars);
      g_lua.pushnil(L); return 1;
   }

   const uintptr_t arrayBase = *(uintptr_t*)res(g_addr->char_array_base);
   if (!arrayBase) {
      fn_GameLog("SetCharacterWeapon: character array not initialised.\n");
      g_lua.pushnil(L); return 1;
   }

   __try {
      char* charSlot     = (char*)arrayBase + charIndex * 0x1B0;
      char* intermediate = *(char**)(charSlot + 0x148);
      if (!intermediate) {
         fn_GameLog("SetCharacterWeapon: char %d has no unit (intermediate null).\n", charIndex);
         g_lua.pushnil(L); return 1;
      }

      // The pointer stored at charSlot+0x148 IS the soldier's Controllable
      // sub-object (struct+0x240) — the exact pointer the engine passes as
      // WeaponDesc::mOwner. Proof: GetCharacterWeapon's proven reads at
      // (intermediate+0x18)+0x4D8 / +0x4F8 land on struct+0x730 (mWeapon) and
      // struct+0x750 (mWeaponIndex) from the ctor disasm — the "+0x18 ctrl
      // view" was an accidental offset onto the same object. The old
      // *(ctrl+0x290) "entity" pointer was NOT the soldier (its +0x4F0 reads
      // NULL for the player) and is no longer used.
      const uintptr_t entity = (uintptr_t)intermediate;
      if (entity == 0xCDCDCDCDu) {
         fn_GameLog("SetCharacterWeapon: char %d controllable ptr invalid.\n", charIndex);
         g_lua.pushnil(L); return 1;
      }

      // Channel → slot index. mWeaponIndex[2] (modtools struct+0x750 / Steam
      // +0x740, filled by the ctor's channel-mapping loop; 0xFF = empty).
      uint8_t slotIdx = 0xFF;
      __try { slotIdx = *(uint8_t*)(entity + lay.weaponIndexMap + channel); }
      __except (EXCEPTION_EXECUTE_HANDLER) { g_lua.pushnil(L); return 1; }
      if (slotIdx >= 8) {
         fn_GameLog("SetCharacterWeapon: char %d channel %d has no weapon slot (idx=%d).\n",
                    charIndex, channel, (int)slotIdx);
         g_lua.pushnil(L); return 1;
      }

      // Old weapon from the ENTITY-side mWeapon[] array — the array the ctor
      // fills with Build results and ~EntitySoldier frees.
      uintptr_t oldWpn = 0;
      __try { oldWpn = *(uintptr_t*)(entity + lay.weaponArray + slotIdx * 4); }
      __except (EXCEPTION_EXECUTE_HANDLER) { g_lua.pushnil(L); return 1; }
      if (!oldWpn || oldWpn == 0xCDCDCDCDu) {
         fn_GameLog("SetCharacterWeapon: char %d slot %d entity-side weapon invalid (0x%08x).\n",
                    charIndex, (int)slotIdx, (unsigned)oldWpn);
         g_lua.pushnil(L); return 1;
      }

      uintptr_t startWc = 0;
      __try { startWc = *(uintptr_t*)(oldWpn + 0x060); }
      __except (EXCEPTION_EXECUTE_HANDLER) { g_lua.pushnil(L); return 1; }
      if (!startWc || startWc == 0xCDCDCDCDu) {
         fn_GameLog("SetCharacterWeapon: char %d slot %d WeaponClass ptr invalid.\n",
                    charIndex, (int)slotIdx);
         g_lua.pushnil(L); return 1;
      }

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

      // Already holding the requested class — nothing to do. (Note: name matching
      // accepts suffixes, so this also triggers if the argument suffix-matches
      // the weapon already held.)
      if (foundWc == startWc) {
         fn_GameLog("SetCharacterWeapon: char %d already holds '%s' - no-op.\n", charIndex, targetOdf);
         g_lua.pushnumber(L, 1); return 1;
      }

      // EntitySoldierClass — per-slot loadout config (offsets from the ctor's
      // weapon-build loop; modtools @0x533e20, Steam @0x4dedc0 — see
      // entity_layout.hpp).
      uintptr_t esClass = 0;
      __try { esClass = *(uintptr_t*)(entity + lay.classPtr); } __except(EXCEPTION_EXECUTE_HANDLER) {}
      if (!esClass || esClass == 0xCDCDCDCDu) {
         fn_GameLog("SetCharacterWeapon: char %d EntitySoldierClass ptr invalid.\n", charIndex);
         g_lua.pushnil(L); return 1;
      }

      int8_t weaponCount = 0;
      __try { weaponCount = *(int8_t*)(esClass + lay.clsWeaponCount); } __except(EXCEPTION_EXECUTE_HANDLER) {}
      if ((int)slotIdx >= (int)weaponCount) {
         fn_GameLog("SetCharacterWeapon: slot %d >= class weapon count %d.\n",
                    (int)slotIdx, (int)weaponCount);
         g_lua.pushnil(L); return 1;
      }

      // WeaponAmmo config for this slot:
      //   negative       → WeaponShareAmmo with another slot   → refuse
      //   bit 0x40000000 → WeaponShareEnergy with another slot → refuse
      //   bit 0x20000000 → infinite ammo → mNumClips = INT_MAX (ctor @0x533eb6)
      int32_t ammoVal = 0;
      __try { ammoVal = *(int32_t*)(esClass + lay.clsWeaponAmmo + slotIdx * 4); } __except(EXCEPTION_EXECUTE_HANDLER) {}
      if (ammoVal < 0 || (ammoVal & 0x40000000)) {
         fn_GameLog("SetCharacterWeapon: slot %d uses WeaponShareAmmo/Energy - not supported.\n",
                    (int)slotIdx);
         g_lua.pushnil(L);
         return 1;
      }
      const int32_t numClips = (ammoVal & 0x20000000) ? 0x7FFFFFFF : (ammoVal & 0xFFFFFF);

      int8_t chSlot = (int8_t)channel;
      __try { chSlot = *(int8_t*)(esClass + lay.clsWeaponChannel + slotIdx); } __except(EXCEPTION_EXECUTE_HANDLER) {}
      if (chSlot < 0 || chSlot > 1) chSlot = (int8_t)channel;

      // WeaponDesc — identical to the stack desc the soldier ctor builds
      // (@0x533e30..0x533ee6). All pointers reference the live soldier, so the
      // new Weapon reads the same triggers/aimer the old one did.
      struct WeaponDescRaw {
         uintptr_t owner;             // Controllable sub-object (== entity)
         uintptr_t aimer;             // entity+0x2D0 (struct+0x510)
         uintptr_t trigger;           // entity+0x38+ch*4 (struct+0x278)
         uintptr_t reload;            // entity+0x40 (struct+0x280)
         int32_t   numClips;
         uintptr_t sharedAmmoCounter; // unused (shared slots refused above)
         uintptr_t sharedEnergyBar;   // unused
      } desc = {};

      desc.owner    = entity;
      desc.aimer    = entity + lay.aimer;
      desc.numClips = numClips;

      uint8_t dualFlag = 0;
      __try { dualFlag = *(uint8_t*)(entity + lay.dualWieldFlag); } __except(EXCEPTION_EXECUTE_HANDLER) {}
      if ((dualFlag & 1) && chSlot == 1) {
         // Dual-wield secondary: fire trigger is the reload trigger, no reload.
         desc.trigger = entity + 0x40;
         desc.reload  = 0;
      } else {
         desc.trigger = entity + 0x38 + (uintptr_t)chSlot * 4;
         desc.reload  = entity + 0x40;
      }

      // Build the replacement — WeaponClass::Build, vtable slot +0x8,
      // __thiscall(WeaponDesc*), returns Weapon* (NULL if the pool is full).
      // The Weapon ctor allocates its own AmmoCounter/EnergyBar, binds the
      // soldier's aimer to itself, and resolves its animation MAP from the
      // owner's animation bank — no manual MAP fixing needed.
      uintptr_t newWpn = 0;
      __try {
         typedef uintptr_t (__thiscall* WcBuild_t)(uintptr_t wc, WeaponDescRaw* d);
         WcBuild_t fnBuild = *(WcBuild_t*)(*(uintptr_t*)foundWc + 0x8);
         newWpn = fnBuild(foundWc, &desc);
      } __except(EXCEPTION_EXECUTE_HANDLER) { newWpn = 0; }
      if (!newWpn) {
         fn_GameLog("SetCharacterWeapon: WeaponClass::Build failed for '%s' (Weapon pool full?).\n",
                    targetOdf);
         g_lua.pushnil(L);
         return 1;
      }

      // Animmap guard. The Weapon ctor resolves its animation MAP from the
      // owner's animation bank; when the bank has no map for this weapon it
      // warns ("Weapon failed to find animmap %s_%s", Weapon.cpp:96) and
      // leaves weapon+0xC8 = -1, which the animation system later crashes on.
      // Roll back: destroy the new weapon and rebind the aimer (the Weapon
      // ctor pointed it at newWpn; ~Weapon does not touch it, so destroy-then-
      // rebind is safe). The entity slots are still untouched here, so the
      // unit simply keeps its old weapon.
      int32_t newMap = -1;
      __try { newMap = *(int32_t*)(newWpn + 0x0C8); } __except(EXCEPTION_EXECUTE_HANDLER) {}
      if (newMap == -1) {
         __try {
            typedef void (__thiscall* WpnDelete_t)(uintptr_t w, uint32_t flags);
            ((WpnDelete_t)(**(uintptr_t**)newWpn))(newWpn, 1);
         } __except(EXCEPTION_EXECUTE_HANDLER) {}
         __try {
            int8_t aimSlot = *(int8_t*)(entity + lay.weaponIndexMap);
            if (aimSlot >= 0 && aimSlot < 8) {
               uintptr_t aimWpn = *(uintptr_t*)(entity + lay.weaponArray + aimSlot * 4);
               if (aimWpn && aimWpn != 0xCDCDCDCDu) {
                  typedef void (__thiscall* AimerSetWeapon_t)(uintptr_t aimer, uintptr_t w);
                  ((AimerSetWeapon_t)res(g_addr->aimer_set_weapon))(entity + lay.aimer, aimWpn);
               }
            }
         } __except(EXCEPTION_EXECUTE_HANDLER) {}
         fn_GameLog("SetCharacterWeapon: '%s' has no animmap in char %d's animation bank - swap refused.\n",
                    targetOdf, charIndex);
         g_lua.pushnil(L);
         return 1;
      }

      // Swap into the mWeapon slot the engine owns. This is the same memory
      // GetCharacterWeapon reads, so Get reflects the swap immediately — there
      // is no second array.
      __try { *(uintptr_t*)(entity + lay.weaponArray + slotIdx * 4) = newWpn; } __except(EXCEPTION_EXECUTE_HANDLER) {}

      // Retarget the cached slot-0 weapon pointer (modtools struct+0x718, the
      // old "ctrl+0x4C0 always equals slot[0]" observation) if it held the old
      // one.  Compare-guarded, so a stale offset guess is a no-op.
      __try {
         uintptr_t* cache = (uintptr_t*)(entity + lay.slot0Cache);
         if (*cache == oldWpn) *cache = newWpn;
      } __except(EXCEPTION_EXECUTE_HANDLER) {}

      // The Weapon ctor unconditionally bound the aimer to the NEW weapon.
      // Re-run the soldier ctor's post-loop fixup (modtools @0x533fee, Steam
      // @0x4defa2): point the aimer back at the primary-channel active weapon
      // (which is newWpn itself when that is the slot we just swapped).
      __try {
         int8_t aimSlot = *(int8_t*)(entity + lay.weaponIndexMap);
         if (aimSlot >= 0 && aimSlot < 8) {
            uintptr_t aimWpn = *(uintptr_t*)(entity + lay.weaponArray + aimSlot * 4);
            if (aimWpn && aimWpn != 0xCDCDCDCDu) {
               typedef void (__thiscall* AimerSetWeapon_t)(uintptr_t aimer, uintptr_t w);
               ((AimerSetWeapon_t)res(g_addr->aimer_set_weapon))(entity + lay.aimer, aimWpn);
            }
         }
      } __except(EXCEPTION_EXECUTE_HANDLER) {}

      // The Weapon ctor leaves the "currently drawn" flag (weapon+0xAC bit 2)
      // CLEAR — Weapon::Update's IDLE state refuses to even read the fire
      // trigger until Weapon::Select sets it (Select = vtable+0x74; body sets
      // field_0xAC = (field_0xAC & 0xFFFFFE07) | 4; slot verified identical in
      // modtools @0x61ba80 and Phantom @0x7af080). The game only calls Select
      // on a weapon switch, so without this the swapped-in weapon can't fire
      // until the player cycles to the other channel and back. Call it now if
      // the swapped slot is the one currently drawn (entity+0x512 low nibble =
      // active slot index, the byte UpdateIndirect reads). Second arg true =
      // skip the weapon-change sound.
      __try {
         uint8_t active = *(uint8_t*)(entity + lay.weaponIndex) & 0x0F;
         if (active == (uint8_t)slotIdx) {
            typedef void (__thiscall* WpnSelect_t)(uintptr_t w, uint32_t chan, uint32_t quiet);
            WpnSelect_t fnSelect = *(WpnSelect_t*)(*(uintptr_t*)newWpn + 0x74);
            fnSelect(newWpn, (uint32_t)channel, 1u);
         }
      } __except(EXCEPTION_EXECUTE_HANDLER) {}

      // Destroy the old weapon LAST, after every reference is rebound.
      // Virtual deleting dtor (vtbl[0](1)) — the same call ~EntitySoldier makes.
      // Frees the item back to the Weapon pool; ~Weapon stops its sounds and
      // releases its refcounted AmmoCounter/EnergyBar. Net pool delta: zero.
      __try {
         typedef void (__thiscall* WpnDelete_t)(uintptr_t w, uint32_t flags);
         ((WpnDelete_t)(**(uintptr_t**)oldWpn))(oldWpn, 1);
      } __except(EXCEPTION_EXECUTE_HANDLER) {}

      // First-person view-model cache. FirstPersonRenderable keeps the active
      // Weapon* at +0x1600 (mCurrentWeapon) and its holster transition keeps
      // rendering it via vtbl+0x8C after a weapon change — with the old
      // weapon destroyed that call lands in freed pool memory (RenderSoldier
      // crash at 0x4aa59c, EIP=0). Clearing it is engine-safe: RenderSoldier
      // null-checks the field, and UpdateSoldier's holster path requires it
      // to be non-null, so the swapped-in weapon is adopted immediately.
      __try {
         if (g_addr->fp_renderable) {
            // mCurrentWeapon +0x1600 is build-invariant (Phantom PDB struct).
            uintptr_t fpr = *(uintptr_t*)res(g_addr->fp_renderable);
            if (fpr && fpr != 0xCDCDCDCDu && *(uintptr_t*)(fpr + 0x1600) == oldWpn)
               *(uintptr_t*)(fpr + 0x1600) = 0;
         }
      } __except(EXCEPTION_EXECUTE_HANDLER) {}

      // Instant animation switch. The Weapon ctor already wrote the correct MAP
      // into newWpn+0xC8 (resolved from the owner's animation bank), and
      // EntitySoldier::UpdateIndirect (0x0053b920) re-asserts it every frame —
      // both sides now agree, so no oscillation is possible. Calling SWAM here
      // just makes the stance change this frame instead of next.
      __try {
         uintptr_t animator = *(uintptr_t*)(entity + lay.animator);
         int32_t   newMap   = *(int32_t*)(newWpn + 0x0C8);
         if (animator && animator != 0xCDCDCDCDu && newMap != -1 && g_addr->set_weapon_anim_map) {
            typedef void (__thiscall* SetWeaponAnimMap_t)(void*, int32_t);
            ((SetWeaponAnimMap_t)res(g_addr->set_weapon_anim_map))((void*)animator, newMap);
         }
      } __except(EXCEPTION_EXECUTE_HANDLER) {}

      fn_GameLog("SetCharacterWeapon: char %d ch %d slot[%d] rebuilt -> '%s' (wpn=0x%08x map=%d)\n",
                 charIndex, (int)chSlot, (int)slotIdx, targetOdf,
                 (unsigned)newWpn, *(int32_t*)(newWpn + 0x0C8));

      g_lua.pushnumber(L, 1);
      return 1;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      g_lua.pushnil(L);
      return 1;
   }
}


// ---------------------------------------------------------------------------
// Resolve (charIndex, channel) → active Weapon* for that channel.
// Returns nullptr on any failure. Shared by GetWeaponAmmo / SetWeaponAmmo.
//
// Confirmed chain (see docs/CharacterWeaponSystem.md):
//   charArray + idx*0x1B0     → charSlot
//   *(charSlot + 0x148)       → intermediate
//   intermediate + 0x18       → Controllable*
//   *(ctrl + 0x4F8 + channel) → uint8 slotIdx
//   *(ctrl + 0x4D8 + slotIdx*4) → Weapon*
// ---------------------------------------------------------------------------
static uintptr_t resolve_active_weapon(int charIndex, int channel)
{
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - kUnrelocatedBase + base; };

   if (!g_addr->char_array_base || !g_addr->max_chars) return 0;

   const int maxChars = *(int*)res(g_addr->max_chars);
   if (charIndex < 0 || charIndex >= maxChars) return 0;
   if (channel < 0 || channel > 7) return 0;

   const uintptr_t arrayBase = *(uintptr_t*)res(g_addr->char_array_base);
   if (!arrayBase) return 0;

   __try {
      char* charSlot = (char*)arrayBase + charIndex * 0x1B0;
      uintptr_t entity = *(uintptr_t*)(charSlot + 0x148);
      if (!entity) return 0;

      uint8_t slotIdx = *(uint8_t*)(entity + g_soldier->weaponIndexMap + channel);
      if (slotIdx >= 8) return 0;

      uintptr_t wpn = *(uintptr_t*)(entity + g_soldier->weaponArray + slotIdx * 4);
      if (!wpn || wpn == 0xCDCDCDCDu) return 0;
      return wpn;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ---------------------------------------------------------------------------
// GetWeaponAmmo(charIndex [, channel]) - returns ammo state for the active
// weapon in a given channel.
//
// All ammo is tracked in **clips**, not rounds. A weapon with a 50-round clip
// reading (1, 3, 4, 50) means: 1 clip currently loaded (= 50 rounds), 3 spare
// clips, max 4 clips, 50 rounds per clip → 200 rounds total.
//
// mCurClip is a float — it represents fractional clip remaining (1.0 = full,
// decreases as rounds are fired; total rounds = mCurClip * mRoundsPerClip).
//
// AmmoCounter layout (24 bytes, allocated at Weapon+0x88):
//   +0   AmmoCounterClass*  m_pClass         (→ +0 int mRoundsPerClip, +4 float mAmmoPerRound)
//   +4   float              mDefaultMaxClips
//   +8   float              mMaxClips        (current cap, can differ from default)
//   +12  float              mNumClips        (reserve clips remaining)
//   +16  float              mCurClip         (clips currently loaded — fractional)
//   +20  uint               m_uiRefCount
//
// @param #int charIndex
// @param #int channel    (default 0)
// @return curClip, numClips, maxClips, roundsPerClip  — four numbers, or nil
// ---------------------------------------------------------------------------
static int lua_GetWeaponAmmo(lua_State* L)
{
   if (!g_lua.isnumber(L, 1)) { g_lua.pushnil(L); return 1; }
   const int charIndex = g_lua.tointeger(L, 1);
   const int channel = (g_lua.gettop(L) >= 2 && g_lua.isnumber(L, 2))
                       ? g_lua.tointeger(L, 2) : 0;

   uintptr_t wpn = resolve_active_weapon(charIndex, channel);
   if (!wpn) { g_lua.pushnil(L); return 1; }

   __try {
      uintptr_t ac = *(uintptr_t*)(wpn + 0x88);
      if (!ac || ac == 0xCDCDCDCDu) { g_lua.pushnil(L); return 1; }

      float curClip  = *(float*)(ac + 0x10);
      float numClips = *(float*)(ac + 0x0C);
      float maxClips = *(float*)(ac + 0x08);

      int roundsPerClip = 0;
      uintptr_t acClass = *(uintptr_t*)(ac + 0x00);
      if (acClass && acClass != 0xCDCDCDCDu)
         roundsPerClip = *(int*)(acClass + 0x00);

      g_lua.pushnumber(L, curClip);
      g_lua.pushnumber(L, numClips);
      g_lua.pushnumber(L, maxClips);
      g_lua.pushnumber(L, roundsPerClip);
      return 4;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      g_lua.pushnil(L);
      return 1;
   }
}

// ---------------------------------------------------------------------------
// SetWeaponAmmo(charIndex, curClip [, numClips [, channel]]) - writes ammo state.
//
// Values are in **clips**, not rounds. curClip is fractional (1.0 = full clip
// loaded, 0.5 = half a clip). numClips is the integer count of spare clips.
// Pass nil for numClips to leave it untouched. Channel defaults to 0.
//
// @return 1 on success, nil on failure
// ---------------------------------------------------------------------------
static int lua_SetWeaponAmmo(lua_State* L)
{
   if (!g_lua.isnumber(L, 1) || !g_lua.isnumber(L, 2)) { g_lua.pushnil(L); return 1; }
   const int   charIndex = g_lua.tointeger(L, 1);
   const float curClip   = (float)g_lua.tonumber(L, 2);

   const bool  haveNumClips = (g_lua.gettop(L) >= 3 && g_lua.isnumber(L, 3));
   const float numClips     = haveNumClips ? (float)g_lua.tonumber(L, 3) : 0.0f;

   const int channel = (g_lua.gettop(L) >= 4 && g_lua.isnumber(L, 4))
                       ? g_lua.tointeger(L, 4) : 0;

   uintptr_t wpn = resolve_active_weapon(charIndex, channel);
   if (!wpn) { g_lua.pushnil(L); return 1; }

   __try {
      uintptr_t ac = *(uintptr_t*)(wpn + 0x88);
      if (!ac || ac == 0xCDCDCDCDu) { g_lua.pushnil(L); return 1; }

      *(float*)(ac + 0x10) = curClip;
      if (haveNumClips) *(float*)(ac + 0x0C) = numClips;

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
   auto res = [=](uintptr_t addr) -> uintptr_t { return addr - kUnrelocatedBase + base; };

   if (!g_addr->team_array_base || !g_addr->game_log ||
       !g_addr->class_def_list || !g_addr->hash_string_thiscall)
      return 0;
   const auto fn_GameLog = (GameLog_t)res(g_addr->game_log);

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
   const uintptr_t teamArrayBase = *(uintptr_t*)res(g_addr->team_array_base);
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
      const auto fn_HashString = (HashString_t)res(g_addr->hash_string_thiscall);
      alignas(4) int hashBuf[2] = {};
      fn_HashString(hashBuf, unitClass);
      const int targetHash = hashBuf[0];

      uintptr_t node = *(uintptr_t*)res(g_addr->class_def_list);
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
   auto res = [=](uintptr_t a) -> uintptr_t { return a - kUnrelocatedBase + base; };
   if (!g_addr->hash_string_thiscall) { g_lua.pushnil(L); return 1; }
   typedef void* (__thiscall* HashString_t)(void* buf, const char* s);
   const auto fn_Hash = (HashString_t)res(g_addr->hash_string_thiscall);
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
   auto res = [=](uintptr_t a) -> uintptr_t { return a - kUnrelocatedBase + base; };
   if (!g_addr->hash_string_thiscall || !g_addr->game_log || !g_addr->class_def_list) {
      g_lua.pushnil(L); return 1;
   }
   typedef void* (__thiscall* HashString_t)(void* buf, const char* s);
   const auto fn_Hash = (HashString_t)res(g_addr->hash_string_thiscall);
   const auto fn_GameLog = (GameLog_t)res(g_addr->game_log);

   // Hash the class name and walk the EntityClass global registry to resolve it
   // to a live EntityClass pointer. Registration fails if the class isn't loaded.
   alignas(4) int hashBuf[2] = {};
   fn_Hash(hashBuf, cls);
   const uint32_t targetHash = (uint32_t)hashBuf[0];

   void* classPtr = nullptr;
   uintptr_t node = *(uintptr_t*)res(g_addr->class_def_list);
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

// SetLoadDisplayLevel(path) - overrides the lvl the loading screen loads.
// The default is "Load\\load" (the vanilla load screen level).
// Call from Script root or ScriptPreInit.
//
// Paths resolve the same way ReadDataFile's do, minus the sublevel suffix:
//
//   SetLoadDisplayLevel("LOAD\\load.lvl")
//       -> data\_lvl_pc\LOAD\load.lvl
//   SetLoadDisplayLevel("dc:LOAD\\load.lvl")
//       -> <addon dir>\Data\_lvl_pc\LOAD\load.lvl  (e.g. addon\VTR\Data\_lvl_pc\...)
//   SetLoadDisplayLevel("..\\..\\addon\\VTR\\data\\_LVL_PC\\LOAD\\load")
//       -> raw path relative to data\_lvl_pc\, the original form, still supported
//
// The trailing ".lvl" is optional: LoadDisplay::LoadDataFile appends it via
// LoadUtil::MakeFullName, so it is stripped here and the engine puts it back.
//
// The resolution itself lives in lvl_resolve_data_path (core/lvl_read.hpp),
// shared with the LoadSoundLVL LoadConfig key so both accept the same forms.
//
// If the resolved file does not exist the call is rejected with a severity-3
// RedWarning and the previous load level is kept, so the modder gets a log line
// naming the path instead of a loading screen that silently renders nothing.
static int lua_SetLoadDisplayLevel(lua_State* L)
{
   const char* path = g_lua.tolstring(L, 1, nullptr);
   if (!path || !*path) return 0;

   char stem[260], reason[512];
   if (lvl_resolve_data_path(path, stem, sizeof(stem), nullptr, 0,
                             reason, sizeof(reason)) != LvlPathStatus::Ok) {
      warn_gamelog(RED_SEVERITY_ERROR, SRC_FILE, __LINE__,
         "SetLoadDisplayLevel(\"%s\"): %s Keeping \"%s\".\n",
         path, reason, g_loadDisplayPath);
      return 0;
   }

   strncpy_s(g_loadDisplayPath, sizeof(g_loadDisplayPath), stem, _TRUNCATE);
   return 0;
}

// SetFogRange(near, far) - sets the fog start/end distances.
// Both the D3D render state and the engine's internal copy are updated.
// Example: SetFogRange(0, 5) -- fog starts at 0m, fully opaque at 5m
static int lua_SetFogRange(lua_State* L)
{
   float fogNear = (float)g_lua.tonumber(L, 1);
   float fogFar  = (float)g_lua.tonumber(L, 2);

   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - kUnrelocatedBase + base; };

   if (!g_addr->red_renderer_set_fog_range) return 0;

   // RedRenderer::SetFogRange(float, float) — cdecl, sets D3DRS_FOGSTART/FOGEND
   typedef void(__cdecl* SetFogRange_t)(float, float);
   ((SetFogRange_t)res(g_addr->red_renderer_set_fog_range))(fogNear, fogFar);

   // FLRenderer fog globals — persistence across RenderFarScene's save/restore.
   // (FLRenderer::SetFogRange only stores these; release builds inlined it.)
   if (g_addr->fl_fog_start && g_addr->fl_fog_end) {
      *(float*)res(g_addr->fl_fog_start) = fogNear;
      *(float*)res(g_addr->fl_fog_end)   = fogFar;
   }

   return 0;
}

// SetFogEnable(enable) - enables or disables fog rendering.
// Example: SetFogEnable(1) -- enable fog, SetFogEnable(0) -- disable
static int lua_SetFogEnable(lua_State* L)
{
   bool enable = g_lua.tonumber(L, 1) != 0;

   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - kUnrelocatedBase + base; };

   if (!g_addr->red_renderer_set_fog_enable) return 0;

   // RedRenderer::SetFogEnable(bool) — cdecl, sets D3DRS_FOGENABLE
   typedef void(__cdecl* SetFogEnable_t)(bool);
   ((SetFogEnable_t)res(g_addr->red_renderer_set_fog_enable))(enable);

   return 0;
}

// Replicates the post-create steps that VehicleSpawn::UpdateSpawn (0x00665a50)
// performs after EntityClass::Create. Stock `CreateEntity` Lua callback skips
// these, so vehicles spawned via it have no team / damage owner and weapons
// silently no-op.
//
//   1. ctrl = entity->vtable[9]()        — get controllable
//   2. ctrl->vtable[36](team)            — Controllable::SetTeam (+0x234 low 4 bits)
//   3. patch +0x234 bits 4-7  = team     — spawner-team
//   4. patch +0x234 bits 8-11 = team     — group/owner bits
//   5. ctrl->vtable[5]()                 — activate
//
// Returns true on success.
static bool apply_vehicle_fixup(void* entity, int team)
{
   if (!entity) return false;

   typedef void* (__fastcall* GetCtrl_t)(void* ecx);
   typedef void  (__fastcall* SetTeam_t)(void* ecx, void* edx, int team);
   typedef void  (__fastcall* Activate_t)(void* ecx);

   __try {
      void** evt = *(void***)entity;
      auto getCtrl = (GetCtrl_t)evt[9];
      void* ctrl = getCtrl(entity);
      if (!ctrl) return false;

      void** cvt = *(void***)ctrl;
      auto setTeam  = (SetTeam_t)cvt[36];
      auto activate = (Activate_t)cvt[5];

      setTeam(ctrl, nullptr, team);

      uint32_t* pField = (uint32_t*)((char*)ctrl + 0x234);
      uint32_t v = *pField;
      v = (v & ~0x0F0u) | ((uint32_t)(team & 0xF) << 4);
      v = (v & ~0xF00u) | ((uint32_t)(team & 0xF) << 8);
      *pField = v;

      activate(ctrl);
      return true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

// Detour for stock `Lua_Callbacks::CreateEntity` (0x00472730).
//
// Original Lua signature: CreateEntity(className, matrix, name) -> entity|nil
// Extended signature:     CreateEntity(className, matrix, name [, team]) -> entity|nil
//
// After the original call returns (top of Lua stack = entity lightuserdata or
// nil), runs the vehicle fixup so weapons work. Team defaults to 0 if omitted.
// Non-vehicle entities are tolerated — apply_vehicle_fixup is SEH-guarded and
// for entities without the expected vtable layout it'll either no-op or fail
// cleanly without affecting the returned entity.
typedef int (__cdecl* fn_lua_create_entity_t)(void* L);
static fn_lua_create_entity_t original_lua_create_entity = nullptr;

static int __cdecl hooked_lua_create_entity(void* L)
{
   int team = 0;
   const bool haveTeam = g_lua.gettop && g_lua.isnumber &&
                         g_lua.gettop((lua_State*)L) >= 4 &&
                         g_lua.isnumber((lua_State*)L, 4);
   if (haveTeam) team = g_lua.tointeger((lua_State*)L, 4);

   const int ret = original_lua_create_entity(L);
   if (ret <= 0) return ret;

   void* entity = g_lua.touserdata((lua_State*)L, -1);
   if (entity) apply_vehicle_fixup(entity, team);
   return ret;
}

void lua_create_entity_hook_install(uintptr_t exe_base)
{
   if (!g_addr->lua_create_entity) return;

   original_lua_create_entity = (fn_lua_create_entity_t)
      resolve(exe_base, g_addr->lua_create_entity);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_lua_create_entity, hooked_lua_create_entity);
   DetourTransactionCommit();
}

struct lua_func_entry {
   const char* name;
   lua_CFunction func;
};

static const lua_func_entry custom_functions[] = {
   { "HttpGet",               lua_HttpGet },
   { "HttpPut",               lua_HttpPut },
   { "HttpPost",              lua_HttpPost },
   { "GetCharacterWeapon",    lua_GetCharacterWeapon },
   // v6 rebuild (2026-07-18): builds a real Weapon via WeaponClass::Build and
   // destroys the old one — the v1..v5 in-place mutation approach corrupted
   // MemoryPool free lists (see docs/CharacterWeaponSystem.md).
   { "SetCharacterWeapon",    lua_SetCharacterWeapon },
   { "GetWeaponAmmo",         lua_GetWeaponAmmo },
   { "SetWeaponAmmo",         lua_SetWeaponAmmo },
   { "HttpGetAsync",          lua_HttpGetAsync },
   { "HttpPutAsync",          lua_HttpPutAsync },
   { "HttpPostAsync",         lua_HttpPostAsync },
   { "RemoveUnitClass",       lua_RemoveUnitClass },
   { "OnCharacterExitVehicle",       lua_OnCEV },
   { "OnCharacterExitVehicleName",   lua_OnCEVName },
   { "OnCharacterExitVehicleTeam",   lua_OnCEVTeam },
   { "OnCharacterExitVehicleClass",  lua_OnCEVClass },
   { "ReleaseCharacterExitVehicle",  lua_ReleaseCEV },
   { "SetLoadDisplayLevel",      lua_SetLoadDisplayLevel },
   { "SetFogRange",              lua_SetFogRange },
   { "SetFogEnable",             lua_SetFogEnable },
   { nullptr, nullptr }
};

// True once register_lua_functions() has built the GameExt table. Until then
// _G.GameExt may be nil or (on the flat-global fallback) a boolean, so indexing
// it for the Disable field would be unsafe.
static bool s_gameExtTableBuilt = false;

bool gameext_is_disabled(lua_State* L)
{
   if (!s_gameExtTableBuilt) return false;

   // Must operate on the CALLER's live Lua state, not the cached global g_L:
   // during a C callback the engine may be running a different thread/state, and
   // pushing onto a stale state's stack faults and corrupts the VM.
   if (!L || !g_lua.pushlstring || !g_lua.gettable ||
       !g_lua.toboolean || !g_lua.gettop || !g_lua.settop)
      return false;

   bool disabled = false;
   const int top = g_lua.gettop(L);         // snapshot before we borrow the stack
   __try {
      g_lua.pushlstring(L, "GameExt", 7);
      g_lua.gettable(L, -10001);            // _G.GameExt (our table)
      g_lua.pushlstring(L, "disable", 7);
      g_lua.gettable(L, -2);                // GameExt.disable
      disabled = g_lua.toboolean(L, -1) != 0;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      disabled = false;
   }
   g_lua.settop(L, top);                     // always restore, even if a call faulted
   return disabled;
}

void register_lua_functions(lua_State* L)
{
   for (const lua_func_entry* entry = custom_functions; entry->name; ++entry)
      lua_register_func(L, entry->name, entry->func);

   // Detection table for scripts. GameExt is a table (still truthy, so the
   // classic `if GameExt then ... end` presence check keeps working) that
   // namespaces the metadata fields:
   //   if GameExt then ... end             -- extension present
   //   print(GameExt.version)              -- "1.0.0"
   //   if GameExt.build == "steam" then    -- which exe we're patching
   //   GameExt.disable = true              -- opt out of intrusive features
   const char* buildName = "unknown";
   switch (g_build) {
      case GameBuild::Modtools: buildName = "modtools"; break;
      case GameBuild::Steam:    buildName = "steam";    break;
      case GameBuild::GOG:      buildName = "gog";      break;
      default: break;
   }

   // IMPORTANT: this runs on every init_state, several times per mission load.
   // If _G.GameExt already exists (we built it on an earlier init_state for this
   // same Lua state), leave it completely alone — rebuilding it would wipe any
   // GameExt.disable a mission script has already set. On a genuinely new Lua
   // state (new mission) GameExt is nil again, so it rebuilds and disable resets.
   bool alreadyPresent = false;
   if (g_lua.pushlstring && g_lua.gettable && g_lua.toboolean &&
       g_lua.gettop && g_lua.settop) {
      const int top = g_lua.gettop(L);
      g_lua.pushlstring(L, "GameExt", 7);
      g_lua.gettable(L, -10001);                    // _G.GameExt (nil on a fresh state)
      alreadyPresent = g_lua.toboolean(L, -1) != 0; // any table is truthy
      g_lua.settop(L, top);
   }

   const bool canBuildTable = g_lua.newtable &&
                              g_lua.pushlstring && g_lua.settable;
   if (alreadyPresent) {
      s_gameExtTableBuilt = true;                    // table persists; reader can index it
   } else if (canBuildTable) {
      // Build _G.GameExt = { version = ..., build = ... }. Push the global key
      // first, then the table; set each field with settable(-3) (which leaves
      // the table on top), then settable(-10001) writes _G["GameExt"] = table.
      g_lua.pushlstring(L, "GameExt", 7);          // [K]
      if (g_lua.new_table(L)) {                     // [K, T]
         auto set_field = [&](const char* key, const char* value) {
            g_lua.pushlstring(L, key, strlen(key));
            g_lua.pushlstring(L, value, strlen(value));
            g_lua.settable(L, -3);
         };
         set_field("version", GAMEEXT_VERSION_STRING);
         set_field("build",   buildName);
         g_lua.settable(L, -10001);                 // _G.GameExt = T
         s_gameExtTableBuilt = true;                // enables gameext_is_disabled()
      } else if (g_lua.settop) {
         g_lua.settop(L, -2);                        // drop the dangling key
      }
   }

   // If the table couldn't be built (a build where Lua is wired but table
   // creation isn't — shouldn't happen in practice), fall back to a plain
   // truthy GameExt so `if GameExt then` still works. No flat GameExtVersion /
   // GameExtBuild globals: scripts read GameExt.version / GameExt.build.
   if (!s_gameExtTableBuilt)
      lua_set_global_bool(L, "GameExt", true);
}
