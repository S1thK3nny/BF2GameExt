#include "pch.h"
#include "content_census.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

// See content_census.hpp for what this reports and why it reports each number
// twice where it can.

int g_contentCensusInterval = 0;

namespace {

constexpr uint32_t kEffectClassSlots = 256; // key slots; values follow at +256

// HeapObj / Tag layout, taken from the surviving accessors rather than a PDB.
// See game_addrs.hpp for the instructions that prove each one.
constexpr uint32_t kHeapStride    = 0x24;
constexpr uint32_t kHeapFreeHead  = 0x04;
constexpr uint32_t kHeapSize      = 0x14;
constexpr uint32_t kHeapName      = 0x18;
constexpr uint32_t kTagNext       = 0x04;
constexpr uint32_t kTagSize       = 0x08;

// The walk runs on our own thread while the game may be allocating, so a torn
// list is possible. Bound every traversal and treat the bound as "unknown"
// rather than reporting a half-summed figure as fact.
constexpr uint32_t kMaxHeaps     = 64;
constexpr uint32_t kMaxFreeNodes = 65536;

// Previous sample, for deltas. A one-shot number tells you where you are; the
// delta tells you where you are going, which is what catches a leak.
uint32_t s_prevUsed[kMaxHeaps] = {};
bool     s_havePrev = false;

uintptr_t s_exeBase = 0;
HANDLE    s_thread  = nullptr;
HANDLE    s_stop    = nullptr;

void census_log(const char* fmt, ...)
{
   FILE* f = nullptr;
   if (fopen_s(&f, "BF2GameExt.log", "a") != 0 || !f) return;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fclose(f);
}

// A bar that makes "you are nearly out" obvious at a glance in a wall of log.
const char* pressure(uint32_t used, uint32_t cap)
{
   if (cap == 0) return "";
   const uint32_t pct = (used * 100) / cap;
   if (pct >= 100) return "  <-- FULL";
   if (pct >= 90)  return "  <-- 90%+";
   if (pct >= 75)  return "  <-- 75%+";
   return "";
}

// Count non-zero keys directly. This is the number that cannot be wrong: it does
// not depend on the engine maintaining a counter correctly, only on the table
// base being right, and a wrong base shows up immediately as a nonsense figure
// rather than as a plausible lie.
bool scan_effect_classes(uint32_t* outScan, uint32_t* outCounter)
{
   if (g_addr->fx_effect_classes_table == 0 || g_addr->fx_effect_classes_count == 0)
      return false;

   const uint32_t* const keys =
      reinterpret_cast<const uint32_t*>(resolve(s_exeBase, g_addr->fx_effect_classes_table));
   const uint32_t* const counter =
      reinterpret_cast<const uint32_t*>(resolve(s_exeBase, g_addr->fx_effect_classes_count));

   uint32_t used = 0;
   __try {
      for (uint32_t i = 0; i < kEffectClassSlots; ++i)
         if (keys[i] != 0) ++used;
      *outCounter = *counter;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
   *outScan = used;
   return true;
}

// Bytes, rendered so a human can compare them at a glance.
void fmt_bytes(char* out, size_t cap, uint32_t bytes)
{
   if (bytes >= 1024u * 1024u)
      _snprintf_s(out, cap, _TRUNCATE, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
   else if (bytes >= 1024u)
      _snprintf_s(out, cap, _TRUNCATE, "%.1f KB", (double)bytes / 1024.0);
   else
      _snprintf_s(out, cap, _TRUNCATE, "%u B", bytes);
}

// Walk one heap's free list. Returns false if the list looked torn, in which
// case the caller reports nothing rather than a partial sum.
bool walk_free_list(const uint8_t* heap, uint32_t* outFree, uint32_t* outLargest)
{
   uint32_t total = 0, largest = 0, n = 0;
   __try {
      const uint8_t* node = *reinterpret_cast<const uint8_t* const*>(heap + kHeapFreeHead);
      while (node) {
         const uint32_t sz = *reinterpret_cast<const uint32_t*>(node + kTagSize);
         total += sz;
         if (sz > largest) largest = sz;
         if (++n > kMaxFreeNodes) return false;
         node = *reinterpret_cast<const uint8_t* const*>(node + kTagNext);
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
   *outFree = total;
   *outLargest = largest;
   return true;
}

void report_heaps()
{
   if (g_addr->red_heap_table_ptr == 0 || g_addr->red_heap_count == 0) return;

   uint32_t count = 0;
   const uint8_t* table = nullptr;
   __try {
      count = *reinterpret_cast<const uint32_t*>(resolve(s_exeBase, g_addr->red_heap_count));
      table = *reinterpret_cast<const uint8_t* const*>(resolve(s_exeBase, g_addr->red_heap_table_ptr));
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return;
   }
   if (!table || count == 0 || count > kMaxHeaps) return;

   census_log("[Census] --- memory ---");
   for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* const heap = table + (uintptr_t)i * kHeapStride;

      uint32_t size = 0;
      const char* name = nullptr;
      __try {
         size = *reinterpret_cast<const uint32_t*>(heap + kHeapSize);
         name = *reinterpret_cast<const char* const*>(heap + kHeapName);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         continue;
      }
      if (size == 0) continue;

      uint32_t freeBytes = 0, largest = 0;
      if (!walk_free_list(heap, &freeBytes, &largest)) {
         census_log("[Census]   %-14s (free list unreadable this tick)",
                    name ? name : "(unnamed)");
         continue;
      }

      const uint32_t used = (freeBytes <= size) ? size - freeBytes : 0;
      char usedS[32], sizeS[32], largeS[32];
      fmt_bytes(usedS, sizeof(usedS), used);
      fmt_bytes(sizeS, sizeof(sizeS), size);
      fmt_bytes(largeS, sizeof(largeS), largest);

      // Largest-contiguous-free is the number stock `mem` never prints, and the
      // one that actually predicts an allocation failure: a heap can be 40%
      // free and still refuse a large block through fragmentation.
      char delta[48];
      delta[0] = 0;
      if (s_havePrev && i < kMaxHeaps) {
         const long d = (long)used - (long)s_prevUsed[i];
         if (d != 0) {
            char dS[32];
            fmt_bytes(dS, sizeof(dS), (uint32_t)(d < 0 ? -d : d));
            _snprintf_s(delta, sizeof(delta), _TRUNCATE, "  (%c%s)", d < 0 ? '-' : '+', dS);
         }
      }
      if (i < kMaxHeaps) s_prevUsed[i] = used;

      census_log("[Census]   %-14s %9s / %-9s  largest free %9s%s%s",
                 name ? name : "(unnamed)", usedS, sizeS, largeS,
                 delta, pressure(used, size));
   }
   s_havePrev = true;
}

DWORD WINAPI census_thread(LPVOID)
{
   for (;;) {
      const DWORD ms = (DWORD)(g_contentCensusInterval > 0 ? g_contentCensusInterval : 30) * 1000u;
      if (WaitForSingleObject(s_stop, ms) == WAIT_OBJECT_0) return 0;
      content_census_report();
   }
}

} // namespace

void content_census_report()
{
   if (s_exeBase == 0) return;

   report_heaps();

   census_log("[Census] --- content ---");

   uint32_t scan = 0, counter = 0;
   if (scan_effect_classes(&scan, &counter)) {
      census_log("[Census]   effect classes   %3u / %u%s",
                 scan, kEffectClassSlots, pressure(scan, kEffectClassSlots));
      // The engine's own counter is reported next to the scan rather than
      // instead of it. They should always agree; if they ever do not, the
      // discrepancy IS the finding, so say so instead of quietly preferring one.
      if (counter != scan) {
         census_log("[Census]   (engine counter says %u, scan says %u -- they disagree,"
                    " trust the scan)", counter, scan);
      }
      if (scan >= kEffectClassSlots) {
         census_log("[Census]   WARNING: the effect table is FULL. Loading one more distinct"
                    " effect will hang during level load, and any lookup of an effect name"
                    " that is not registered will hang the game now.");
      }
   }

   if (g_addr->branch_region_count != 0) {
      __try {
         const uint32_t regions =
            *reinterpret_cast<const uint32_t*>(resolve(s_exeBase, g_addr->branch_region_count));
         census_log("[Census]   path regions    %u", regions);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
   }

   census_log("[Census] --- end ---");
}

void content_census_install(uintptr_t exe_base)
{
   s_exeBase = exe_base;
   if (g_contentCensusInterval <= 0) return;

   s_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
   if (!s_stop) return;
   // Below normal: this must never compete with the simulation for a core.
   s_thread = CreateThread(nullptr, 0, census_thread, nullptr, 0, nullptr);
   if (s_thread) SetThreadPriority(s_thread, THREAD_PRIORITY_BELOW_NORMAL);
}

void content_census_uninstall()
{
   if (s_stop) SetEvent(s_stop);
   if (s_thread) {
      WaitForSingleObject(s_thread, 2000);
      CloseHandle(s_thread);
      s_thread = nullptr;
   }
   if (s_stop) { CloseHandle(s_stop); s_stop = nullptr; }
}
