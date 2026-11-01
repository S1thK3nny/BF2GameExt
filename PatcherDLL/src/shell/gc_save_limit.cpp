#include "pch.h"
#include "gc_save_limit.hpp"
#include "gc_save_codec.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "lua/lua_hooks.hpp"
#include "util/install_log.hpp"

#include <detours.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =============================================================================
// How a GC save works (modtools addresses; Steam/GOG in game_addrs.hpp).
//
// ScriptCB_SaveMetagameState (0x469AE0) calls SaveLuaItems(buf 0xB31810, 0x200),
// which serializes its arguments and deflates them into the 512 byte buffer,
// storing the compressed length in 0xB31708. StartSaveMetagame queues the
// buffer with SaveFsm::QueueFile (0x749050) at a fixed 0x200, and the FSM
// writes a CRC-32/BZIP2 of the queued bytes followed by the bytes: 516 bytes.
// Loading queues the same 0x200, freads exactly that many and checks the CRC.
// ScriptCB_LoadMetagameState then calls LoadLuaItems(buf, size). Campaign and
// mission setup use the same pair with their own 256 byte buffers.
//
// The fix: the four code references to the buffer point at a 16 KB buffer
// here, SaveLuaItems and LoadLuaItems are replaced by gc_save_codec, and
// QueueFile is given the real size for the metagame buffer: 512 when the state
// fits (the stock file), else the stream length, and on load the file's size.
// Stock reading a bigger file fails its CRC and shows the load error popup.
//
// Steam/GOG compiled both Lua item functions with the buffer in EDX and the
// size on the stack, caller cleaned; all six callers use only EAX after.
// =============================================================================

using namespace gc_save;

namespace {

constexpr int kQueueLoad = 0;   // SaveFsmQueueOp (PDB)
constexpr int kQueueSave = 1;
constexpr int kTypeGc    = 2;   // SaveDevice extension table: 1 profile, 2 gc, 3 rote

alignas(16) uint8_t s_buf[kMaxSize];
uint32_t s_fileSize  = 0;   // payload the next metagame save writes
uint32_t s_used      = 0;   // compressed bytes of the state in s_buf
bool     s_installed = false;

// Last good compressed length per other buffer, returned when a save is refused.
struct Remembered { const uint8_t* buf; uint32_t size; };
Remembered s_last[4] = {};

GcLua      s_lua = {};
lua_State** s_luaState = nullptr;

using QueueMt_t = void(__thiscall*)(void* fsm, const wchar_t* name, void* dest, uint32_t size,
                                    int type, int op);
using QueueRt_t = void(__stdcall*)(const wchar_t* name, void* dest, uint32_t size, int type, int op);

void*     s_origSave  = nullptr;
void*     s_origLoad  = nullptr;
QueueMt_t s_origQueueMt = nullptr;
QueueRt_t s_origQueueRt = nullptr;

// The four buffer operands, restored on uninstall.
uint32_t* s_refs[4]    = {};
uint32_t  s_refOrig    = 0;

void report(const char* fmt, ...)
{
   char msg[512];
   va_list ap;
   va_start(ap, fmt);
   _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
   va_end(ap);
   install_log("%s", msg);
   warn_gamelog(RED_SEVERITY_ERROR, SRC_FILE, __LINE__, "%s", msg);
}

uint32_t recall(const uint8_t* buf)
{
   for (const Remembered& r : s_last)
      if (r.buf == buf) return r.size;
   return 0;
}

void remember(const uint8_t* buf, uint32_t size)
{
   for (Remembered& r : s_last)
      if (r.buf == buf || !r.buf) { r.buf = buf; r.size = size; return; }
}

const GcLua& bound_lua()
{
   s_lua.L = *s_luaState;
   return s_lua;
}

// Payload size of SaveGames\<name>.gc, built the way SaveDevice opens it, or 0.
uint32_t file_payload(const wchar_t* name)
{
   wchar_t rel[MAX_PATH], full[MAX_PATH];
   if (!name || _wmakepath_s(rel, nullptr, L"SaveGames", name, L"gc") != 0) return 0;
   if (!_wfullpath(full, rel, MAX_PATH)) return 0;
   WIN32_FILE_ATTRIBUTE_DATA fa;
   if (!GetFileAttributesExW(full, GetFileExInfoStandard, &fa)) return 0;
   if (!fa.nFileSizeHigh && fa.nFileSizeLow <= 4) return 0;
   if (fa.nFileSizeHigh || fa.nFileSizeLow - 4 > kMaxSize) {
      report("[GCSave] The Galactic Conquest save \"%ls\" is bigger than the %u bytes a save "
             "can hold, so it cannot be loaded.", name, kMaxSize + 4);
      return kMaxSize;   // the CRC then fails and the game shows its load error
   }
   return fa.nFileSizeLow - 4;
}

uint32_t queue_size(const wchar_t* name, const void* dest, uint32_t size, int type, int op)
{
   if (dest != s_buf || type != kTypeGc) return size;
   if (op == kQueueSave) return s_fileSize ? s_fileSize : size;
   if (op == kQueueLoad) {
      const uint32_t payload = file_payload(name);
      if (payload) s_fileSize = payload;
      return payload ? payload : size;
   }
   return size;
}

void __fastcall hooked_queue_mt(void* fsm, void*, const wchar_t* name, void* dest, uint32_t size,
                                int type, int op)
{
   s_origQueueMt(fsm, name, dest, queue_size(name, dest, size, type, op), type, op);
}

void __stdcall hooked_queue_rt(const wchar_t* name, void* dest, uint32_t size, int type, int op)
{
   s_origQueueRt(name, dest, queue_size(name, dest, size, type, op), type, op);
}

} // namespace

// File scope, not in the anonymous namespace, so the naked thunks can name them.
static uint32_t __cdecl save_impl(uint8_t* buf, uint32_t cap)
{
   const bool metagame = buf == s_buf;
   const SaveResult r = save(bound_lua(), buf, metagame ? kMaxSize : cap);
   if (r.status == Status::Ok || r.status == Status::Empty) {
      remember(buf, r.compressed);
      if (metagame) {
         s_used = r.compressed;
         if (r.compressed) s_fileSize = r.compressed > kVanillaSize ? r.compressed : kVanillaSize;
      }
      return r.compressed;
   }

   const char* what = metagame ? "ScriptCB_SaveMetagameState"
                               : "ScriptCB_SaveCampaignState or ScriptCB_SaveMissionSetup";
   if (r.status == Status::TooBig)
      report("[GCSave] %s did not save: the state compresses to %u bytes and at most %u fit. "
             "The previous state is kept.", what, r.compressed, metagame ? kMaxSize : cap);
   else
      report("[GCSave] %s did not save: %s. The previous state is kept.", what,
             status_text(r.status));
   return metagame ? s_used : recall(buf);
}

static int __cdecl load_impl(const uint8_t* buf, uint32_t len)
{
   if (!len) return 0;
   const bool metagame = buf == s_buf;
   const LoadResult r = load(bound_lua(), buf, metagame ? kMaxSize : len);
   if (r.status == Status::Ok) {
      if (metagame) s_used = r.consumed;
      return r.values;
   }
   report("[GCSave] %s could not read the saved state: %s, so nothing is loaded.",
          metagame ? "ScriptCB_LoadMetagameState"
                   : "ScriptCB_LoadCampaignState or ScriptCB_LoadMissionSetup",
          status_text(r.status));
   return 0;
}

// Steam/GOG: buffer in EDX, size at [esp+4], caller cleans.
static __declspec(naked) void retail_save()
{
   __asm {
      push dword ptr [esp + 4]
      push edx
      call save_impl
      add  esp, 8
      ret
   }
}

static __declspec(naked) void retail_load()
{
   __asm {
      push dword ptr [esp + 4]
      push edx
      call load_impl
      add  esp, 8
      ret
   }
}

int lua_GetMetagameSaveSize(lua_State* L)
{
   if (!s_installed || !g_lua.pushnumber) return 0;
   g_lua.pushnumber(L, (float)s_used);
   g_lua.pushnumber(L, (float)kMaxSize);
   return 2;
}

void gc_save_limit_install(uintptr_t exe_base)
{
   const GameAddrs& a = *g_addr;
   if (!a.save_lua_items || !a.load_lua_items || !a.save_fsm_queue_file || !a.metagame_buffer ||
       !a.metagame_buffer_ref_load || !a.metagame_buffer_ref_save ||
       !a.metagame_buffer_ref_save_state || !a.metagame_buffer_ref_load_state ||
       !a.lua_type || !a.lua_next || !a.lua_pushvalue || !a.lua_rawset || !a.lua_checkstack ||
       !a.lua_topointer || !a.lua_newtable || !a.g_lua_state_ptr)
      return;

   const bool modtools = g_build == GameBuild::Modtools;

   // MOV EAX,size / CALL __chkstk on modtools; PUSH EBP / MOV EBP,ESP first on retail.
   static const uint8_t kSaveMt[] = {0xB8, 0x00, 0x00, 0x01, 0x00, 0xE8};
   static const uint8_t kLoadMt[] = {0xB8, 0x04, 0x00, 0x01, 0x00, 0xE8};
   static const uint8_t kSaveRt[] = {0x55, 0x8B, 0xEC, 0xB8, 0x04, 0x00, 0x01, 0x00, 0xE8};
   static const uint8_t kLoadRt[] = {0x55, 0x8B, 0xEC, 0xB8, 0x08, 0x00, 0x01, 0x00, 0xE8};
   // PUSH ESI / MOV ESI,ECX on modtools; PUSH EBP / MOV EBP,ESP / MOV EAX,[count] on retail.
   static const uint8_t kQueueMt[] = {0x56, 0x8B, 0xF1, 0x8B, 0x86, 0xE0, 0x32, 0x03, 0x00};
   static const uint8_t kQueueRt[] = {0x55, 0x8B, 0xEC, 0xA1};

   void* const save  = resolve(exe_base, a.save_lua_items);
   void* const load  = resolve(exe_base, a.load_lua_items);
   void* const queue = resolve(exe_base, a.save_fsm_queue_file);
   const bool codeOk = modtools
      ? memcmp(save, kSaveMt, sizeof(kSaveMt)) == 0 && memcmp(load, kLoadMt, sizeof(kLoadMt)) == 0 &&
        memcmp(queue, kQueueMt, sizeof(kQueueMt)) == 0
      : memcmp(save, kSaveRt, sizeof(kSaveRt)) == 0 && memcmp(load, kLoadRt, sizeof(kLoadRt)) == 0 &&
        memcmp(queue, kQueueRt, sizeof(kQueueRt)) == 0;
   if (!codeOk) {
      install_log("[GCSave] not installed: save code at %08X / %08X / %08X does not match",
                  (unsigned)a.save_lua_items, (unsigned)a.load_lua_items,
                  (unsigned)a.save_fsm_queue_file);
      return;
   }

   // Each reference is a 5 byte PUSH imm32 or MOV EDX,imm32; the address is the operand.
   const uintptr_t refAddrs[4] = {a.metagame_buffer_ref_load, a.metagame_buffer_ref_save,
                                  a.metagame_buffer_ref_save_state, a.metagame_buffer_ref_load_state};
   const uint32_t stockBuf = (uint32_t)(uintptr_t)resolve(exe_base, a.metagame_buffer);
   for (int i = 0; i < 4; ++i) {
      s_refs[i] = (uint32_t*)resolve(exe_base, refAddrs[i]);
      if (*s_refs[i] != stockBuf) {
         install_log("[GCSave] not installed: buffer reference %08X reads %08X, expected %08X",
                     (unsigned)refAddrs[i], *s_refs[i], stockBuf);
         memset(s_refs, 0, sizeof(s_refs));
         return;
      }
   }

   s_luaState = (lua_State**)resolve(exe_base, a.g_lua_state_ptr);
   auto fn = [&](uintptr_t addr) { return resolve(exe_base, addr); };
   s_lua.gettop            = (decltype(s_lua.gettop))fn(a.lua_gettop);
   s_lua.settop            = (decltype(s_lua.settop))fn(a.lua_settop);
   s_lua.type              = (decltype(s_lua.type))fn(a.lua_type);
   s_lua.toboolean         = (decltype(s_lua.toboolean))fn(a.lua_toboolean);
   s_lua.tonumber          = (decltype(s_lua.tonumber))fn(a.lua_tonumber);
   // Raw on purpose: never writes a length on retail, but the codec only reads
   // strings it has typed as strings and measures them with strlen, as stock does.
   s_lua.tolstring         = (decltype(s_lua.tolstring))fn(a.lua_tolstring);
   s_lua.touserdata        = (decltype(s_lua.touserdata))fn(a.lua_touserdata);
   s_lua.topointer         = (decltype(s_lua.topointer))fn(a.lua_topointer);
   s_lua.pushvalue         = (decltype(s_lua.pushvalue))fn(a.lua_pushvalue);
   s_lua.next              = (decltype(s_lua.next))fn(a.lua_next);
   s_lua.checkstack        = (decltype(s_lua.checkstack))fn(a.lua_checkstack);
   s_lua.pushnil           = (decltype(s_lua.pushnil))fn(a.lua_pushnil);
   s_lua.pushboolean       = (decltype(s_lua.pushboolean))fn(a.lua_pushboolean);
   s_lua.pushnumber        = (decltype(s_lua.pushnumber))fn(a.lua_pushnumber);
   s_lua.pushlstring       = (decltype(s_lua.pushlstring))fn(a.lua_pushlstring);
   s_lua.pushlightuserdata = (decltype(s_lua.pushlightuserdata))fn(a.lua_pushlightuserdata);
   s_lua.newtable          = (decltype(s_lua.newtable))fn(a.lua_newtable);
   s_lua.rawset            = (decltype(s_lua.rawset))fn(a.lua_rawset);
   s_lua.gettable          = (decltype(s_lua.gettable))fn(a.lua_gettable);

   s_origSave = save;
   s_origLoad = load;
   if (modtools) s_origQueueMt = (QueueMt_t)queue;
   else          s_origQueueRt = (QueueRt_t)queue;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&s_origSave, modtools ? (void*)save_impl : (void*)retail_save);
   DetourAttach(&s_origLoad, modtools ? (void*)load_impl : (void*)retail_load);
   if (modtools) DetourAttach(reinterpret_cast<PVOID*>(&s_origQueueMt), hooked_queue_mt);
   else          DetourAttach(reinterpret_cast<PVOID*>(&s_origQueueRt), hooked_queue_rt);
   const LONG commit = DetourTransactionCommit();
   if (commit != NO_ERROR) {
      install_log("[GCSave] not installed: DetourTransactionCommit=%ld", commit);
      s_origSave = s_origLoad = nullptr;
      s_origQueueMt = nullptr;
      s_origQueueRt = nullptr;
      memset(s_refs, 0, sizeof(s_refs));
      return;
   }

   // .text is writable for the whole install window.
   s_refOrig = stockBuf;
   const uint32_t ours = (uint32_t)(uintptr_t)s_buf;
   for (uint32_t* ref : s_refs) *ref = ours;
   s_installed = true;
   install_log("[GCSave] installed: Galactic Conquest saves hold %u bytes", kMaxSize);
}

void gc_save_limit_uninstall()
{
   if (!s_installed) return;
   for (uint32_t* ref : s_refs)
      if (ref) protected_write(ref, &s_refOrig, sizeof(s_refOrig));
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&s_origSave, g_build == GameBuild::Modtools ? (void*)save_impl : (void*)retail_save);
   DetourDetach(&s_origLoad, g_build == GameBuild::Modtools ? (void*)load_impl : (void*)retail_load);
   if (s_origQueueMt) DetourDetach(reinterpret_cast<PVOID*>(&s_origQueueMt), hooked_queue_mt);
   if (s_origQueueRt) DetourDetach(reinterpret_cast<PVOID*>(&s_origQueueRt), hooked_queue_rt);
   DetourTransactionCommit();
   s_installed = false;
}
