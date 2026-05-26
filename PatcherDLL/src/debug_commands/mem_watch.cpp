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
//
// Per unique accessor EIP we capture (on first sighting): a value/register
// snapshot and a best-effort call stack (stack-scan heuristic — the game is
// optimized and frequently omits frame pointers, so we scan ESP for words that
// point just past a CALL instruction rather than walking EBP).
// =============================================================================

namespace {

constexpr int kMaxWatch = 4;   // DR0..DR3
constexpr int kStackFrames = 5;

// ---- One armed watchpoint ---------------------------------------------------
struct Watch {
   uintptr_t addr;   // absolute runtime address (0 = slot free)
   uint8_t   len;    // 1, 2 or 4 bytes
   uint8_t   rw;     // DR7 R/W code: 0b01 = write, 0b11 = read|write
};

// ---- State ------------------------------------------------------------------
uintptr_t        s_exeBase  = 0;
uintptr_t        s_codeLo   = 0;   // .text bounds (for call-stack validation)
uintptr_t        s_codeHi   = 0;
PVOID            s_veh      = nullptr;
volatile bool    s_active   = false;

Watch            s_watch[kMaxWatch] = {};

struct Hit {
   uint32_t eipUnreloc;
   uint32_t count;
   uint8_t  drMask;
   uint32_t valAtAddr;             // dword at the matched watch addr, first hit
   uint32_t regs[5];               // EAX, ECX, EDX, ESI, EDI (first hit)
   uint32_t stack[kStackFrames];   // caller return addrs, unrelocated (0-term)
   uint8_t  stackN;
};
constexpr int    kMaxHits = 256;
Hit              s_hits[kMaxHits];
int              s_hitCount = 0;
CRITICAL_SECTION s_cs;
bool             s_csInit = false;

inline uint32_t to_unreloc(uintptr_t reloc)
{
   return (uint32_t)(reloc - s_exeBase + kUnrelocatedBase);
}

// ---------------------------------------------------------------------------
// Cache the main module's code section bounds (for call-stack validation).
// ---------------------------------------------------------------------------
void cache_code_bounds()
{
   __try {
      IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)s_exeBase;
      IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)(s_exeBase + dos->e_lfanew);
      s_codeLo = s_exeBase + nt->OptionalHeader.BaseOfCode;
      s_codeHi = s_codeLo  + nt->OptionalHeader.SizeOfCode;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      s_codeLo = s_exeBase;
      s_codeHi = s_exeBase + 0x00800000;   // crude fallback
   }
}

// A return address points just past a CALL. Confirm the preceding bytes are a
// CALL of some common form, to filter stale stack garbage.
bool is_return_addr(uintptr_t v)
{
   if (v <= s_codeLo + 6 || v >= s_codeHi) return false;
   __try {
      const uint8_t* p = (const uint8_t*)v;
      if (p[-5] == 0xE8) return true;                               // call rel32
      if (p[-6] == 0xFF && p[-5] == 0x15) return true;              // call [mem32]
      if (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0) return true;     // call reg
      if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;     // call [reg+disp8]
      if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;     // call [reg+disp32]
   } __except (EXCEPTION_EXECUTE_HANDLER) {}
   return false;
}

// Heuristic call stack: scan up the stack for plausible return addresses.
void capture_stack(uintptr_t esp, uint32_t* out, uint8_t* outN)
{
   *outN = 0;
   __try {
      for (int i = 0; i < 192 && *outN < kStackFrames; ++i) {
         uintptr_t v = *(uint32_t*)(esp + i * 4);
         if (is_return_addr(v))
            out[(*outN)++] = to_unreloc(v);
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------
// Record one access (deduplicated by EIP). Runs inside the VEH.
// ---------------------------------------------------------------------------
void record_hit(CONTEXT* c, uint8_t drMask)
{
   uint32_t unreloc = to_unreloc(c->Eip);

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
      Hit& h = s_hits[s_hitCount++];
      h.eipUnreloc = unreloc;
      h.count      = 1;
      h.drMask     = drMask;
      h.regs[0] = c->Eax; h.regs[1] = c->Ecx; h.regs[2] = c->Edx;
      h.regs[3] = c->Esi; h.regs[4] = c->Edi;

      // value at the matched watch address (lowest set bit of drMask)
      int slot = 0; uint8_t m = drMask;
      while (slot < kMaxWatch && !(m & 1)) { m >>= 1; ++slot; }
      h.valAtAddr = 0;
      if (slot < kMaxWatch && s_watch[slot].addr) {
         __try { h.valAtAddr = *(uint32_t*)s_watch[slot].addr; }
         __except (EXCEPTION_EXECUTE_HANDLER) {}
      }
      capture_stack(c->Esp, h.stack, &h.stackN);
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
      if (c->Dr6 & 0xF) {
         if (s_active)
            record_hit(c, (uint8_t)(c->Dr6 & 0xF));
         c->Dr6 = 0;
         return EXCEPTION_CONTINUE_EXECUTION;
      }
   }
   return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// DR7 helpers + thread application.
// ---------------------------------------------------------------------------
uint32_t len_code(uint8_t len)
{
   switch (len) { case 1: return 0b00; case 2: return 0b01; default: return 0b11; }
}

DWORD build_dr7()
{
   DWORD dr7 = 0;
   for (int n = 0; n < kMaxWatch; ++n) {
      if (!s_watch[n].addr) continue;
      dr7 |= (1u << (n * 2));
      dr7 |= ((uint32_t)(s_watch[n].rw & 0x3) << (16 + n * 4));
      dr7 |= (len_code(s_watch[n].len)        << (18 + n * 4));
   }
   return dr7;
}

void apply_to_thread(HANDLE hThread, DWORD dr7)
{
   CONTEXT ctx;
   ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
   if (!GetThreadContext(hThread, &ctx)) return;
   ctx.Dr0 = s_watch[0].addr; ctx.Dr1 = s_watch[1].addr;
   ctx.Dr2 = s_watch[2].addr; ctx.Dr3 = s_watch[3].addr;
   ctx.Dr6 = 0;
   ctx.Dr7 = dr7;
   ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
   SetThreadContext(hThread, &ctx);
}

void apply_all_threads()
{
   DWORD dr7  = build_dr7();
   DWORD pid  = GetCurrentProcessId();
   DWORD self = GetCurrentThreadId();

   HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
   if (snap == INVALID_HANDLE_VALUE) { apply_to_thread(GetCurrentThread(), dr7); return; }

   THREADENTRY32 te; te.dwSize = sizeof(te);
   if (Thread32First(snap, &te)) {
      do {
         if (te.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID))
            continue;
         if (te.th32OwnerProcessID != pid) continue;
         if (te.th32ThreadID == self) { apply_to_thread(GetCurrentThread(), dr7); continue; }

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
   apply_all_threads();
}

// ---------------------------------------------------------------------------
// Console command.
// ---------------------------------------------------------------------------
int __cdecl cmd_memwatch(void* /*console*/, unsigned int /*id*/, const char* args)
{
   GameLog_t log = get_gamelog();
   if (!log) return 1;

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
      }
      for (int i = 0; i < count; ++i) {
         Hit& h = s_hits[i];
         log("  [%d] EIP(unreloc)=%08X  hits=%u  DR%s%s%s%s  val@addr=%08X\n",
             i, h.eipUnreloc, h.count,
             (h.drMask & 1) ? "0" : "", (h.drMask & 2) ? "1" : "",
             (h.drMask & 4) ? "2" : "", (h.drMask & 8) ? "3" : "", h.valAtAddr);
         log("       EAX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X\n",
             h.regs[0], h.regs[1], h.regs[2], h.regs[3], h.regs[4]);
         if (h.stackN) {
            log("       callers(unreloc):");
            for (int s = 0; s < h.stackN; ++s) log(" %08X", h.stack[s]);
            log("\n");
         }
      }
      log("[memwatch] EIP/callers are unrelocated (imagebase %08X); EIP = instr AFTER access.\n",
          (uint32_t)kUnrelocatedBase);
      disarm_all();
      return 1;
   }

   // ---- "memwatch clear" ----
   if (_strnicmp(p, "clear", 5) == 0) {
      disarm_all();
      log("[memwatch] cleared all watchpoints.\n");
      return 1;
   }

   // ---- "memwatch [u]<hexaddr> [len] [r|w|rw]" ----
   // Leading 'u' => address is UNRELOCATED (Ghidra imagebase 0x400000); rebase it.
   bool unreloc = false;
   if (*p == 'u' || *p == 'U') { unreloc = true; ++p; }

   char* end = nullptr;
   uintptr_t addr = (uintptr_t)strtoul(p, &end, 16);
   if (end == p || addr == 0) {
      log("[memwatch] usage: memwatch [u]<hexaddr> [len 1|2|4] [r|w|rw]\n");
      log("                  'u' prefix = unrelocated Ghidra addr (e.g. memwatch uB9A3F0)\n");
      log("                  bare 'memwatch' = report+disarm ; 'memwatch clear' = silent disarm\n");
      return 1;
   }
   if (unreloc) addr = addr - kUnrelocatedBase + s_exeBase;

   uint8_t len = 4;
   const char* q = end;
   while (*q && isspace((unsigned char)*q)) ++q;
   if (isdigit((unsigned char)*q)) {
      long l = strtol(q, &end, 10);
      if (l == 1 || l == 2 || l == 4) len = (uint8_t)l;
      q = end;
      while (*q && isspace((unsigned char)*q)) ++q;
   }

   uint8_t rw = 0b11;                          // default read|write
   if (*q == 'w' || *q == 'W') rw = 0b01;      // write-only

   if (addr & (uintptr_t)(len - 1))
      log("[memwatch] WARNING: %08X not %d-byte aligned; hardware bp may not fire.\n",
          (uint32_t)addr, len);

   int slot = -1;
   for (int i = 0; i < kMaxWatch; ++i) if (s_watch[i].addr == addr) { slot = i; break; }
   if (slot < 0) for (int i = 0; i < kMaxWatch; ++i) if (!s_watch[i].addr) { slot = i; break; }
   if (slot < 0) {
      log("[memwatch] all %d slots in use — run 'memwatch' to report+clear first.\n", kMaxWatch);
      return 1;
   }

   if (!s_active) { EnterCriticalSection(&s_cs); s_hitCount = 0; LeaveCriticalSection(&s_cs); }

   s_watch[slot].addr = addr;
   s_watch[slot].len  = len;
   s_watch[slot].rw   = rw;
   apply_all_threads();
   s_active = true;

   uint32_t cur = 0;
   __try { cur = *(uint32_t*)addr; } __except (EXCEPTION_EXECUTE_HANDLER) {}
   log("[memwatch] ARMED DR%d on %08X (unreloc %08X) len=%d mode=%s cur=%08X\n",
       slot, (uint32_t)addr, to_unreloc(addr), len,
       (rw == 0b01) ? "write" : "read|write", cur);
   log("[memwatch] run 'memwatch' (no args) to report + disarm.\n");
   return 1;
}

} // namespace

void MemWatch::install(uintptr_t exe_base)
{
   s_exeBase = exe_base;
   cache_code_bounds();
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
