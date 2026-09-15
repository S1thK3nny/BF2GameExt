# Raising the combo animation limit

## Applied in BF2GameExt

`[LimitIncreases] ComboAnimIncrease` now grows the storage as well as the name
list, on modtools, Steam and GOG. On by default.

The old patch handed out indices past 163 without making room for them. An index
could land in the next map, or in the custom fields at the end of this one. The
getter guard stopped the crash by returning NULL, but the animation still could
not play. `entity/combo_anim_limit.cpp` gives those indices their own storage.

| Quantity | Stock | With ComboAnimIncrease |
|---|---:|---:|
| Combo animation names | 30 | 90 |
| Distinct animation-bank names | 16 | 64 |
| Bank/weapon maps | 30 | 90 |
| Combo animation references | 256 | 768 |
| Logical melee indices | 134–163 | 134–223 |
| No-animation sentinel | 164 (`0xA4`) | 224 (`0xE0`) |
| AnimationMap size | `0x4B8` | `0x698` |
| Custom fields, relative to map | `+0x490` | `+0x670` |
| SoldierAnimationData size | `0xF60` | `0x1500` |
| Animation name table bound | 328 | 448 |

The weapon/melee union is unchanged: logical weapon indices 116–133 alias the
first eighteen melee pairs. The unassigned byte `0xFF` is still unassigned, not
another animation. Weapon types stay at 20, low-resolution bank names at 12,
and the temporary map cache at 24. Loaded numbered sub-banks use the separate
array extended by `anim_bank_append.cpp`.

### What moves, and what does not

The class stays `0xF7C0` bytes. Moving its tail was what broke the earlier attempt
below: interior pointers into the collision tables were missed. Instead the DLL
holds 90 maps, 90 collision records and 24 temporary map payloads. The old inline
arrays remain in the object, so unrelated members keep their offsets.

MapCache keeps its original bank/weapon keys at stride `0x4C0`, where the parent
stack still walks them. Only the returned map pointer moves. The supplied-pointer
GetMap translates `this+0x24+map*0x4B8` to a DLL map and preserves parent aliases.
Newly claimed maps get the same `0xFF` fill as stock. The initializer resets DLL
storage first, lets the engine reset its own keys, then drops the cache's stack
pointer on return. Initialization and class rebuilds remain non-reentrant.

The bank registry moves with both of its initializer tables: 64 records of
`0x2C` bytes, a 65-entry bank-order list (including the final -1), and 64×20
bank/weapon cells. Raising AddBank's comparison alone would overrun those stack
tables. Original hashing, name copies, parent links and low-resolution links
stay in use. The installer copies the built-in entries; mission reset still
reduces the bank count to one and the map count to five. Registered map IDs are
checked before they can reach the unchecked skeleton lookup.

LowResClass::PostLoad also had a 256-entry stack list with no bound check. It
deduplicates by `(map, logical animation)`, so `90*224` eight-byte records cover
every possible pair. Its eighteen accesses per build now use DLL storage. The
per-class count reset, final heap allocation and stack frame are unchanged.

### Readers that cannot use the old inline maps

Upper/lower movement, action, weapon and generic getters, plus the custom getter,
all read the new maps. The modtools physical-pair preview getter is replaced too;
retail compiled that path out. Custom writers use `+0x670`, and the final cleanup
walk advances by `0x698`. Collision records keep their 900-byte stride; both
readers and initializer writes are redirected, including the retail write that
computes its address independently of the other anchors.

The jetpack sampler is a separate trap. Modtools `0x0058414D` reads
`[ESI+0xEC]`, which is map 0's upper action 25 (`0x24+25*8`). It bypasses every
getter. Steam `0x00647456` and GOG `0x006484F6` push the same cell instead. These
instructions now read the DLL cell, keeping the original MOV/PUSH behaviour.

Retail callers keep ECX and sometimes EDX live across the original leaf getters.
A C++ calling-convention annotation does not preserve that contract. The naked
shims preserve registers other than the EAX result, flags and XMM0–7, and keep
each original RET argument count. Function and instruction tables are in
`combo_anim_limit.cpp`; capacities and layout assertions are in
`combo_anim_layout.hpp`.

### Installation and guards

The patch core checks every numeric site before calling `patch_set::install`.
That callback checks all storage instructions and hook entries, installs the
hooks in one Detours transaction, and restores storage bytes if commit fails.
Only a successful install permits the numeric limits to change. Retail absolute
operands are rebased for both verification and rollback. These are startup,
process-lifetime patches, like the other array relocations.

The damage guard uses the same expanded lookup and leaves its detours alone.
With the option off, it keeps the stock 164-index/30-map bound. Both paths skip
missing clips; the lower-body fallback is unchanged. See
[ComboDamageResolver.md](ComboDamageResolver.md).

The initialization log gives the live counts and cache peak/failures. The crash
logger also recognizes the modtools lower-movement fault at `0x0057906D`, caller
`0x0057BBE3`, and prints the map, stance and raw slots without faulting on an
unreadable pointer. Startup logs append after one initial truncation, so messages
from separate installer handles do not overwrite one another.

Played on modtools and Steam with 52 names, 253 references, 31 maps and 17 banks;
both logs show a 14/24 cache peak and no cache failures. GOG's original-byte and
native checks pass; live GOG play has not been checked here.

## Historical in-place attempt (reverted)

The remainder is the failed 2026-09-06 approach. Its 120-name target and tail
offsets are not the applied layout above. The custom offset is `0x490` relative
to a map; `0x4B4` includes the class's `+0x24` map-array base.

> **The in-place growth described below was BUILT, TESTED AND REVERTED on 2026-09-06.**
> It crashes instantly. Read "Why this failed" at the bottom before acting on any of the
> site tables here. The derivation and the index-space analysis are still correct and are
> kept for the relocate-based approach; the tail-shift half is a record of a mistake.


The finished half of `[LimitIncreases] ComboAnimIncrease`. The shipped patch set grows the
combo animation *name list* but not the per-map storage those names resolve into, so any
index past 163 reads into the next map's block. See
[docs/RE/ComboDamageResolver.md](ComboDamageResolver.md) for the crash that exposed it.

## The index space

`SoldierAnimatorClass::AnimationMap` (Phantom PDB type record; 1216 bytes there, **1208
shipped**) is a union of four sub-arrays plus a tail:

    mActionAnimation      [38]      pairs   offset    0   (304 bytes)
    mMovementAnimation    [6][13]   = 78    offset  304   (624 bytes)
    mWeaponAnimation      [3][6]    = 18  } aliased  928   (240 bytes)
    mWeaponMeleeAnimation [30]      = 30  } union    928
    mCustomAnimation      uint[10]          offset 1168   (40 bytes) -> 1208

`GetAnimFromAnimIndex` (modtools `0x00588AD0`) decodes it:

    idx <  38  -> mActionAnimation[idx]
    idx < 116  -> mMovementAnimation[(idx-38) % 13]
    idx < 134  -> mWeaponAnimation[(idx-116) % 6]
    idx < 164  -> mWeaponMeleeAnimation[idx-134]
    else           0

38 + 78 + 18 = 134, + 30 melee = **164**, which is why `0xA4` is the sentinel.

**The melee array is the last sub-array and occupies the last range of the index space**, so
it can be lengthened without moving the other three. That is what makes this tractable.

`SoldierAnimatorClass` holds `AnimationMap mMap[30]` at `+0x24`, followed by
`m_aOrdCollTable[30]` (900 each), `mAnimBank[18]`, `mAnimBankCount`, `mJetPackMatrix`,
totalling `0xF7C0`.

## Target

Melee `30 -> 120`, matching the sentinel of `0xFE` (254) the patch set already writes
(254 - 134 = 120). Choosing 90 instead would mean re-patching the sentinel at 25 sites to
`0xE0`, which is more churn.

    AnimationMap      1208 -> 1928 bytes   (0x12E -> 0x1E2 dwords, 0x4B8 -> 0x788 bytes)
    mCustomAnimation  +0x4B4 -> +0x784     (moved by 90 pairs = 720 bytes)
    mMap[30]          delta  +0x5460       (30 * 720 = 21600)
    allocation        0xF7C0 -> 0x14C20

## Verified modtools sites

Found by linear-sweep decode (`scan_sites.py`), not raw byte matching. **Four of these are
invisible to an `IMUL`-only search** and skipping them corrupts memory: three `rep stosd`
map fills and one per-map walk.

### AnimationMap stride, dwords: `0x12E -> 0x1E2`

| site | operand | instruction |
|---|---|---|
| `0x0057DD49` | +2 | `imul edx, edx, 0x12e`  (GetUpperBodyAnimation) |
| `0x0057DD89` | +2 | `imul edx, edx, 0x12e`  (lower-body variant) |
| `0x0057DEA8` | +2 | `imul eax, eax, 0x12e`  (GetCustomAnimation) |
| `0x0057E195` | +1 | `mov ecx, 0x12e` then `rep stosd` (fill one map with -1) |
| `0x0057F35E` | +1 | `mov ecx, 0x12e` then `rep stosd` |
| `0x0057F573` | +1 | `mov ecx, 0x12e` then `rep stosd` |

### AnimationMap stride, bytes: `0x4B8 -> 0x788`

| site | operand | instruction |
|---|---|---|
| `0x00582074` | +2 | `imul edi, edi, 0x4b8`  (SetupBodyMasks map base) |
| `0x00584087` | +2 | `add ebx, 0x4b8`  (per-map walk, bounded by `[0x00ACECF4]`) |

### mCustomAnimation displacement: `0x4B4 -> 0x784`

| site | operand | instruction |
|---|---|---|
| `0x0057DEB0` | +3 | `mov eax, [ecx + eax*4 + 0x4b4]` |

### Tail displacements, all shift by `+0x5460`  — **INCOMPLETE, this is the half that broke it**

`m_aOrdCollTable` `0x8DB4 -> 0xE214`

    0x005287CA +3   lea eax, [eax + ecx + 0x8db4]
    0x0053D3A2 +3   lea eax, [ecx + edx + 0x8db4]

`mAnimBank` `0xF72C -> 0x14B8C`

    0x0057DE56 +2   lea esi, [ebx + 0xf72c]
    0x0057DEF7 +2   lea edx, [ecx + 0xf72c]
    0x0057DF49 +3   mov [ecx + esi*4 + 0xf72c], edi
    0x00580C72 +2   lea edi, [esi + 0xf72c]
    0x00581CC3 +2   lea ecx, [eax + 0xf72c]

`mAnimBankCount` `0xF774 -> 0x14BD4`

    0x0057DE45 +2   mov eax, [ebx + 0xf774]
    0x0057DE71 +2   mov eax, [ebx + 0xf774]
    0x0057DEEB +2   mov esi, [ecx + 0xf774]
    0x0057DF04 +2   mov ebx, [ecx + 0xf774]
    0x0057DF50 +2   inc dword ptr [ecx + 0xf774]
    0x00580C8B +2   mov [esi + 0xf774], ebx
    0x00581CC9 +1   add eax, 0xf774

`mJetPackMatrix` `0xF780 -> 0x14BE0`

    0x00528810 +2   lea eax, [ecx + 0xf780]
    0x00536207 +1   add eax, 0xf780
    0x00537EC3 +2   add ecx, 0xf780
    0x00584170 +2   lea edi, [esi + 0xf780]

### Allocation: `0xF7C0 -> 0x14C20`

    0x005817A9 +1   push 0xf7c0     (SoldierAnimatorClass::Create)

### Melee count

    0x00588A40 +2   cmp eax, 0x1e   (IsWeaponMeleeAnimIndex) -> 0x78 (120)

`ComboAnimIncrease` already sets this one to `0x5A` (90); it has to match the melee array
and the sentinel, so it becomes 120.

## Interaction to watch

`anim_bank_append.cpp` relocates `mAnimBank` to a heap buffer via hooks on
`FUN_0057DEC0` / `FUN_0057DE40`, and several of the `0xF72C` / `0xF774` sites are inside
those hooked functions. The count stays inline, so its displacements still matter. Check
that the Detours trampolines do not straddle a patched displacement.

`SetupBodyMasks` also carries fixed-size stack arrays fused to the bank/weapon caps
(`[16][20]`, stride `0x14`, six sites) and a 24-element `0x4C0`-stride array.

> **Correction, 2026-09-06.** That 24-element array WAS dismissed here as "not AnimationMaps"
> because `0x4C0 != 0x4B8`. It is one. Each element is an 8-byte header followed by an
> AnimationMap: `0x0057F34E` does `imul edx,edx,0x4C0` / `add ecx,edx` / `lea edx,[ecx+8]`
> and then fills `0x12E` dwords at `edx`, and `0x0057E17A` walks it with `add ecx,0x4C0`
> for `0x18` iterations. Any stride change has to grow this array too.

## Tooling

`tools/scan_sites.py <exe> <hex constants...>` linear-sweep decodes `.text` and reports every
instruction whose disp32 or imm32 equals one of the constants. Use it to find stride sites;
do NOT use it to enumerate a struct tail (see below for why that does not work).

## Why this failed

Built as described, 29 sites, every `expected` verified against the exe, 0 mismatches. The
game crashed instantly: `EIP` in unmapped memory, `EAX = ECX = 0xFFFFFFFF`.

The tail scan looked for the four BASE values. Code also reaches **interior** fields of the
trailing members with their own displacements, which never contain the base value:

    0058427f  lea ecx, [eax + 0x8ff4]     ; m_aOrdCollTable[0] + 576
    0058428e  lea edx, [eax + 0x9134]     ; m_aOrdCollTable[0] + 896

Both sit in the same function as two sites that WERE patched. After the shift they point
inside the enlarged `mMap`, so ordnance-collision writes landed on animation pointers, the
reader picked up the `rep stosd` fill value `0xFFFFFFFF`, and the game called through it.

Widening the scan does not rescue this. Catching interior accesses means treating every
displacement in `0x8DB4`..`0xF7C0` as a candidate - 294 hits - and nothing distinguishes an
offset off a `SoldierAnimatorClass*` from one off an unrelated class with a field at the same
number. Verifying the sites you found says nothing about the ones you never found.

## The approach that can be shown correct

Relocate `mMap` into DLL-owned memory and leave `SoldierAnimatorClass` byte-for-byte
unchanged. The tail never moves, so there are **zero** tail sites, and the remaining list is
only the base computations - each identified by what the instruction does, not by what
constant it holds:

- the three getters `0x0057DD40` / `0x0057DD80` / `0x0057DEA0` (tiny leaves, replace wholesale)
- `SetupBodyMasks`: `imul edi,edi,0x4B8` `0x00582074` and `lea edx,[edi+ecx+0x24]` `0x00582084`
- the per-map walk `add ebx,0x4B8` `0x00584087`
- the three `rep stosd` fills - they take a pointer, only the count changes
- the `mCustomAnimation` displacement `0x0057DEB0`

Needs code caves rather than immediates, since the base arrives in `ECX`. Precedent:
`anim_bank_append.cpp` relocates `mAnimBank[18]` for exactly this reason rather than growing
it in place.

### Why the writers do not need to be found (2026-09-06)

A scan for every instruction addressing `[reg + reg*4 + 0x24]` or `[reg + reg*4 - 0x6C]` -
the shape the getters use - finds **only the four getter loads** in the whole `.text`. Nothing
writes a map slot by animation index.

The population path takes a *pointer*: `SetupBodyMasks` computes the base once
(`imul edi,edi,0x4B8` `0x00582074`, `lea edx,[edi+ecx+0x24]` `0x00582084`) and passes it to
`0x0057F520` (via ILT thunk `0x00403044`), which either hands back a cached map or resets the
passed buffer with `rep stosd` and caches it. Everything downstream works off that pointer
with small displacements.

So relocating the storage fixes every write for free. The complete list of things to change
is the base computations plus the three getters plus the allocations - it does **not** include
a writer hunt.

### What still has to be pinned first

Every allocation of an AnimationMap, not just `mMap[30]`. Known so far: `mMap[30]` inside the
`operator new(0xF7C0)`; the 24-element `0x4C0`-stride array corrected above; and whatever
`0x0057F520`'s cache table (`[ecx + ebx*4 + 4]`, `0x0057F544` / `0x0057F58E`) points at. Miss
one and it overflows exactly the way the tail shift did.
