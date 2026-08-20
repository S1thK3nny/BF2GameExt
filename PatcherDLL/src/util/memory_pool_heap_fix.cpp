#include "pch.h"
#include "memory_pool_heap_fix.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <detours.h>
#include <stdio.h>
#include <stdarg.h>

// See memory_pool_heap_fix.hpp for the mechanism and why the retarget is safe.

bool g_memoryPoolHeapFix = true;
bool g_poolGrowthDiag    = false;

namespace {

// MemoryPool layout, from Phantom's PDB (84 bytes total).  Only the fields this
// needs are named; the rest are documented in the header.
constexpr uint32_t kLabel = 0x10; // char[32]
constexpr uint32_t kSize  = 0x30;
constexpr uint32_t kCount = 0x34;
constexpr uint32_t kGrow  = 0x38;
constexpr uint32_t kHeap  = 0x44;
constexpr uint32_t kFree  = 0x50;

// modtools.  Allocate is `void __thiscall Allocate(MemoryPool*, uint)` -- read
// from Ghidra, not inferred: one stack argument, so it RETs 4 and the __fastcall
// shim below must declare exactly one.
constexpr uintptr_t kAllocate    = 0x00802300;
constexpr uintptr_t kRedCurrHeap = 0x00CF68DC;

using fn_allocate_t = void(__fastcall*)(void* self, void* edx, uint32_t size);

fn_allocate_t g_origAllocate = nullptr;
int32_t*      g_currHeap     = nullptr;

volatile LONG s_growths   = 0;
volatile LONG s_retargets = 0;
int  s_logged = 0;
constexpr int kMaxLogged = 64;

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

void __fastcall hooked_allocate(void* self, void* edx, uint32_t size)
{
   uint8_t* const pool = static_cast<uint8_t*>(self);

   // A null free list is the engine's own signal that this call will grow.
   // Everything else falls straight through, so the common path costs one load
   // and one compare.
   if (pool && *reinterpret_cast<void**>(pool + kFree) == nullptr) {
      InterlockedIncrement(&s_growths);

      const int32_t live     = g_currHeap ? *g_currHeap : -1;
      const int32_t captured = *reinterpret_cast<int32_t*>(pool + kHeap);

      if (g_poolGrowthDiag && s_logged < kMaxLogged) {
         ++s_logged;
         diag_log("[PoolGrow] \"%.32s\" size=%u count=%u grow=%u  capturedHeap=%d liveHeap=%d%s",
                  reinterpret_cast<const char*>(pool + kLabel),
                  *reinterpret_cast<uint32_t*>(pool + kSize),
                  *reinterpret_cast<uint32_t*>(pool + kCount),
                  *reinterpret_cast<uint32_t*>(pool + kGrow),
                  captured, live,
                  (captured != live && live >= 0) ? "   <-- would grow onto a stale heap" : "");
      }

      // Only act when the captured heap is not the one that is actually live.
      // If the diagnosis is wrong this never fires and nothing changes.
      if (g_memoryPoolHeapFix && live >= 0 && captured != live) {
         *reinterpret_cast<int32_t*>(pool + kHeap) = live;
         InterlockedIncrement(&s_retargets);
      }
   }

   g_origAllocate(self, edx, size);
}

} // namespace

void memory_pool_heap_fix_install(uintptr_t exe_base)
{
   if (!g_memoryPoolHeapFix && !g_poolGrowthDiag) return;
   if (g_build != GameBuild::Modtools) return;

   g_currHeap     = reinterpret_cast<int32_t*>(resolve(exe_base, kRedCurrHeap));
   g_origAllocate = reinterpret_cast<fn_allocate_t>(resolve(exe_base, kAllocate));

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG rd = DetourAttach(reinterpret_cast<PVOID*>(&g_origAllocate), hooked_allocate);
   const LONG rc = DetourTransactionCommit();

   if (rd != NO_ERROR || rc != NO_ERROR) {
      g_origAllocate = nullptr;
      diag_log("[PoolGrow] install failed: attach=%ld commit=%ld", rd, rc);
   }
}

void memory_pool_heap_fix_uninstall()
{
   if (!g_origAllocate) return;

   if (g_poolGrowthDiag || s_retargets > 0)
      diag_log("[PoolGrow] --- final --- growths=%ld  retargeted=%ld",
               s_growths, s_retargets);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(reinterpret_cast<PVOID*>(&g_origAllocate), hooked_allocate);
   DetourTransactionCommit();
   g_origAllocate = nullptr;
}
