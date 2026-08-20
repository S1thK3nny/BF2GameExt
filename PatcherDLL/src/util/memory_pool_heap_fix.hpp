#pragma once

#include <stdint.h>

// =============================================================================
// MemoryPool growth heap fix.
//
// A MemoryPool captures the heap it will ever allocate from at CONSTRUCTION and
// never revisits that choice:
//
//   MemoryPool::Create   (modtools 0x00802200)
//     this->mHeap = ___RedCurrHeap;
//
//   MemoryPool::Allocate (modtools 0x00802300), when the free list runs dry
//     if (mHeap == -1) mHeap = ___RedCurrHeap;
//     saved = RedSetCurrentHeap(mHeap);
//     mFree = RedAllocFromHeap(___RedCurrHeap, mSize * mGrow, 0);
//     RedSetCurrentHeap(saved);
//
// Per docs/RE/RedHeapSystem.md the whole ScriptInit / loading loop runs with
// __RedCurrHeap = TempLoadHeap, a 2 MB block that ReleaseTempHeap memsets to
// 0xDE at the end of every load.  So a pool constructed during level load is
// bound to a heap that gets wiped -- and it survives only for as long as it
// never grows.  The first growth threads a fresh free list through released
// memory and starts handing those pointers out, which is why the crash
// correlates with the engine's own
//
//     Memory pool "%s" is full; raise count to at least %d
//
// A mission script can dodge this with SetMemoryPool, but only if you have the
// script.  Maps whose source is lost cannot be fixed that way, which is the
// case this exists for.
//
// THE FIX: before the engine grows a pool, point mHeap at the heap that is
// actually live right now.  During gameplay that is the persistent RunTimeHeap,
// so the new slab outlives the next load instead of being wiped by it.
//
// WHY THAT IS SAFE, and it is less invasive than it first looks.  Phantom's PDB
// gives MemoryPool as 84 bytes with exactly one storage pointer:
//
//     +0x00 Node mNode | +0x10 char mLabel[32] | +0x30 mSize | +0x34 mCount
//     +0x38 mGrow | +0x3C mUsed | +0x40 mPeak | +0x44 mHeap | +0x48 void* mPool
//     +0x4C bool mPoolOwner | +0x50 void* mFree
//
// There is nowhere to record a second slab, and Allocate only ever assigns
// mPool `if (mPool == NULL)`.  Grown slabs are therefore ALREADY unreachable and
// already leaked, whichever heap they came from.  Moving them to the persistent
// heap turns an unreachable-and-dangling slab into an unreachable-and-valid one.
// The leak is unchanged and tiny: Create defaults mGrow to (mSize+63)/mSize,
// which is one or two items, so each growth is on the order of 64 bytes.
//
// The retarget only fires when the live heap differs from the captured one, so
// if this diagnosis is wrong the condition simply never triggers.
//
// Set [Diagnostic] PoolGrowthDiag=1 to log every growth with the pool's name,
// its captured heap and the live heap -- that is what proves or disproves the
// diagnosis on a given map.
//
// modtools only for now; the retail addresses are not derived and the installer
// no-ops there.
// =============================================================================

extern bool g_memoryPoolHeapFix;

// Hand-added key, deliberately absent from ini_registry.hpp:
// [Diagnostic] PoolGrowthDiag.
extern bool g_poolGrowthDiag;

void memory_pool_heap_fix_install(uintptr_t exe_base);
void memory_pool_heap_fix_uninstall();
