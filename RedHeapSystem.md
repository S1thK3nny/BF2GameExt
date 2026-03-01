# BF2 Red Heap System

Everything reverse-engineered about SWBF2's (BF2_modtools exe) memory
allocator system.

---

## Overview

BF2 uses a bespoke heap allocator called "Red" (prefix `_Red`). All heap
operations go through a single **current heap** global; whichever heap is
"current" receives all allocations until the current heap is switched.

---

## Globals

| Symbol | Address | Type | Notes |
|--------|---------|------|-------|
| `__RedCurrHeap` / `g_CurrentHeapID` | — | `int` | Index of the currently active heap. Both names appear in the codebase and refer to the same value. |
| `RunTimeHeap` | `0x00b30220` | `int` | Index = **2** after `BuildHeaps`. Persistent for the whole process lifetime. |
| `TempLoadHeap` index | `0x00ba111c` via `s_loadHeap` | `int` | Index = **3** during a level load. Exists only between `BuildHeaps` and `ReleaseTempHeap`. |
| `gTempHeapBlock` | — | `void*` | The 2 MB block TempLoadHeap is built on. Allocated from RunTimeHeap by `BuildHeaps`; freed back by `ReleaseTempHeap`. |

---

## Functions

### `_RedSetCurrentHeap` — `0x007e2c70`

```c
// __cdecl
int _RedSetCurrentHeap(int heapIndex);
```

Sets `__RedCurrHeap` (and `g_CurrentHeapID`) to `heapIndex`. Returns the
**previous** heap index. This is the only safe way to switch the active heap.

Every allocation function reads `g_CurrentHeapID` to decide which heap to use,
so the switch is global and immediate.

---

### `GameMemory::BuildHeaps` — thunk `0x0044a020`

Called early in `GameState::PreStateInit`. Allocates a **2 MB block**
(`gTempHeapBlock`) from RunTimeHeap, then constructs TempLoadHeap over it.
After this call:

- RunTimeHeap index = 2
- TempLoadHeap index = 3
- `gTempHeapBlock` points to the 2 MB region

---

### `GameMemory::ReleaseTempHeap` — `0x00415230`

Called once, from `FUN_00734040` at `0x00734837`, after the level has finished
loading. Sequence:

1. `memset(gTempHeapBlock, 0xDE, 2MB)` — entire region is filled with `0xDEDEDEDE`.
2. `_RedSetTempHeap(-1)` — marks the TempLoadHeap slot as gone.
3. Frees `gTempHeapBlock` back to RunTimeHeap.

**Does not call `_RedSetCurrentHeap`.** After `ReleaseTempHeap` returns,
`__RedCurrHeap` is still 3 (the now-destroyed TempLoadHeap). Any pointer into
the former TempLoadHeap region is immediately dangling.

---

### `_RedGetHeapFree` — `0x007e2d60`

```c
// __cdecl
int _RedGetHeapFree(int heapIndex);
```

Walks the free list of the specified heap and sums up free block sizes.
Called after load (in `FUN_0044fe10`) as a sanity check on RunTimeHeap.

Crash site: `0x007e2d77` — `MOV EAX, [ECX+4]` where ECX is a free-list node
pointer. If ECX = `0xDEDEDEDE`, the read faults. This indicates a free-list
node backed by former TempLoadHeap memory (filled with `0xDE` by
`ReleaseTempHeap`).

---

### `_RedFreeToHeap`

Frees a block back to a specific heap by index. Searches all heaps, so
**cross-heap frees are valid** — a block allocated from TempLoadHeap can be
freed to RunTimeHeap and the allocator will find it correctly.

---

### `FUN_007e2e00` — free-list insertion, `0x007e2e00`

```c
// EDI = heap struct pointer
void FUN_007e2e00(uint* blockHeader);
```

Inserts `blockHeader` into the heap's free list in sorted (ascending address)
order. Performs forward coalescence (merge with next free block if adjacent)
and backward coalescence (merge with previous free block if adjacent). Normal,
non-crashing path when block headers contain valid metadata.

---

### `FUN_007e2e80` — unlink from allocated list, `0x007e2e80`

```c
// EAX = block_header, param_1 = heap list_head, param_2 = unused
void FUN_007e2e80(/* EAX */ int* blockHeader, int* listHead);
```

Removes a block from the heap's allocated-block doubly-linked list before
freeing it. Reads `blockHeader[0]` (prev) and `blockHeader[1]` (next) and
patches adjacent entries.

With a `0xDE`-filled block header (`next = 0xDEDEDEDE`), this function tries
to write to `*(int*)0xDEDEDEDE` — an access violation on the **write**, inside
this function. This is a distinct crash from `007E2D77` (which is a read AV
in the free-list walk).

---

## The SortHeap

The SortHeap is a dynamic array of render commands, filled by
`PlatformRenderTexture` calls and flushed at end-of-frame.

### `FUN_00806920` — buffer allocator, `0x00806920`

```c
int FUN_00806920(int* capacityInOut);
```

Allocates a new SortHeap backing buffer from `g_CurrentHeapID` (the current
heap at call time). Size = `*capacityInOut * 8` bytes.

### `FUN_00808860` — resize, `0x00808860`

```c
// __thiscall
uint FUN_00808860(int* pSortHeap, int newCap, char flags);
```

Called when the SortHeap array is at capacity. Sequence:

1. Calls `FUN_00806920` to allocate a new buffer — **from `g_CurrentHeapID`**.
2. Copies old data to the new buffer.
3. Calls `RedFreeToHeap(g_CurrentHeapID, oldBuffer, 0)` — frees old buffer
   **to `g_CurrentHeapID`**.
4. Updates the pointer and capacity fields.

Both the new allocation and the old-buffer free use whatever heap is current
at call time. If `g_CurrentHeapID = 3` (TempLoadHeap), the new buffer lives
in TempLoadHeap.

---

## MemoryPool

`MemoryPool::Allocate` at `0x008024A6`. Creates or expands slab storage from
`__RedCurrHeap`. If slabs are created while `__RedCurrHeap = TempLoadHeap`,
they are dangling after `ReleaseTempHeap`.

---

## Heap Layout During a Level Load

```
BuildHeaps
│   RunTimeHeap (idx 2) — persists forever
│   TempLoadHeap (idx 3) — built over gTempHeapBlock (2 MB from RunTimeHeap)
│
├── GameState::PreStateInit
│       _RedSetCurrentHeap(TempLoadHeap)  ← __RedCurrHeap = 3
│       LoadDisplay::Create               ← s_loadHeap = 3
│       _RedSetCurrentHeap(RunTimeHeap)   ← __RedCurrHeap = 2
│
├── ScriptInit / loading loop             ← __RedCurrHeap = 3 (TempLoadHeap) throughout
│       LoadDisplay::Update × many        ← internally saves/restores __RedCurrHeap
│           _RedSetCurrentHeap(s_loadHeap) → 3
│           Render()
│           _RedSetCurrentHeap(saved)     → restores 3
│       (custom) g_orig_load_render calls ← __RedCurrHeap = 3 if unguarded
│
├── LoadDisplay::End
│
└── ReleaseTempHeap
        memset(gTempHeapBlock, 0xDE, 2MB)
        __RedCurrHeap still = 3 after this
```

---

## Heap Guard Strategy (in `bf1_load_ext`)

Any code that allocates during the loading loop must explicitly switch to
RunTimeHeap first, because `__RedCurrHeap = TempLoadHeap` throughout.

| Guard | Purpose |
|-------|---------|
| `*g_s_load_heap_ptr = *g_runtime_heap_idx` before `g_orig_load_update` | Redirects Update's internal `_RedSetCurrentHeap(s_loadHeap)` to RunTimeHeap, covering all SortHeap growth from loading text + progress bar draws |
| `g_set_current_heap(*g_runtime_heap_idx)` before each `g_orig_load_render` | Covers PlatformRender's pre-callback MemoryPool and SortHeap allocations for directly-injected render calls |
| `g_set_current_heap(*g_runtime_heap_idx)` before BF1 `g_prt` calls in `hooked_render_screen` | Defence-in-depth for any BF1 draw that reaches the callback through an unguarded path |
