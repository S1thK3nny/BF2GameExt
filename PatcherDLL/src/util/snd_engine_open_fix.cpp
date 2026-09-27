#include "pch.h"
#include "snd_engine_open_fix.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "core/x86_emit.hpp"
#include "util/install_log.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// =============================================================================
// See the header for what this fixes.  The engine detail, modtools addresses
// (Steam / GOG in the table below):
//
// Snd::Engine::Open (0x00886420), the sample-RAM heap:
//
//   00886939  68 00 00 00 02   PUSH 0x2000000         ; 0x10000000 with SoundLimit
//   0088693E  E8 xx xx xx xx   CALL _malloc           ; <- redirected
//   00886943  83 C4 04         ADD  ESP,4
//   00886946  3B C5            CMP  EAX,EBP
//   00886948  A3 889F3302      MOV  [smSoundHeap],EAX
//   0088694D  0F84 C5FCFFFF    JZ   0x00886618        ; -> CALL DSClose, bail
//   ...
//   00886AEA  68 00 00 00 02   PUSH 0x2000000         ; SampleRAM::Init size,
//                                                     ; rewritten to what we got
//
// Snd::Engine::DSClose (0x008860C0), the stream loop:
//
//   00886174  33 FF            XOR  EDI,EDI
//   00886176  EB 08            JMP  0x00886180
//   00886178  8D A4 24 00000000 90                    ; alignment padding
//   00886180  8B 15 8C103302   MOV  EDX,[smStreams]   ; loop head
//   ...                                               ; derefs smStreams+EDI
//   008861CC  72 B2            JC   0x00886180
//   008861CE  E8 ...           CALL 0x0089AD30        ; empty-safe list walk
//   008861D3  B9 D0A03302      MOV  ECX,0x0233A0D0
//   008861D8  E8 ...           CALL 0x0088D290        ; closes the bracket the
//                                                     ; PUSH 0 / CALL at
//                                                     ; 0x0088616A opened
//   008861DD  E8 ...           CALL EngineBase::Close ; <- redirected
//
// The twelve bytes at 0x00886174 are an exact fit for the whole guard:
//
//   A1 8C103302   MOV  EAX,[smStreams]
//   85 C0         TEST EAX,EAX
//   74 51         JZ   0x008861CE     ; past the loop, still closes the bracket
//   33 FF         XOR  EDI,EDI
//   90            NOP                 ; falls into the loop head
//
// The retail loop has no padding: the bracket's CALL and the XOR EDI,EDI sit
// directly in front of the loop head, so those seven bytes become a JMP to
// dsclose_loop_guard_retail, which replays them and makes the same decision.
//
// EngineBase::Close is the inverse of EngineBase::Open, which writes smVoices
// and smStreams and is the only thing that does; Close NULLs both.  So smStreams
// NULL means "never opened, or already closed", and in both cases Close has
// nothing to do.  Only the call from DSClose is redirected; other callers keep
// the original.
// =============================================================================

namespace {

constexpr uint32_t kStockHeapBytes = 0x2000000; // 32 MB, also the fallback step

struct Sites {
   uintptr_t mallocCall;      // CALL _malloc for the heap (E8 rel32)
   uintptr_t malloc;          // its target
   uintptr_t initSizeImm;     // imm32 of PUSH size before SampleRAM::Init
   uintptr_t smStreams;       // Snd::EngineBase::smStreams (the pointer)
   uintptr_t loopGuard;       // modtools: XOR EDI,EDI; retail: CALL bracket
   uintptr_t bracket;         // retail: the CALL's target
   uintptr_t loopHead;        // retail: first instruction of the loop
   uintptr_t afterLoop;       // retail: first instruction after the loop
   uintptr_t closeCall;       // CALL EngineBase::Close inside DSClose
   uintptr_t engineBaseClose; // its target
};

constexpr Sites kModtools = {
   0x0088693E, 0x008D6A61, 0x00886AEB, 0x0233108C,
   0x00886174, 0, 0, 0,
   0x008861DD, 0x00882AA0,
};
constexpr Sites kSteam = {
   0x007323FC, 0x00752AEC, 0x007325BD, 0x009D8410,
   0x00731BBA, 0x0073C4F0, 0x00731BC1, 0x00731C11,
   0x00731C20, 0x00733ED0,
};
constexpr Sites kGOG = {
   0x007334EC, 0x00753BEC, 0x007336AD, 0x009D98B0,
   0x00732C8A, 0x0073D5E0, 0x00732C91, 0x00732CE1,
   0x00732CF0, 0x00734FC0,
};

constexpr size_t kModtoolsGuardLen = 12;
constexpr uint8_t kModtoolsGuardOrig[kModtoolsGuardLen] = {
   0x33, 0xFF, 0xEB, 0x08, 0x8D, 0xA4, 0x24, 0x00, 0x00, 0x00, 0x00, 0x90,
};
constexpr size_t kRetailGuardLen = 7; // CALL rel32 + XOR EDI,EDI

using fn_malloc_t = void*(__cdecl*)(size_t);

fn_malloc_t g_gameMalloc     = nullptr;
uint32_t*   g_initSizeImm    = nullptr;
uint32_t*   g_smStreams      = nullptr;

// Read by the naked stubs, so they have to be plain globals.
uintptr_t g_bracket          = 0;
uintptr_t g_loopHead         = 0;
uintptr_t g_afterLoop        = 0;
uintptr_t g_engineBaseClose  = 0;

// Everything we overwrote, for uninstall.
struct Saved {
   uint8_t* addr = nullptr;
   uint8_t  bytes[kModtoolsGuardLen] = {};
   size_t   len = 0;
};
Saved    s_mallocCall, s_loopGuard, s_closeCall;
uint32_t s_initSizeOrig = 0;

uint32_t largest_free_range()
{
   uint32_t best = 0;
   uintptr_t p = 0x10000;
   MEMORY_BASIC_INFORMATION mbi;
   while (p < 0x7FFF0000 && VirtualQuery((void*)p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
      if (mbi.State == MEM_FREE && mbi.RegionSize > best) best = (uint32_t)mbi.RegionSize;
      const uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
      if (next <= p) break;
      p = next;
   }
   return best;
}

constexpr uint32_t mb(uint32_t bytes) { return bytes >> 20; }

// Replaces the heap's CALL _malloc.  The caller pushed the requested size and
// pops it again, so this is a plain cdecl malloc as far as it can tell.  The
// block must still come from the game's own malloc: DSClose frees it with the
// game's free().
void* __cdecl sound_heap_alloc(size_t requested)
{
   const uint32_t freeBefore = largest_free_range();

   uint32_t size = (uint32_t)requested;
   void* heap = g_gameMalloc(size);
   while (!heap && size > kStockHeapBytes) {
      size -= kStockHeapBytes;
      heap = g_gameMalloc(size);
   }

   if (!heap) {
      install_log("[SoundHeap] could not allocate even %u MB of sample RAM (largest free range "
                  "%u MB) -- starting without sound", mb(size), mb(freeBefore));
      warn_gamelog(RED_SEVERITY_ERROR, SRC_FILE, __LINE__,
                   "[SoundHeap] Could not allocate %u MB of sound memory. The game will run "
                   "without sound. Restarting the PC usually fixes this.", mb(size));
      return nullptr;
   }

   // SampleRAM::Init must be told the size we really have, or it would hand out
   // sample slots past the end of the block.  A smaller size than the bitmap was
   // built for just leaves the bitmap's tail unused.
   if (*g_initSizeImm != size) protected_write(g_initSizeImm, &size, sizeof(size));

   if (size == requested) {
      install_log("[SoundHeap] %u MB of sample RAM at %p (largest free range was %u MB)",
                  mb(size), heap, mb(freeBefore));
   }
   else {
      install_log("[SoundHeap] %u MB of sample RAM did not fit (largest free range %u MB), "
                  "fell back to %u MB at %p", mb((uint32_t)requested), mb(freeBefore), mb(size), heap);
      warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__,
                   "[SoundHeap] Not enough free memory for %u MB of sound memory, using %u MB. "
                   "Levels with a lot of sound may run out. Restarting the PC usually fixes this.",
                   mb((uint32_t)requested), mb(size));
   }
   return heap;
}

// Retail DSClose: replays the bracket CALL and XOR EDI,EDI that the JMP here
// replaced, then enters the stream loop only if smStreams is set.  EAX is free:
// the loop writes it before reading it, and nothing after the loop reads it.
__declspec(naked) void dsclose_loop_guard_retail()
{
   __asm {
      call [g_bracket]
      mov  eax, [g_smStreams]
      cmp  dword ptr [eax], 0
      je   skip_loop
      xor  edi, edi
      jmp  [g_loopHead]
   skip_loop:
      jmp  [g_afterLoop]
   }
}

// DSClose's call to EngineBase::Close.  Register-transparent apart from the
// flags, since the retail callers are LTCG code.
__declspec(naked) void enginebase_close_guard()
{
   __asm {
      push eax
      mov  eax, [g_smStreams]
      cmp  dword ptr [eax], 0
      pop  eax
      je   skip_close
      jmp  [g_engineBaseClose]
   skip_close:
      ret
   }
}

bool is_call_to(const uint8_t* site, uintptr_t target)
{
   if (site[0] != 0xE8) return false;
   int32_t rel;
   memcpy(&rel, site + 1, 4);
   return (uintptr_t)(site + 5) + rel == target;
}

void save(Saved& s, uint8_t* addr, size_t len)
{
   s.addr = addr;
   s.len  = len;
   memcpy(s.bytes, addr, len);
}

void restore(Saved& s)
{
   if (!s.addr) return;
   protected_write(s.addr, s.bytes, s.len);
   s.addr = nullptr;
}

} // namespace

void snd_engine_open_fix_install(uintptr_t exe_base)
{
   const Sites* S;
   switch (g_build) {
   case GameBuild::Modtools: S = &kModtools; break;
   case GameBuild::Steam:    S = &kSteam;    break;
   case GameBuild::GOG:      S = &kGOG;      break;
   default: return;
   }

   auto at = [exe_base](uintptr_t va) { return (uint8_t*)resolve(exe_base, va); };

   uint8_t* mallocCall = at(S->mallocCall);
   uint8_t* initImm    = at(S->initSizeImm);
   uint8_t* loopGuard  = at(S->loopGuard);
   uint8_t* closeCall  = at(S->closeCall);
   const uintptr_t mallocFn = (uintptr_t)at(S->malloc);
   const uintptr_t closeFn  = (uintptr_t)at(S->engineBaseClose);

   // Every site is checked before any of them is touched, so a mismatch leaves
   // the engine exactly as it was.
   const bool retail = g_build != GameBuild::Modtools;
   bool ok = is_call_to(mallocCall, mallocFn) && initImm[-1] == 0x68 &&
             is_call_to(closeCall, closeFn);
   if (retail) {
      ok = ok && is_call_to(loopGuard, (uintptr_t)at(S->bracket)) &&
           loopGuard[5] == 0x33 && loopGuard[6] == 0xFF;
   }
   else {
      ok = ok && memcmp(loopGuard, kModtoolsGuardOrig, kModtoolsGuardLen) == 0;
   }
   if (!ok) {
      install_log("[SoundHeap] unexpected bytes at the Snd::Engine::Open sites -- fix not installed");
      return;
   }

   g_gameMalloc     = (fn_malloc_t)mallocFn;
   g_initSizeImm    = (uint32_t*)initImm;
   g_smStreams      = (uint32_t*)at(S->smStreams);
   g_engineBaseClose = closeFn;
   s_initSizeOrig   = *g_initSizeImm;

   save(s_mallocCall, mallocCall, 5);
   x86::write_branch(mallocCall, x86::kCall, &sound_heap_alloc);

   save(s_closeCall, closeCall, 5);
   x86::write_branch(closeCall, x86::kCall, &enginebase_close_guard);

   if (retail) {
      g_bracket   = (uintptr_t)at(S->bracket);
      g_loopHead  = (uintptr_t)at(S->loopHead);
      g_afterLoop = (uintptr_t)at(S->afterLoop);

      save(s_loopGuard, loopGuard, kRetailGuardLen);
      x86::write_branch(loopGuard, x86::kJmp, &dsclose_loop_guard_retail, kRetailGuardLen);
   }
   else {
      save(s_loopGuard, loopGuard, kModtoolsGuardLen);
      uint8_t guard[kModtoolsGuardLen] = {
         0xA1, 0, 0, 0, 0,   // MOV  EAX,[smStreams]
         0x85, 0xC0,         // TEST EAX,EAX
         0x74, 0x51,         // JZ   past the loop
         0x33, 0xFF,         // XOR  EDI,EDI
         0x90,               // NOP, into the loop head
      };
      const uint32_t streams = (uint32_t)(uintptr_t)g_smStreams;
      memcpy(guard + 1, &streams, 4);
      memcpy(loopGuard, guard, kModtoolsGuardLen);
   }
}

void snd_engine_open_fix_uninstall()
{
   // Sections are re-protected by the time this runs.
   restore(s_loopGuard);
   restore(s_closeCall);
   restore(s_mallocCall);
   if (g_initSizeImm) {
      protected_write(g_initSizeImm, &s_initSizeOrig, sizeof(s_initSizeOrig));
      g_initSizeImm = nullptr;
   }
}
