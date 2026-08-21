#include "pch.h"
#include "reservation_pool.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

// See reservation_pool.hpp for the mechanism, why 127 is a hard ceiling, and why
// retail needs four sites where modtools needs one.

int g_reservationPoolSize = 127;

namespace {

constexpr uint32_t kStockCount = 60;   // 0x3C, the capacity every site encodes
constexpr int      kMinCount   = 61;   // below this there is nothing to do
constexpr int      kMaxCount   = 127;  // 0x7F -- the sign-extended PUSH imm8 ceiling

struct Site {
   uint8_t* addr;
   uint32_t width;   // 1 for the PUSH imm8, 4 for the folded imm32 copies
   uint32_t orig;
};

Site s_site[4] = {};
int  s_count   = 0;

// CRT logging, not GameLog: install runs inside the window where every section is
// mapped PAGE_READWRITE, so calling back into the engine would EXEC-fault.
void install_log(const char* fmt, ...)
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

uint32_t read_at(const uint8_t* p, uint32_t width)
{
   uint32_t v = 0;
   memcpy(&v, p, width);
   return v;
}

// Verify-then-record. Returns false the moment a site does not hold the stock
// capacity, which is the signal that the address has moved on this build.
bool expect(uintptr_t exe_base, uintptr_t va, uint32_t width, const char* what)
{
   uint8_t* const p = reinterpret_cast<uint8_t*>(resolve(exe_base, va));
   const uint32_t got = read_at(p, width);
   if (got != kStockCount) {
      install_log("[ReservePool] %s site %08X reads %u, expected %u -- left stock",
                  what, (unsigned)va, got, kStockCount);
      return false;
   }
   s_site[s_count].addr  = p;
   s_site[s_count].width = width;
   s_site[s_count].orig  = got;
   ++s_count;
   return true;
}

} // namespace

void reservation_pool_install(uintptr_t exe_base)
{
   int n = g_reservationPoolSize;
   if (n <= (int)kStockCount) return;  // 0 or any stock-or-smaller value = leave it alone
   if (n < kMinCount) n = kMinCount;
   if (n > kMaxCount) n = kMaxCount;

   const uintptr_t push   = g_addr->reserve_pool_count_push;
   const uintptr_t alloc  = g_addr->reserve_pool_count_alloc;
   const uintptr_t field  = g_addr->reserve_pool_count_field;
   const uintptr_t cookie = g_addr->reserve_pool_count_cookie;

   if (push == 0) return;  // not mapped for this build

   // A build declares either just the push (out-of-line ctor, which derives
   // everything from it) or all four folded copies. Anything in between means the
   // table is half-filled, and a partial write would leave mPoolSize describing
   // more elements than were allocated -- a heap overrun on the first Reserve.
   const bool anyFolded = (alloc != 0) || (field != 0) || (cookie != 0);
   if (anyFolded && (alloc == 0 || field == 0 || cookie == 0)) {
      install_log("[ReservePool] folded site set is incomplete "
                  "(alloc=%08X field=%08X cookie=%08X) -- left stock",
                  (unsigned)alloc, (unsigned)field, (unsigned)cookie);
      return;
   }

   // Only modtools has been verified to derive the whole ListPool from the pushed
   // count; on every other build the push is dead and patching it alone does
   // nothing. Refuse rather than pretend.
   if (!anyFolded && g_build != GameBuild::Modtools) {
      install_log("[ReservePool] build has no folded sites but is not modtools "
                  "-- left stock");
      return;
   }

   // Verify every site before writing any of them.
   s_count = 0;
   bool ok = expect(exe_base, push, 1, "push");
   if (ok && anyFolded) {
      ok = expect(exe_base, alloc,  4, "alloc")
        && expect(exe_base, field,  4, "mPoolSize")
        && expect(exe_base, cookie, 4, "cookie");
   }
   if (!ok) {
      s_count = 0;
      return;
   }

   const uint32_t v = (uint32_t)n;
   for (int i = 0; i < s_count; ++i)
      memcpy(s_site[i].addr, &v, s_site[i].width);

   install_log("[ReservePool] AI reservation pool %u -> %d (%d site%s)",
               kStockCount, n, s_count, s_count == 1 ? "" : "s");
}

void reservation_pool_uninstall()
{
   // Sections are re-protected by now, so this cannot be a plain store.
   for (int i = 0; i < s_count; ++i)
      protected_write(s_site[i].addr, &s_site[i].orig, s_site[i].width);
   s_count = 0;
}
