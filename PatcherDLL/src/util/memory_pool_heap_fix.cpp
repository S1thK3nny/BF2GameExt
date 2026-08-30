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

// MemoryPool layout.  These three are the same on every build -- mNode (16) and
// mLabel[32] fill 0x00..0x30, so mSize starts at 0x30 everywhere.
constexpr uint32_t kLabel = 0x10; // char[32]
constexpr uint32_t kSize  = 0x30;
constexpr uint32_t kCount = 0x34;
constexpr uint32_t kGrow  = 0x38;
//
// mHeap and mFree are NOT the same: the release builds drop mPeak, shifting them
// down four bytes, so both come from the per-build table.  See game_addrs.hpp.
//
// Allocate is `void __thiscall Allocate(MemoryPool*, uint)` on all three builds
// -- read from Ghidra, not inferred: one stack argument, so it RETs 4 and the
// __fastcall shim below must declare exactly one.
uint32_t g_heapOffset = 0;
uint32_t g_freeOffset = 0;

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
   if (pool && *reinterpret_cast<void**>(pool + g_freeOffset) == nullptr) {
      InterlockedIncrement(&s_growths);

      const int32_t live     = g_currHeap ? *g_currHeap : -1;
      const int32_t captured = *reinterpret_cast<int32_t*>(pool + g_heapOffset);

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
         *reinterpret_cast<int32_t*>(pool + g_heapOffset) = live;
         InterlockedIncrement(&s_retargets);
      }
   }

   g_origAllocate(self, edx, size);
}

} // namespace

void memory_pool_heap_fix_install(uintptr_t exe_base)
{
   if (!g_memoryPoolHeapFix && !g_poolGrowthDiag) return;
   if (g_addr->memory_pool_allocate == 0 || g_addr->red_curr_heap == 0) return;

   g_heapOffset = (uint32_t)g_addr->mempool_heap_offset;
   g_freeOffset = (uint32_t)g_addr->mempool_free_offset;
   if (g_heapOffset == 0 || g_freeOffset == 0) return;

   g_currHeap     = reinterpret_cast<int32_t*>(resolve(exe_base, g_addr->red_curr_heap));
   g_origAllocate = reinterpret_cast<fn_allocate_t>(
      resolve(exe_base, g_addr->memory_pool_allocate));

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
