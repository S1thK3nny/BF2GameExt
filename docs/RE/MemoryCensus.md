# Memory census: reading BF2's heaps without the engine's getters

The modtools console command `mem` is `DumpMemoryUsage`, which calls a dozen small getters.
**That is not a portable base.** `DumpMemoryUsage` does not exist on Steam or GOG, and because it
was their only caller, 8 of its 12 getters were dead-stripped from both retail builds - including
`RedGetMaxHeaps`, `RedGetHeapName`, `RedGetHeapStart` and `RedGetHeapLargestFree`.

**Read the descriptor table directly instead.** It works identically on all three builds, needs
no stripped functions, and yields largest-contiguous-free even where the engine's own accessor is
gone. The layout is not inferred - `RedGetHeapSize` and `RedGetHeapFree` spell it out.

## The layout, from the surviving accessors

Steam `RedGetHeapSize` (`0x006C3A80`), `__cdecl`, plain `RET`:

    MOV EAX,[EBP+8]            ; heap index
    LEA ECX,[EAX+EAX*8]        ; i*9
    MOV EAX,[0x0093EBA4]       ; heap table POINTER
    MOV EAX,[EAX+ECX*4+0x14]   ; table + i*0x24 + 0x14

Steam `RedGetHeapFree` (`0x006C3A50`), same convention:

    MOV ECX,[0x0093EBA4]
    MOV ECX,[ECX+EAX*4+0x4]    ; +0x04 = free-list head
  loop:
    ADD EDX,[ECX+0x8]          ; Tag.size
    MOV ECX,[ECX+0x4]          ; Tag.next

So: **stride 0x24**, and free is a plain free-list walk. Largest-free is the same walk with
`max` instead of `+=`.

`HeapObj`, 0x24 bytes, identical on all four images:
`+0x00 start` `+0x04 free.first` `+0x08 free.last` `+0x0C used.first` `+0x10 used.last`
`+0x14 size (bytes)` `+0x18 char* name` `+0x1C align` `+0x20 frozen` `+0x21 permanent`

`Tag` (free/used node), 12 bytes: `+0x00 prev` `+0x04 next` `+0x08 size (bytes, INCLUDING the
12-byte header)`.

## Globals, per build (verified per image, not deltas)

| | modtools | Steam | GOG |
|---|---|---|---|
| heap table pointer | `0x00CF68E0` | `0x0093EBA4` | `0x00940044` |
| heap count | `0x00CF68E4` | `0x0093EBA8` | `0x00940048` |
| `__RedCurrHeap` | `0x00CF68DC` | `0x0093EBAC` | `0x0094004C` |
| `gRuntimeHeap` (index) | `0x00B30220` | `0x01E56160` | `0x01E57610` |
| `RedGetHeapSize` | `0x007E2DB0` | `0x006C3A80` | `0x006C4B10` |
| `RedGetHeapFree` | `0x007E2D60` | `0x006C3A50` | `0x006C4AE0` |

modtools' accessors are frameless (arg at `[ESP+4]`); retail uses an EBP frame (`[EBP+8]`). Same
`__cdecl`, different bytes - which is why a byte pattern lifted from modtools finds nothing.

## Sound: THE UNITS DIFFER BY BUILD

`Snd::EngineBase::smSoundRAMAllocator` is an `AllocBitmap`, 0x18 bytes:
`+0x0C mStartAddress` `+0x10 mMemorySize (bytes)` `+0x14 mEntrySize (bytes/block)`.
All three builds: total `0x2000000` (32 MiB), block `0x80`.

| | modtools | Steam | GOG |
|---|---|---|---|
| allocator base | `0x02331190` | `0x01E2968C` | `0x01E2AB2C` |
| `GetFreeMemory` -> **BYTES** | `0x00881DC0` | stripped | stripped |
| `GetFreeEntries` -> **BLOCKS** | `0x00881BB0` | `0x007454B0` | `0x007465A0` |

**modtools kept the bytes version; retail only has the blocks version.** Calling whichever exists
without converting reports a figure **128x too small** on retail. Multiply blocks by
`mEntrySize`. This is a plausible wrong number, not an obvious failure.

## Two more Phantom-layout failures

`MemoryPool` is **0x54 on modtools/Phantom** with `mPeak` at `+0x40`, and **0x50 on retail with
no `mPeak`**, everything from `+0x40` shifted down 4 (retail `+0x40` is `mHeap`). Ghidra displays
the Phantom PDB type on the retail programs regardless, so it looks right.
Portable fields on every build: `+0x10 mLabel[32]` `+0x30 mSize` `+0x34 mCount` `+0x38 mGrow`
`+0x3C mUsed`.

`RedTexture::m_sizeInBytes` is `+0x34` on modtools/Phantom but **`+0x2C` on Steam/GOG** (retail
drops `m_szName` and a bitfield). Reading `+0x34` on retail lands in the list node.

See also `GhidraTypeTriage.md` - Phantom is authoritative for NAMES, never for LAYOUT.
