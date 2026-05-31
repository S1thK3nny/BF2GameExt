#include "pch.h"
#include "list_anim_banks.hpp"
#include "command_registry.hpp"
#include "core/resolve.hpp"

#pragma warning(disable: 4996) // _snprintf / strncpy deprecation

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <detours.h>

// =============================================================================
// ListAnimBanks — see header. Walks the global RedAnimation hash table and
// reports every loaded animation bank, the live total, and the distinct count.
//
// Name capture: the engine only writes a RedAnimation's name string (at +0x20)
// when g_bDumpGraphicsMemoryUsage is set during Read{Zaa,Zaf}. We do NOT set
// that flag globally — it is also read by state-cleanup code (FUN_00450090),
// which would spam Dump{Texture,Model,Animation}MemUsage on every transition.
// Instead we hook the two RedAnimation chunk loaders and flip the flag on ONLY
// for the duration of those calls. Names get stored; the dump sites always see
// the flag as 0, so nothing dumps.
// =============================================================================

// Need to fix the counting of anim banks. It is inaccurate as of now.

namespace {

// ---- Global anim hash table (PblHashTableCode<RedAnimation>) ----------------
//
// Open-addressing layout (from PblHashTableCode::_Find @0x007e1a40):
//   buckets       = size >> 1
//   keys[i]       = table[i]              for i in [0, buckets)
//   values[i]     = table[i + buckets]    RedAnimation* parallel to the key
// A key of 0 means the bucket is empty.
constexpr int kTableSize = 0x800;
constexpr int kBuckets   = kTableSize >> 1;   // 0x400

// ---- RedAnimation layout (confirmed via ReadZaa/ReadZaf/ctor disasm) --------
constexpr int kRA_NameHash      = 0x04;   // uint  _uiNameHash
constexpr int kRA_ZephyrBank    = 0x14;   // ZephyrAnimBank* (NULL => name registered, no data resident)
constexpr int kRA_Name          = 0x20;   // char* m_szName (only set when g_bDumpGraphicsMemoryUsage is on)

// ---- State ------------------------------------------------------------------
uintptr_t s_exeBase   = 0;
uint32_t* s_table     = nullptr;   // anim hash table base
uint8_t*  s_dumpFlag  = nullptr;   // g_bDumpGraphicsMemoryUsage
GameLog_t s_log       = nullptr;

// ---- Scoped name-capture hooks on the two bank loaders ----------------------
typedef void(__cdecl* ReadChunk_t)(void* chunk);
ReadChunk_t s_origReadZaf = nullptr;
ReadChunk_t s_origReadZaa = nullptr;

// Enable the engine's name capture only while the loader runs, then restore the
// flag to whatever it was. Save/restore (rather than force 0) keeps this safe
// under nesting and if the -dumpgfxmem switch was genuinely passed.
void __cdecl hooked_ReadZaf(void* chunk)
{
   uint8_t prev = s_dumpFlag ? *s_dumpFlag : 0;
   if (s_dumpFlag) *s_dumpFlag = 1;
   s_origReadZaf(chunk);
   if (s_dumpFlag) *s_dumpFlag = prev;
}

void __cdecl hooked_ReadZaa(void* chunk)
{
   uint8_t prev = s_dumpFlag ? *s_dumpFlag : 0;
   if (s_dumpFlag) *s_dumpFlag = 1;
   s_origReadZaa(chunk);
   if (s_dumpFlag) *s_dumpFlag = prev;
}

// Copy the bank name into out (root-stripped if stripSub), returning whether a
// usable name was found. Falls back to "#<hash>" when the engine didn't store
// a name string.
bool bank_name(void* entry, uint32_t hash, char* out, int outSz)
{
   const char* name = nullptr;
   __try {
      name = *(const char**)((char*)entry + kRA_Name);
      if (name && (*name == '\0')) name = nullptr;
   } __except (EXCEPTION_EXECUTE_HANDLER) { name = nullptr; }

   if (name) {
      __try {
         strncpy(out, name, outSz - 1);
         out[outSz - 1] = '\0';
         return true;
      } __except (EXCEPTION_EXECUTE_HANDLER) {}
   }
   _snprintf(out, outSz, "#%08X", hash);
   out[outSz - 1] = '\0';
   return false;
}

// Strip a trailing "_<digits>" sub-bank suffix in place: "human_5" -> "human",
// "human_rifle" stays "human_rifle". Mirrors how sub-banks share a root bank.
void strip_subbank(char* name)
{
   int len = (int)strlen(name);
   int i = len - 1;
   if (i < 0 || !isdigit((unsigned char)name[i])) return;
   while (i >= 0 && isdigit((unsigned char)name[i])) --i;
   if (i >= 0 && name[i] == '_') name[i] = '\0';
}

// ---- Distinct root-name accumulator -----------------------------------------
constexpr int kMaxDistinct = 256;
constexpr int kNameLen     = 64;
char s_distinct[kMaxDistinct][kNameLen];
int  s_nDistinct = 0;

void distinct_reset() { s_nDistinct = 0; }

// Add a root name if not already present (case-insensitive). Returns true if
// it was newly added.
bool distinct_add(const char* root)
{
   for (int i = 0; i < s_nDistinct; ++i)
      if (_stricmp(s_distinct[i], root) == 0) return false;
   if (s_nDistinct >= kMaxDistinct) return false;
   strncpy(s_distinct[s_nDistinct], root, kNameLen - 1);
   s_distinct[s_nDistinct][kNameLen - 1] = '\0';
   ++s_nDistinct;
   return true;
}

// ---------------------------------------------------------------------------
// Console command.
// ---------------------------------------------------------------------------
int __cdecl cmd_listanimbanks(void* /*console*/, unsigned int /*id*/, const char* args)
{
   if (!s_log)   return 1;
   if (!s_table) { s_log("[animbanks] hash table not resolved.\n"); return 1; }

   const char* p = args ? args : "";
   while (*p && isspace((unsigned char)*p)) ++p;
   bool namesOnly = (_strnicmp(p, "names", 5) == 0);

   distinct_reset();
   int total    = 0;   // entries present in the table (key != 0)
   int resident = 0;   // of those, with ZephyrAnimBank data actually loaded

   s_log("[animbanks] %s:\n", namesOnly ? "distinct bank names" : "loaded animation banks");

   for (int b = 0; b < kBuckets; ++b) {
      uint32_t key = 0;
      void*    entry = nullptr;
      __try {
         key = s_table[b];
         if (key == 0) continue;
         entry = (void*)s_table[b + kBuckets];
      } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
      if (!entry) continue;

      ++total;

      uint32_t hash = key;
      bool hasData = false;
      __try {
         hash    = *(uint32_t*)((char*)entry + kRA_NameHash);
         hasData = (*(void**)((char*)entry + kRA_ZephyrBank) != nullptr);
      } __except (EXCEPTION_EXECUTE_HANDLER) {}
      if (hasData) ++resident;

      char name[kNameLen];
      bool named = bank_name(entry, hash, name, sizeof(name));

      char root[kNameLen];
      strncpy(root, name, sizeof(root) - 1);
      root[sizeof(root) - 1] = '\0';
      strip_subbank(root);
      bool newRoot = distinct_add(root);

      if (!namesOnly) {
         s_log("  %-32s hash=%08X  %s%s\n",
               name, hash,
               hasData ? "[resident]" : "[no-data] ",
               named ? "" : " (name not captured)");
      } else if (newRoot) {
         s_log("  %s\n", root);
      }
   }

   s_log("[animbanks] total banks=%d (resident=%d, no-data=%d) | distinct names=%d\n",
         total, resident, total - resident, s_nDistinct);
   if (total == 0)
      s_log("[animbanks] none loaded yet (load a level first).\n");
   return 1;
}

} // namespace

void ListAnimBanks::install(uintptr_t exe_base)
{
   s_exeBase  = exe_base;
   s_table    = (uint32_t*)resolve(exe_base, game_addrs::modtools::anim_hash_table);
   s_log      = (GameLog_t)resolve(exe_base, game_addrs::modtools::game_log);
   s_dumpFlag = (uint8_t*) resolve(exe_base, game_addrs::modtools::anim_dump_gfx_mem_flag);

   // Hook the two bank loaders so name capture is enabled ONLY while they run
   // (see file header). They are created early, before any .lvl loads.
   s_origReadZaf = (ReadChunk_t)resolve(exe_base, game_addrs::modtools::anim_read_zaf);
   s_origReadZaa = (ReadChunk_t)resolve(exe_base, game_addrs::modtools::anim_read_zaa);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)s_origReadZaf, hooked_ReadZaf);
   DetourAttach(&(PVOID&)s_origReadZaa, hooked_ReadZaa);
   DetourTransactionCommit();
}

void ListAnimBanks::lateInit()
{
   DebugCommandRegistry::addCommand("listanimbanks", cmd_listanimbanks);
}

void ListAnimBanks::uninstall()
{
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (s_origReadZaf) DetourDetach(&(PVOID&)s_origReadZaf, hooked_ReadZaf);
   if (s_origReadZaa) DetourDetach(&(PVOID&)s_origReadZaa, hooked_ReadZaa);
   DetourTransactionCommit();
}
