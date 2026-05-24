#include "pch.h"
#include "mem_watch.hpp"
#include "command_registry.hpp"
#include "core/resolve.hpp"

#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// =============================================================================
// MemWatch — generic hardware data-breakpoint diagnostic. See header.
// =============================================================================

namespace {

constexpr int kMaxWatch = 4;   // DR0..DR3

// ---- One armed watchpoint ---------------------------------------------------
struct Watch {
   uintptr_t addr;   // absolute runtime address (0 = slot free)
   uint8_t   len;    // 1, 2 or 4 bytes
   uint8_t   rw;     // DR7 R/W code: 0b01 = write, 0b11 = read|write
};

// ---- State ------------------------------------------------------------------
uintptr_t        s_exeBase = 0;
PVOID            s_veh     = nullptr;
volatile bool    s_active  = false;

Watch            s_watch[kMaxWatch] = {};

struct Hit { uint32_t eipUnreloc; uint32_t count; uint8_t drMask; };
constexpr int    kMaxHits = 256;
Hit              s_hits[kMaxHits];
int              s_hitCount = 0;
CRITICAL_SECTION s_cs;
bool             s_csInit = false;

// ---------------------------------------------------------------------------
// Record one accessor EIP (deduplicated). Runs inside the VEH.
// ---------------------------------------------------------------------------
void record_hit(uintptr_t eipReloc, uint8_t drMask)
{
   uint32_t unreloc = (uint32_t)(eipReloc - s_exeBase + kUnrelocatedBase);

   EnterCriticalSection(&s_cs);
   for (int i = 0; i < s_hitCount; ++i) {
      if (s_hits[i].eipUnreloc == unreloc) {
         s_hits[i].count++;
         s_hits[i].drMask |= drMask;
         LeaveCriticalSection(&s_cs);
         return;
      }
   }
   if (s_hitCount < kMaxHits) {
      s_hits[s_hitCount].eipUnreloc = unreloc;
      s_hits[s_hitCount].count      = 1;
      s_hits[s_hitCount].drMask     = drMask;
      s_hitCount++;
   }
   LeaveCriticalSection(&s_cs);
}

// ---------------------------------------------------------------------------
// Vectored exception handler — fires on DR0..DR3 data-breakpoint traps.
// ---------------------------------------------------------------------------
LONG CALLBACK veh(EXCEPTION_POINTERS* ep)
{
   if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
      CONTEXT* c = ep->ContextRecord;
      if (c->Dr6 & 0xF) {                       // B0..B3 — one of our data bps
         if (s_active)
            record_hit(c->Eip, (uint8_t)(c->Dr6 & 0xF));
         c->Dr6 = 0;                            // clear sticky status on resume
         return EXCEPTION_CONTINUE_EXECUTION;
      }
   }
   return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// LEN field encoding for DR7: 1 byte -> 0b00, 2 -> 0b01, 4 -> 0b11.
// ---------------------------------------------------------------------------
uint32_t len_code(uint8_t len)
{
   switch (len) {
      case 1:  return 0b00;
      case 2:  return 0b01;
      case 4:  return 0b11;
      default: return 0b11;
   }
}

// ---------------------------------------------------------------------------
// Build DR7 from the current watch table.
//   per slot n: Ln (local enable) = bit n*2
//               R/Wn (2 bits) at bit 16 + n*4
//               LENn (2 bits) at bit 18 + n*4
// ---------------------------------------------------------------------------
DWORD build_dr7()
{
   DWORD dr7 = 0;
   for (int n = 0; n < kMaxWatch; ++n) {
      if (!s_watch[n].addr) continue;
      dr7 |= (1u << (n * 2));                                  // local enable
      dr7 |= ((uint32_t)(s_watch[n].rw & 0x3)   << (16 + n * 4));
      dr7 |= (len_code(s_watch[n].len)          << (18 + n * 4));
   }
   return dr7;
}

void apply_to_thread(HANDLE hThread, DWORD dr7)
{
   CONTEXT ctx;
   ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
   if (!GetThreadContext(hThread, &ctx)) return;
   ctx.Dr0 = s_watch[0].addr;
   ctx.Dr1 = s_watch[1].addr;
   ctx.Dr2 = s_watch[2].addr;
   ctx.Dr3 = s_watch[3].addr;
   ctx.Dr6 = 0;
   ctx.Dr7 = dr7;
   ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
   SetThreadContext(hThread, &ctx);
}

// Apply the current watch table to EVERY thread in this process.
void apply_all_threads()
{
   DWORD  dr7  = build_dr7();
   DWORD  pid  = GetCurrentProcessId();
   DWORD  self = GetCurrentThreadId();

   HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
   if (snap == INVALID_HANDLE_VALUE) {
      apply_to_thread(GetCurrentThread(), dr7);
      return;
   }

   THREADENTRY32 te; te.dwSize = sizeof(te);
   if (Thread32First(snap, &te)) {
      do {
         if (te.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID))
            continue;
         if (te.th32OwnerProcessID != pid) continue;

         if (te.th32ThreadID == self) {
            apply_to_thread(GetCurrentThread(), dr7);
            continue;
         }
         HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                               FALSE, te.th32ThreadID);
         if (!h) continue;
         SuspendThread(h);
         apply_to_thread(h, dr7);
         ResumeThread(h);
         CloseHandle(h);
      } while (Thread32Next(snap, &te));
   }
   CloseHandle(snap);
}

void disarm_all()
{
   s_active = false;
   for (int i = 0; i < kMaxWatch; ++i) s_watch[i] = Watch{};
   apply_all_threads();   // DR7 == 0 now -> all disabled
}

// ---------------------------------------------------------------------------
// Console command.
// ---------------------------------------------------------------------------
int __cdecl cmd_memwatch(void* /*console*/, unsigned int /*id*/, const char* args)
{
   GameLog_t log = get_gamelog();
   if (!log) return 1;

   // Trim leading whitespace.
   const char* p = args ? args : "";
   while (*p && isspace((unsigned char)*p)) ++p;

   // ---- bare "memwatch" -> report + disarm ----
   if (*p == '\0') {
      EnterCriticalSection(&s_cs);
      int count = s_hitCount;
      LeaveCriticalSection(&s_cs);

      log("[memwatch] DISARMED. %d distinct accessor EIP(s):\n", count);
      if (count == 0) {
         log("   (nothing accessed the watched address(es))\n");
      } else {
         for (int i = 0; i < count; ++i)
            log("   EIP(unreloc)=%08X  hits=%u  drMask=%X  (DR%s%s%s%s)\n",
                s_hits[i].eipUnreloc, s_hits[i].count, s_hits[i].drMask,
                (s_hits[i].drMask & 1) ? "0" : "", (s_hits[i].drMask & 2) ? "1" : "",
                (s_hits[i].drMask & 4) ? "2" : "", (s_hits[i].drMask & 8) ? "3" : "");
      }
      log("[memwatch] EIP is the instruction AFTER the access (data bp = trap).\n");
      disarm_all();
      return 1;
   }

   // ---- "memwatch clear" -> silent disarm ----
   if (_strnicmp(p, "clear", 5) == 0) {
      disarm_all();
      log("[memwatch] cleared all watchpoints.\n");
      return 1;
   }

   // ---- "memwatch <hexaddr> [len] [r|w|rw]" -> arm one ----
   char* end = nullptr;
   uintptr_t addr = (uintptr_t)strtoul(p, &end, 16);   // hex, with or without 0x
   if (end == p || addr == 0) {
      log("[memwatch] usage: memwatch <hexaddr> [len 1|2|4] [r|w|rw]   |   memwatch (report)   |   memwatch clear\n");
      return 1;
   }

   // Optional length.
   uint8_t len = 4;
   const char* q = end;
   while (*q && isspace((unsigned char)*q)) ++q;
   if (isdigit((unsigned char)*q)) {
      long l = strtol(q, &end, 10);
      if (l == 1 || l == 2 || l == 4) len = (uint8_t)l;
      q = end;
      while (*q && isspace((unsigned char)*q)) ++q;
   }

   // Optional mode. x86 has no read-only bp: 'r'/'rw' -> read|write (0b11), 'w' -> write (0b01).
   uint8_t rw = 0b11;
   if (*q == 'w' || *q == 'W') rw = 0b01;

   if (addr & (uintptr_t)(len - 1)) {
      log("[memwatch] WARNING: address %08X is not %d-byte aligned; hardware bp may not fire.\n",
          (uint32_t)addr, len);
   }

   int slot = -1;
   for (int i = 0; i < kMaxWatch; ++i) {
      if (s_watch[i].addr == addr) { slot = i; break; }   // re-arm same addr
   }
   if (slot < 0)
      for (int i = 0; i < kMaxWatch; ++i)
         if (!s_watch[i].addr) { slot = i; break; }
   if (slot < 0) {
      log("[memwatch] all %d slots in use — run 'memwatch' to report+clear first.\n", kMaxWatch);
      return 1;
   }

   // First arm of a fresh session: reset the hit log.
   if (!s_active) {
      EnterCriticalSection(&s_cs);
      s_hitCount = 0;
      LeaveCriticalSection(&s_cs);
   }

   s_watch[slot].addr = addr;
   s_watch[slot].len  = len;
   s_watch[slot].rw   = rw;
   apply_all_threads();
   s_active = true;

   uint8_t cur = 0;
   __try { cur = *(uint8_t*)addr; } __except (EXCEPTION_EXECUTE_HANDLER) {}
   log("[memwatch] ARMED DR%d on %08X  len=%d  mode=%s  (first byte=%02X)\n",
       slot, (uint32_t)addr, len, (rw == 0b01) ? "write" : "read|write", cur);
   log("[memwatch] run 'memwatch' (no args) to report + disarm.\n");
   return 1;
}

} // namespace

void MemWatch::install(uintptr_t exe_base)
{
   s_exeBase = exe_base;
   if (!s_csInit) { InitializeCriticalSection(&s_cs); s_csInit = true; }
   if (!s_veh)    s_veh = AddVectoredExceptionHandler(1, veh);
}

void MemWatch::lateInit()
{
   DebugCommandRegistry::addCommand("memwatch", cmd_memwatch);
}

void MemWatch::uninstall()
{
   if (s_active) disarm_all();
   if (s_veh)    { RemoveVectoredExceptionHandler(s_veh); s_veh = nullptr; }
   if (s_csInit) { DeleteCriticalSection(&s_cs); s_csInit = false; }
}
