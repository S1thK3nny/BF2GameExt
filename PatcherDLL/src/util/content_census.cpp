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

   census_log("[Census] --- content budget ---");

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
