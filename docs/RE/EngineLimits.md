# Engine limits: reservations, command posts, skinning

Three fixed limits investigated 2026-08-20. Addresses are modtools unless stated.

---

## 1. `ReserveManager::sList` — the 60-entry reservation pool

> **Shipped** as `[LimitIncreases] ReservationPoolSize` (default 127), implemented in
> `PatcherDLL/src/ai/reservation_pool.cpp`. All eight retail immediates below were re-read
> from the images before the patcher was written and all held `0x3C`.

The source of `List pool is full; raise count from 60 to at least 2129`.

`ListPool<ReserveManager::ReserveStruct,int>`, capacity `0x3C` = 60, element 24
bytes. Holds AI reservation claims: `RT_REPAIR`=0, `RT_BOARD`=1, `RT_ATTACK`=2,
`RT_FORMATION`=3, `RT_HeavyWeapons`=4. It is the **only** ListPool in the game with
capacity 60 — the full set is {5, 16, 16, 20, 20, 32, 50, **60**, 100, 1200, 1200}.

| Build | pool ptr | ctor | Init |
|---|---|---|---|
| phantom | `0x00bfda6c` | — | `0x0073a800` |
| modtools | `0x00b8f414` | `0x005c60f0` | `0x005c6200` |
| steam | `0x01eaeff8` | `0x006300e0` | `0x0062f550` |
| gog | `0x01eb04ac` | `0x00631180` | `0x006305f0` |

**Overflow is fail-safe.** `Append` is
`mPeak++; if (mLength != mPoolSize) { store; mLength++ } else { RedWarning; return }`.
The element is dropped; nothing is written out of bounds. The guard is `JZ`
(equality), not `JB` — `3B C1 / 74 12` at `0x00593d2d`. So the cost of overflow is
lost AI reservations and log spam, not corruption.

### "2129" is not a demand figure

`mPeak` is incremented **before** the capacity test and **decremented on every
removal** (`UnreserveObject`, `Unreserve`, `UnreserveReserver` — modtools
`0x005c70f0` / `0x005c6fe0` / `0x005c71e0`). It is not a high-water mark.

A dropped reservation never enters the list, so no Unreserve ever cancels its
increment, and `Reserve` retries the same claim next frame because `IsReservedBy`
stays false. **`mPeak - mLength` is net rejected adds accumulated since level
load — it measures how LONG the pool has been starved, not how much is wanted.**
Sizing the pool to 2129 would be sizing to a stopwatch. It resets per level load
(`ReserveManager::Init` <- `AIUtil::Init` <- `PreStateInit`).

### The patch

**modtools — one byte.** Verified in place:

```asm
005c620c  74 0F          JZ   0x005c621d
005c620e  6A 3C          PUSH 0x3C          ; imm8 at 005c620f
005c6210  8B C8          MOV  ECX,EAX
005c6212  E8 56B3E4FF    CALL 0x0041156d
005c6217  A3 14F4B800    MOV  [0x00b8f414],EAX
005c621c  C3             RET
005c621d  C7 05 ...      MOV  dword [0x00b8f414],0
005c6227  C3             RET
005c6228  CC x24         padding, to 0x005c623f
005c6240                 next function
```

`6A ib` is **sign-extended**, so the in-place ceiling is `0x7F` = 127. One byte
suffices because the out-of-line ctor `0x005c60f0` derives everything from the
single argument: `LEA EAX,[ESI+ESI*2]` / `LEA ECX,[EAX*8+4]` for the allocation,
`MOV [EDI+0xC],ESI` for mPoolSize, `MOV [EAX],ESI` for the array cookie.

**Never write 0x80-0xFF.** Sign-extension makes ESI negative, the allocation
request becomes ~4 GB, `operator new[]` returns NULL, and the ctor stores
`mElements = NULL` — but `mPoolSize` was already written negative. The first
`Reserve` then writes to NULL.

Above 127 needs a 31-byte in-place rewrite to `PUSH imm32`. The 24 bytes of `0xCC`
at `0x005c6228` are confirmed free (no function there; next real one is
`0x005c6240`), so no code cave is needed.

**Retail needs FOUR sites, not one.** The ctor is constant-folded there and the
caller's pushed count is **dead** (`PUSH ECX` on an uninitialised register at steam
`0x0062f581`). Porting the modtools one-byte patch to retail by editing "the push"
is a silent no-op.

Steam: imm32 `0x006300fd`, imm32 `0x0063010e`, imm8 `0x00630147` (max 0x7f),
imm32 `0x0063014d`. GOG: `0x0063119d`, `0x006311ae`, `0x006311e7`, `0x006311ed`.
`MOV EDX,0x18` and `PUSH 0x18` nearby are element SIZE — do not touch.

> **Trap:** Phantom `0x0073a827` is `74 3C`, a JZ displacement, not a capacity. A
> blind byte-replace of `3C` across Init corrupts control flow.

### Measured outcome, and the other pools (2026-08-21)

**The warning string is shared by all ELEVEN `ListPool<T>` instantiations**, so the capacity
printed in it is the only thing identifying which pool overflowed. Measured on a live Steam
session before and after the raise:

| Log | from capacity 60 | from capacity 5 |
|---|---|---|
| pre-fix | **1998** | 40 |
| post-fix | **0** | 2 |

So the reservation pool was ~98% of the flood and is now silent; a capacity-5 pool remains and
fires twice during level load (peak 6 then 7 = two rejected adds), interleaved with texture
loading and command-post setup.

**How to identify any of them cheaply.** The pools are passed as `this` rather than being named
globals, so static identification is awkward — but each template instantiation is compiled
separately and passes its OWN compile-time string to `RedWarning::SetLogData`, even though every
one reports the same `ListPool.h(92)`:

| `Append` (modtools) | element size | timestamp |
|---|---|---|
| `0x005C62A0` (ReserveManager) | 0x18 | `15:23:19` |
| `0x005D8660` | 0x0C | `15:23:11` |
| `0x007448D0` | 0x04 | `15:23:17` |
| `0x00497EA0` | 0x04 | `15:24:23` |

The other seven are `0x00593D00`, `0x005C8640`, `0x005C8869`, `0x00497FB0`, `0x00597770`,
`0x00744D40`, `0x00744DB0`. One hook on `SetLogData` that records the timestamp whenever the
file is `ListPool.h` would name any overflowing pool exactly, without reverse-engineering each.

### Cost

Memory is trivial (60x24+4 = 1444 B). Frame time is the real cost: every query is a
linear scan on `mLength`, and `Reserve` calls `IsReservedBy` first, so insertion
alone is O(n). 60 to 127 is cheap; 60 to 2129 would be ~35x the scan work for
nothing. `ReserveManager::Reset` is a bare `global = 0` that leaks the pool, so
raising capacity scales a per-load leak (1444 B -> 3052 B at 127).

**Replay/journal divergence:** `ReserveOffset` feeds `PblJournal::Record`, so which
reservations survive changes journaled formation offsets. Patched and unpatched
builds diverge in replay playback.

---

## 2. Command posts: 16 is a wire format, not an array bound

**Not raisable for multiplayer.** Three independent caps land on 16, and the
decisive ones are in the network event `REL_CHANGECOMMANDPOSTTEAMS`:

| Field | Width | Per post |
|---|---|---|
| `mTeamBits` | `WriteBits(pkt, x, 0x20)` | **2 bits** — 16 posts fills a uint32 exactly |
| `mAliveBits` | `WriteBits(pkt, x, 0x10)` | **1 bit** |
| post reference | `WriteBits(pkt, mPostIndex, 4)` | a **4-bit index** |

modtools: Read `0x006ee0c0`, Write `0x006f9880`, Handle `0x006ee100`
(`83 7D F8 10` at `0x006ee124`), `ReadCommandPost` `0x006efee0` (`6A 04` at
`0x006eff0c`).

Post 16 writes bit 32 of a 32-bit field; x86 masks the shift by `&0x1f`, so it
**silently aliases onto post 0**. No crash, just corrupted ownership. Widening the
fields changes packet length, and there is no version negotiation on this event, so
a patched host and a stock client desync immediately. `ReadCommandPost` has no
bounds check, so widening the index without widening the array is a wild pointer
driven by network data.

Storage is two static `CommandPost*[16]`, adjacent with zero slack — modtools
`0x00b93b58` (client) / `0x00b93b98` (host), with `sActivePostCount` immediately
after. Steam and GOG share `0x01e308a0` / `0x01e308e0`.

`CommandPost::BuildPost` (modtools `0x0064fdf0`) checks `83 FF 10` at `0x0064ff13`,
logs "Exceeded %d command posts!" and then **writes out of bounds anyway**. Retail
stripped the check entirely. So post 17 on a host already overwrites the
`sActivePostCount` pointer today.

A single-player-only path exists but is gated by `HUD::ElementMap`, which embeds
`mPost[16]`, `mPostScale[16]`, `mPostText[16]` and `mPostSelect[16]` **inline** at
fixed offsets (126080, 130176, 131776, 136528) with literal displacements baked into
the accessors. Growing them shifts every member above offset 126080 — a structural
rewrite, not a byte patch.

---

## 3. Skinning: the limit is 15 bones per SEGMENT, not 32

The premise "32 bones" conflates two unrelated limits.

**The GPU limit is 15 per mesh segment.** The bone palette starts at vertex shader
constant **c51**, each bone is `SetMatrix4x3` = **3 float4 registers**, and the
engine's shadow register file is a compile-time `[96]` array:

```
pcRedVertexShaderConstants {
   pcRedVertexShaderConstantRegister m_Registers[96];  // 1536 B
   int m_iMinConstantSet;   // +0x600
   int m_iMaxConstantSet;   // +0x604
}                            // size 1544
```

`(96 - 51) / 3 = 15`. Confirmed by address arithmetic on all four builds — palette
start to array end is exactly `0x2D0` = 720 B = 45 registers:

| Build | base | palette (c51) | min | max |
|---|---|---|---|---|
| phantom | `0x00aa7238` | `0x00aa7568` | `0x00aa7838` | `0x00aa783c` |
| modtools | `0x00ae2898` | `0x00ae2bc8` | `0x00ae2e98` | `0x00ae2e9c` |
| steam | `0x007de348` | `0x007de678` | `0x007de948` | `0x007de94c` |
| gog | `0x007df348` | `0x007df678` | `0x007df948` | `0x007df94c` |

The engine targets **vs_1_1**, which has exactly 96 constants. `internalSetupCaps`
(phantom `0x0087ff80`) falls back to `SKINNING_SOFTWARE` unless
`MaxVertexShaderConst > 0x5F`. 32 bones would need 96 registers — the entire file,
leaving nothing for view-projection (c2-c5), camera (c6), fog (c7), object-to-world
(c16-c18), ambient (c19-c20), lights (c21-c32), material (c39-c41). 64 bones would
need 192.

The upload immediates are patchable but useless: every one of them just walks the
write off the end of a fixed 96-entry array. Lowering c51 to buy headroom collides
with everything above AND desyncs the exe from every skinned shader in every shipped
mod, since the vs_1_1 bytecode in `core.lvl` reads the palette at c51 and shaders
are assets (`normal_shader.xml`, `zprepass_shader.xml`), not exe-resident.

**The "32" is `ZephyrSkeleton<32>`** — the animation system's template parameter for
total joints, unrelated to shader constants.

### The munger does the split, so 15 is NOT a character limit

The content pipeline (munger) splits a mesh into segments each referencing at most
15 bones. Nothing in the exe clamps it — but nothing needs to, because assets never
arrive unsplit. Confirmed by the map author, and consistent with the absence of any
runtime check.

**Therefore the shader constant file does not limit character complexity.** A
64-joint character simply munges into more segments; the palette is uploaded per
draw call, not per character. The cost is draw calls, not correctness. Any reasoning
of the form "32 bones would need all 96 registers, so 32 joints is impossible" is
wrong — that arithmetic applies to a single segment, which the munger guarantees is
never that large.

The real character ceiling is `ZephyrSkeleton<32>`. See below.

### Latent hazard, for hand-built assets only

`RedRenderer::pcRenderPrimitive` (`0x00882ca0`) stores
`numBoneMatrices = (ushort)param_7` raw from `RedSegment::m_uiNumBones` with no
bounds check, and the dirty-range high-water is `numBoneMatrices * 3 + 50`. Bone
index 15 writes c96/c97/c98 — straight into `m_iMinConstantSet` and
`m_iMaxConstantSet`.

So a segment with 16+ bones **silently corrupts engine state before D3D ever
rejects the oversized `SetVertexShaderConstantF`**. For munged assets this is
unreachable. It only matters for anything hand-built that bypasses the pipeline.

### The actual blocker: `SoldierAnimator`'s layout

`ZephyrSkeleton<64>`, `ZephyrPoseStatic<64>`, `ZephyrPoseDyn<64>` and
`ZephyrAnimInst<64>` **already ship and are already used**, by `EntityWalker`
(`0x005914f0`) and `EntityTrap` (`0x0058e510`). The animation half of a 64-joint
character exists today.

What stops characters using it is `SoldierAnimator` (8240 bytes), which embeds all
four `<32>` objects **by value**, from the PDB layout:

| Offset | Size | Member |
|---|---|---|
| 88 | 4 | `PblBitVector<32> mUpperBodyAnimMask` |
| 92 | 4 | `PblBitVector<32> mLowerBodyAnimMask` |
| 208 | 2064 | `ZephyrSkeleton<32> mZephyrSkeleton` |
| 2272 | 900 | `ZephyrPoseStatic<32> mZephyrPoseStatic` |
| 3172 | 2480 | `ZephyrPoseDyn<32> mZephyrPoseDynUpper` |
| 5652 | 2480 | `ZephyrPoseDyn<32> mZephyrPoseDynLower` |
| 8132+ | — | everything else |

Two independent 32s, and both must move together:

1. **Layout.** Switching to `<64>` grows the object by roughly 7808 bytes and shifts
   every member above offset 208. Those displacements are baked as literals across
   thousands of instructions. This is a structural rewrite, not a byte patch.
2. **Bitmask width.** `mUpperBodyAnimMask` / `mLowerBodyAnimMask` are
   `PblBitVector<32>` — 4 bytes, **one bit per joint**. Joint 32 aliases onto joint
   0 exactly the way command post 16 aliases onto post 0. Widening them is part of
   the same rewrite, and missing it would silently mis-mask the upper/lower body
   split rather than fail loudly.

The renderer is NOT a blocker — see the munger note above.

Others: `ZephyrJointSet` is `{uchar m_puJoints[32]; uchar m_puSkinSets[16]; uchar
m_uNumJoints; uchar m_uNumSkinSets}` — a true 32 ceiling with a uchar count.
`ZephyrJointShared::m_iParent/m_iChild/m_iSibling` are **signed char** with a -1
sentinel, giving a 127-joint hierarchy cap. `RedModel::Render` (`0x008993d0`) has a
stack array `PblMatrix* [129]` memset with `numBones << 2`, so >128 bones smashes
the stack. `AllocBoneMatricesInCache` (`0x00875d60`) guards at `0xBF5` = 3061.

---

## Unverified

- Retail `ReserveManager::Init` was located positionally (the call after
  `HintManager::Initialize` inside `AIUtil::Init`), not by symbol. The ctor body
  (element `0x18`, count `0x3c`) can only be this ListPool, but single-step it once
  before writing to retail.
- The claim that only one ListPool has capacity 60 rests on an enumeration of all
  eleven instantiations. Confirm the first `%d` in the warning prints 60 before
  patching, since the format string carries no pool name.
- Whether `sValidateTimeStamp` makes `GetReserveOrder` O(n^2) or early-outs was
  disputed and not settled. It does not affect the 60 to 127 recommendation.
