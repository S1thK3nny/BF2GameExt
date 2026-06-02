#include "pch.h"
#include "list_anim_banks.hpp"
#include "command_registry.hpp"
#include "core/resolve.hpp"

#pragma warning(disable: 4996) // _snprintf / strncpy deprecation

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <detours.h>

// =============================================================================
// ListAnimBanks — see header.
//
// Default view walks the SoldierAnimationBank distinct-NAME registry
// (SoldierAnimationBank::_GetBank @0x5703c0, base 0x00ACECF8, count 0x00ACECE8).
// This is the registry that AddBank (0x5704b0) hard-caps at 16 and that the
// alloca arrays in SetupBodyMasks are sized to — i.e. the count that actually
// crashes the game. Only soldier banks are added here, so vehicle/FP/preview
// banks (atst, balutar, speederbike, …) never appear and never count, which is
// exactly what we want. Each slot is 0x2c bytes:
//   +0x00 char name[0x20]   (always populated by AddBank, no dumpgfxmem needed)
//   +0x20 uint nameHash
//   +0x24 int  parentBankIdx
//
// `listanimbanks all` falls back to the old behaviour: walk the global
// RedAnimation hash table and dump every loaded bank (vehicles included).
//
// Name capture (all-view only): the engine only writes a RedAnimation's name
// string (at +0x20) when g_bDumpGraphicsMemoryUsage is set during Read{Zaa,Zaf}.
// We do NOT set that flag globally — it is also read by state-cleanup code
// (FUN_00450090), which would spam Dump{Texture,Model,Animation}MemUsage on
// every transition. Instead we hook the two RedAnimation chunk loaders and flip
// the flag on ONLY for the duration of those calls. Names get stored; the dump
// sites always see the flag as 0, so nothing dumps.
// =============================================================================

namespace {

// Hard cap AddBank enforces (CMP/`15 < count`): the 17th distinct name → crash.
constexpr int kSoldierBankCap = 16;

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

// ---- SoldierAnimationBank registry layout (slot stride 0x2c) ----------------
constexpr int kReg_Stride   = 0x2c;
constexpr int kReg_Name     = 0x00;   // char[0x20]
constexpr int kReg_NameHash = 0x20;   // uint
constexpr int kReg_Parent   = 0x24;   // int  (-1 / parent bank index)

// ---- State ------------------------------------------------------------------
uintptr_t s_exeBase   = 0;
uint32_t* s_table     = nullptr;   // anim hash table base
uint8_t*  s_dumpFlag  = nullptr;   // g_bDumpGraphicsMemoryUsage
GameLog_t s_log       = nullptr;
char*     s_reg       = nullptr;   // soldier bank-name registry base
int*      s_regCount  = nullptr;   // distinct soldier bank-name count

// ---- Clean log output (bf2log, no RedWarning severity/source header) --------
uint8_t* s_fmtFlag = nullptr;   // RedWarning::g_bFormatted

// printf-style line emitter that routes through RedWarning::LogMessage (so it
// lands in the bf2log like every other engine message), but temporarily clears
// g_bFormatted so the line isn't decorated with "Message Severity: N\n
// <file>(line)". Save/restore keeps it safe under nesting / the -dumpgfxmem path.
void con_log(const char* fmt, ...)
{
   if (!s_log) return;
   char buf[1024];
   va_list ap;
   va_start(ap, fmt);
   _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
   va_end(ap);
   buf[sizeof(buf) - 1] = '\0';

   uint8_t prev = s_fmtFlag ? *s_fmtFlag : 0;
   if (s_fmtFlag) *s_fmtFlag = 0;
   s_log("%s", buf);
   if (s_fmtFlag) *s_fmtFlag = prev;
}

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
// Default view: the SoldierAnimationBank distinct-NAME registry — the set that
// counts toward the 16-name crash cap. Vehicle/FP/preview banks never appear.
// ---------------------------------------------------------------------------
int list_soldier_registry()
{
   if (!s_reg || !s_regCount) {
      con_log("[animbanks] soldier bank registry not resolved.\n");
      return 1;
   }

   int count = 0;
   __try { count = *s_regCount; } __except (EXCEPTION_EXECUTE_HANDLER) { count = -1; }
   constexpr int kMaxReg = 64;
   if (count < 0 || count > kMaxReg) {   // sanity guard against a bad read
      con_log("[animbanks] soldier bank count looks invalid (%d).\n", count);
      return 1;
   }

   // Snapshot the registry names, then tally how many resident/no-data .zaabin
   // sub-banks each one expanded to. A name like "human" loads as "human_0".."human_N"
   // in the global RedAnimation table, so the registry hash never matches those
   // entries directly — we match by stripped root name instead.
   char regName[kMaxReg][kNameLen];
   uint32_t regHash[kMaxReg] = {0};
   int subResident[kMaxReg]  = {0};   // .zaabin sub-banks with data loaded
   int subNoData[kMaxReg]    = {0};   // registered but no ZephyrAnimBank yet

   // Flat list of every matched sub-bank, so we can print the actual .zaabin
   // names grouped under their registry root.
   constexpr int kMaxSub = 256;
   char subName[kMaxSub][kNameLen];
   int  subOwner[kMaxSub] = {0};   // registry index this sub-bank belongs to
   bool subData[kMaxSub]  = {false};
   int  nSub = 0;

   for (int i = 0; i < count; ++i) {
      char* slot = s_reg + i * kReg_Stride;
      __try {
         strncpy(regName[i], slot + kReg_Name, kNameLen - 1);
         regName[i][kNameLen - 1] = '\0';
         regName[i][0x1f] = '\0';   // registry field is 0x20 wide
         regHash[i] = *(uint32_t*)(slot + kReg_NameHash);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         _snprintf(regName[i], kNameLen, "<slot %d unreadable>", i);
         regName[i][kNameLen - 1] = '\0';
      }
   }

   // Walk the global table once, attributing every soldier sub-bank to its
   // registry root. Anything whose root isn't a registered soldier name (i.e.
   // vehicles, FP, previews) is ignored, which is what excludes the noise.
   int totalResident = 0, totalNoData = 0;
   if (s_table) {
      for (int b = 0; b < kBuckets; ++b) {
         void* entry = nullptr;
         __try {
            if (s_table[b] == 0) continue;
            entry = (void*)s_table[b + kBuckets];
         } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
         if (!entry) continue;

         uint32_t hash = 0;
         bool hasData = false;
         __try {
            hash    = *(uint32_t*)((char*)entry + kRA_NameHash);
            hasData = (*(void**)((char*)entry + kRA_ZephyrBank) != nullptr);
         } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }

         char name[kNameLen];
         if (!bank_name(entry, hash, name, sizeof(name))) continue;  // need the name to match a root

         char root[kNameLen];
         strncpy(root, name, sizeof(root) - 1);
         root[sizeof(root) - 1] = '\0';
         strip_subbank(root);

         for (int i = 0; i < count; ++i) {
            if (_stricmp(regName[i], root) != 0) continue;
            if (hasData) ++subResident[i]; else ++subNoData[i];
            if (nSub < kMaxSub) {   // remember the actual sub-bank name to print
               strncpy(subName[nSub], name, kNameLen - 1);
               subName[nSub][kNameLen - 1] = '\0';
               subOwner[nSub] = i;
               subData[nSub]  = hasData;
               ++nSub;
            }
            break;
         }
      }
   }

   con_log("[animbanks] soldier animation bank names (count toward the %d-name cap):\n",
         kSoldierBankCap);
   for (int i = 0; i < count; ++i) {
      int loaded = subResident[i];
      totalResident += subResident[i];
      totalNoData   += subNoData[i];
      char tail[48];
      if (subNoData[i])
         _snprintf(tail, sizeof(tail), "%d .zaabin (+%d no-data)", loaded, subNoData[i]);
      else if (loaded)
         _snprintf(tail, sizeof(tail), "%d .zaabin", loaded);
      else
         _snprintf(tail, sizeof(tail), "[no-data]");
      tail[sizeof(tail) - 1] = '\0';
      con_log("  %2d  %-28s hash=%08X  %s\n", i, regName[i], regHash[i], tail);

      // List the actual .zaabin sub-banks attributed to this name.
      for (int s = 0; s < nSub; ++s) {
         if (subOwner[s] != i) continue;
         con_log("        - %-26s %s\n", subName[s], subData[s] ? "[resident]" : "[no-data] ");
      }
   }

   con_log("[animbanks] distinct soldier bank names=%d / %d max%s\n",
         count, kSoldierBankCap,
         count >= kSoldierBankCap ? "  *** AT CAP — one more crashes ***" : "");
   con_log("[animbanks] soldier .zaabin sub-banks loaded=%d resident (+%d no-data)\n",
         totalResident, totalNoData);
   if (count == 0)
      con_log("[animbanks] none registered yet (load a level / spawn a soldier first).\n");
   con_log("[animbanks] (use 'listanimbanks all' for the full RedAnimation table incl. vehicles)\n");
   return 1;
}

// ---------------------------------------------------------------------------
// 'all' view: full global RedAnimation hash table (vehicles included).
// ---------------------------------------------------------------------------
int list_all_banks()
{
   if (!s_table) { con_log("[animbanks] hash table not resolved.\n"); return 1; }

   distinct_reset();
   int total    = 0;   // entries present in the table (key != 0)
   int resident = 0;   // of those, with ZephyrAnimBank data actually loaded

   con_log("[animbanks] all loaded animation banks (RedAnimation table):\n");

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
      distinct_add(root);

      con_log("  %-32s hash=%08X  %s%s\n",
            name, hash,
            hasData ? "[resident]" : "[no-data] ",
            named ? "" : " (name not captured)");
   }

   con_log("[animbanks] total banks=%d (resident=%d, no-data=%d) | distinct roots=%d\n",
         total, resident, total - resident, s_nDistinct);
   if (total == 0)
      con_log("[animbanks] none loaded yet (load a level first).\n");
   return 1;
}

// ---------------------------------------------------------------------------
// Console command.
// ---------------------------------------------------------------------------
int __cdecl cmd_listanimbanks(void* /*console*/, unsigned int /*id*/, const char* args)
{
   if (!s_log) return 1;

   const char* p = args ? args : "";
   while (*p && isspace((unsigned char)*p)) ++p;

   if (_strnicmp(p, "all", 3) == 0)
      return list_all_banks();
   return list_soldier_registry();
}

} // namespace

void ListAnimBanks::install(uintptr_t exe_base)
{
   s_exeBase  = exe_base;
   s_table    = (uint32_t*)resolve(exe_base, game_addrs::modtools::anim_hash_table);
   s_log      = (GameLog_t)resolve(exe_base, game_addrs::modtools::game_log);
   s_dumpFlag = (uint8_t*) resolve(exe_base, game_addrs::modtools::anim_dump_gfx_mem_flag);
   s_reg      = (char*)    resolve(exe_base, game_addrs::modtools::anim_soldier_bank_registry);
   s_regCount = (int*)     resolve(exe_base, game_addrs::modtools::anim_soldier_bank_count);
   s_fmtFlag  = (uint8_t*) resolve(exe_base, game_addrs::modtools::log_formatted_flag);

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
