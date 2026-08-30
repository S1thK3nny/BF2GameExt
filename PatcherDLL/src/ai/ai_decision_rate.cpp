#include "pch.h"
#include "ai_decision_rate.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <stdio.h>
#include <stdarg.h>

// See ai_decision_rate.hpp for the mechanism and why scaling this table is safe.

float g_aiDecisionRate = 1.0f;

namespace {

// The stock table, in seconds, tier 0 (furthest) through tier 4 (nearest).
// Every site is verified against its entry before anything is written, so a
// build whose table has moved leaves the game untouched rather than corrupting
// an unrelated constant.
constexpr float kStock[5] = {4.0f, 3.0f, 2.0f, 1.0f, 0.25f};

// Tier 4 is what the engine gives a unit standing next to a player.  Nothing is
// taken below it: this dial closes the gap between distant and nearby AI, it
// does not raise the ceiling.
constexpr float kFloor = 0.25f;

// The dial's own bounds.  4.0 lands demand near the 10-per-turn budget at the
// unit counts this was measured at; 0.25 is the other direction, for reclaiming
// frame time on very large battles.
constexpr float kMinRate = 0.25f;
constexpr float kMaxRate = 4.0f;

float* s_site[5] = {};
float  s_orig[5] = {};
bool   s_patched = false;

// CRT logging, not GameLog: install runs inside the window where every section
// is mapped PAGE_READWRITE, so calling back into the engine would EXEC-fault.
void diag_log(const char* fmt, ...)
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

} // namespace

void ai_decision_rate_install(uintptr_t exe_base)
{
   float rate = g_aiDecisionRate;
   if (rate <= 0.0f) return; // 0 or negative reads as "leave it alone"
   if (rate < kMinRate) rate = kMinRate;
   if (rate > kMaxRate) rate = kMaxRate;
   if (rate == 1.0f) return; // stock

   const uintptr_t va[5] = {
      g_addr->ai_lod_interval_t0, g_addr->ai_lod_interval_t1,
      g_addr->ai_lod_interval_t2, g_addr->ai_lod_interval_t3,
      g_addr->ai_lod_interval_t4,
   };

   float* site[5] = {};
   for (int i = 0; i < 5; ++i) {
      if (va[i] == 0) return; // not mapped for this build
      site[i] = reinterpret_cast<float*>(resolve(exe_base, va[i]));
   }

   // Verify the whole table before touching any of it -- a half-written table
   // would be worse than leaving it stock.
   for (int i = 0; i < 5; ++i) {
      if (*site[i] != kStock[i]) {
         diag_log("[AIRate] tier %d site %08X reads %f, expected %f -- left stock",
                  i, (unsigned)va[i], *site[i], kStock[i]);
         return;
      }
   }

   float applied[5];
   for (int i = 0; i < 5; ++i) {
      applied[i] = kStock[i] / rate;
      if (applied[i] < kFloor) applied[i] = kFloor;

      s_site[i] = site[i];
      s_orig[i] = *site[i];
      *site[i]  = applied[i];
   }
   s_patched = true;

   diag_log("[AIRate] rate=%.2f  intervals %.2f %.2f %.2f %.2f %.2f s "
            "(tier 0 furthest .. tier 4 nearest a player)",
            rate, applied[0], applied[1], applied[2], applied[3], applied[4]);
}

void ai_decision_rate_uninstall()
{
   if (!s_patched) return;

   // Sections are re-protected by now, so these cannot be plain stores.
   for (int i = 0; i < 5; ++i)
      if (s_site[i]) protected_write(s_site[i], &s_orig[i], sizeof(float));

   s_patched = false;
}
