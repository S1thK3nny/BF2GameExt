# BF2 Sound System (Snd)

Reverse-engineering notes on the `Snd` namespace, focused on how a playing sound
is owned, stopped, and retired. Written while fixing the loading screen's harsh
sound cutoff. The concurrency sections at the end were added later, while raising
the number of sounds that can be audible at once.

Field names come from the Phantom build's PDB. Addresses are modtools unless
stated. The `Snd` module's structs are shared between builds: `VoiceVirtual::Stop`
matches Phantom exactly at `+0x6c`, `+0x9c` and `+0xa0`, and `sizeof(Voice)` is
`0x540` in both.

---

## Object model

```
GameSoundControllable      the caller's handle to one playing sound
  mVoiceVirtualHandle u16  +0x00   0 when it owns nothing
  mFlags              u8   +0x02

VoiceVirtual               a logical voice; there are many
  loop flag           bit  +0x34 & 0x10
  state word          u16  +0x6c
  mFlags              u32  +0x9c
  mVoice              ptr  +0xa0   -> Voice, or null when not currently sounding
  loop-restart cb     ptr  +0xc0 / +0xc4

Voice                      a real mixer slot; a fixed pool of 32, stride 0x540
  loop flag           bit  +0x34 & 0x10   (a COPY - see below)
  state word          u16  +0x6c
  mSourceResampler         +0x3d4
  mSourceResamplerLFE      +0x490
```

`Snd::Sound::Play` returns a `VoiceVirtual*`.
`Snd::Sound::VoiceVirtualToVoiceVirtualHandle` maps it to the handle stored in a
`GameSoundControllable`, which is the safe way to check a pointer is still live.

---

## Update chain

| Function | modtools | Notes |
|----------|----------|-------|
| `Snd::EngineBase::Update` | `0x008827b0` | top of the audio tick |
| `Snd::Voice::Update` | `0x0089ee40` | called for all 32 pool voices |
| `Snd::VoiceVirtualManager::Update` | `0x0089deb0` | iterates virtual voices |
| `Snd::VoiceVirtual::Update` | `0x00894c50` | per virtual voice |
| `Snd::StreamSourceMemory::SetLooping` | `0x008a5b70` | writes `mLooping` |

`EngineBase::Update` drives the voice pool with a literal
`for (i = 0; i < 0xa800; i += 0x540)`, which is where the 32-voice pool size and
`sizeof(Voice)` come from.

---

## Stopping a sound

### `GameSoundControllable::Stop(bool hardStop)` - `0x0074d470`

The engine's own teardown, and the function to use:

```c
if (mVoiceVirtualHandle != 0) {
    vv = ReleaseVoice(this);            // handle -> VV, release, clear handle
    if (hardStop) Snd::VoiceVirtual::Stop(vv);
}
mFlags = 0;
```

`Snd::VoiceVirtual::Stop` (`0x00894800`) sets `state = (state & 0xfffa) | 2` on
**both** the VoiceVirtual and its `mVoice`, unlinks it from the manager, fires the
inactive callback and zeroes the sort value. It silences the voice immediately,
mid-waveform - which is audible as a click.

`ReleaseVoice` alone (`hardStop = false`) does **not** stop anything. Its
`Snd::Sound::VoiceVirtualRelease` zeroes the callback pair at `VoiceVirtual+0xc0/
+0xc4` and installs `FireForget::InactiveCallback`, so the voice self-frees once
it becomes inactive - but nothing makes it become inactive.

> **Naming trap.** `voice_virtual_release` in `game_addrs.hpp` (`0x0074d440`)
> decompiles as `Snd::Sound::VoiceVirtualRelease` but is really
> `GameSoundControllable::ReleaseVoice` - it takes the controllable, not a
> VoiceVirtual.

### Retiring without a click

**Clear `VoiceVirtual+0x34` bit `0x10`, then `Stop(hardStop = false)`.**

From `Snd::VoiceVirtual::Update`, which treats the object as `byte**`:

```c
VoiceBase::CopyParameters(vv[0x28], vv, false);   // vv[0x28] == mVoice (+0xa0)
...
if (((uint)vv[0xd] & 0x10) == 0)   // vv[0xd] == byte offset 0x34
    Stop(vv);                       // past the sample end, not looping -> stop
else
    pos -= sampleEnd;               // looping -> wrap
```

With the bit clear the engine stops the voice itself, at the end of the pass it is
already playing, through its own `Stop` path. The sound finishes naturally, and
the `FireForget` callback installed by `ReleaseVoice` frees it afterwards.

> **The flag on `Voice` is a copy, not the source of truth.**
> `Snd::Voice::Update` really does read `Voice+0x34` bit `0x10` and feed it to
> `StreamSourceMemory::SetLooping`:
>
> ```asm
> MOV CL, byte ptr [ESI + 0x34]
> SHR CL, 0x4
> CALL 0x008a5b70            ; SetLooping on [ESI+0x3d4]
> ```
>
> but `VoiceVirtual::Update` calls `VoiceBase::CopyParameters` every tick, which
> pushes the VoiceVirtual's copy back down. Writing to `Voice+0x34` is silently
> undone on the next audio update. A confirmed *read* site is not proof of a
> writable field.

---

## Gain and fading

**There is no per-voice or per-sound gain.** The only `SetGain` methods belong to
`Bus`, `Encoder`, `Decoder` and `SoftOutput`. A sound's gain comes from its
Properties record and is read at Play time (`GameSound::GetGain` reads
`mProps+0x14` for type 1, `mProps+0x20` for type 3).

The only timed fade is `Snd::Bus::Fade`, and it acts on a whole bus:

```c
// Phantom 0x00801710, __thiscall, RET 0xC
void Snd::Bus::Fade(Bus* bus, float duration, float targetGain, float startGain);
//   bus+0x10 = 0                                        elapsed
//   bus+0x04 = (startGain == -1.0f) ? bus+0x14 : startGain
//   bus+0x08 = targetGain
//   bus+0x0c = duration
```

Reachable from Lua as
`ScriptCB_SndBusFade(busName, duration, targetGain [, startGain])`.
`Snd::Bus::FindByHashID` is Phantom ILT `0x00409b33`; `Bus` has `mGain` at `+36`,
`mFinalGain` at `+40`, `mHashID` at `+44`.

So fading a single sound out is not expressible - it would mean fading a bus and
restoring it afterwards. Clearing the loop flag and letting the voice finish is
the better answer for anything that just needs to stop gracefully.

---

## Concurrency: the active list and the voice pool

`Snd::Sound::Play` does not reserve a mixer slot. It builds a `VoiceVirtual`,
links it into the one active list owned by `Snd::VoiceVirtualManager`, and
returns. Layer creation goes down the same path, and every `SoundParameterized`
layer is itself a `VoiceVirtual` with its own entry, so a single logical sound can
hold several. Nothing on that path consults the mixer.

Arbitration happens once per tick, in `VoiceVirtualManager::Update`
(`0x0089deb0`). It walks the active list and either grants an entry a `Voice` out
of a fixed pool or leaves its `mVoice` null. A null `mVoice` is not an error and
not a stop: the entry stays linked, keeps its sort value, and simply makes no
sound this tick.

So the active list's ceiling sits far above the pool's, and the pool is the only
thing deciding how many sounds are audible. Stock, that pool is 32. Instrumented
in a real match, 117-152 VoiceVirtuals were asking for a voice against those 32,
on every engine tick.

### `VoiceVirtualManager` layout

`Snd::EngineBase::smVoiceVirtualManager` is modtools `0x02331170`.

| Offset | Field |
|--------|-------|
| `+0x00` / `+0x04` | the active `VoiceVirtual` list, doubly linked. The manager's own embedded node is the terminator - the walk ends when it arrives back at the manager. Nodes sit at `object+0x94`. |
| `+0x0c` | a second list |
| `+0x18` | the `Voice` list. Nodes sit at `object+0x74`. |
| `+0x1c` | `mNumManagedVoices` |

`VoiceVirtual::mVoice` is `+0xa0`. Null means the entry wants to sound and has no
slot, which is the quantity worth counting.

`mNumManagedVoices` is a **runtime** counter, not a constant: `Open` increments it
once per constructed `Voice` (`0x00882cee`). That is why the scheduler follows a
larger pool with no patch of its own - construct more voices and the number it
arbitrates against grows with them.

> **The node offset is not the object address.** These lists hang off intrusive
> nodes sitting well inside their objects - `object+0x94` for a `VoiceVirtual`,
> `object+0x74` for a `Voice` - and a walk that treats the node as the object
> still yields pointers that dereference cleanly and look ordered. Walking the
> `+0x0c` list with the node taken as `object+0x04` reads `mVoice` from `0x90`
> bytes past the end of each object; it reported 93 bound voices out of a pool of
> 32, then zero once the values were range-checked, and both readings were
> believed before the offset was pinned down. Range-check anything derived from
> these nodes against the pool bounds before believing a count.

### The `Voice` pool

`Snd::EngineBase::smVoices` at `0x02331088` is a *pointer* to the pool, not the
pool. `Open` writes it from an immediate:

```asm
00882c43  MOV dword ptr [0x02331088],0x00edfe18   ; operand at 00882c49
```

so repointing one disp32 relocates the whole pool. Stride is `0x540`, the
`sizeof(Voice)` established above.

Four loops walk the pool by byte offset rather than by index, each bounded by
`0xa800` - the same `32 * 0x540` the update chain uses:

| Loop | modtools |
|------|----------|
| `Open` | `0x00882cf5` |
| `Update` | `0x00882823` |
| `Close` | `0x00882b58` |
| `SetCentrePeakMode` | `0x0088519f` |

All four have to move together; one left behind walks a different pool than the
rest. `smStreamStorage` begins exactly `0xa800` after the stock pool, so the pool
cannot be grown in place - it has to be relocated.

---

## The voice count under EAX

The 32 above is the pool size. Under EAX it is not even the operative number:
`Engine::Open` throws it away and substitutes a figure derived from DirectSound.

`mixConfig` is `0x02339f84`:

| Value | Mode |
|-------|------|
| 1 | Software |
| 2 | DirectSoundHardware (the EAX path) |
| 3 | DirectSoundSoftware |
| 4 | Disabled |

```asm
00886b14  MOV EAX,[0x02339f84]     ; mixConfig
00886b1b  CMP EAX,0x2              ; 2 == DirectSoundHardware
00886b24  MOV EDI,[0x02339f48]     ; dwFreeHw3DStreamingBuffers
00886b2a  ADD EDI,-0x8             ; reserve 8 - and that is the voice count
00886b40  ...                      ; Voice::Initialize, EDI times
00886b56  CMP EDI,0x20             ; fewer than 32 built?
00886b61  MOV EDI,0x20             ; then the rest of the 32-slot pool is
00886b6c  SUB EDI,EBP              ;   finished off by a second loop
```

Nothing between the load and the loop clamps that figure - the `0x20` under it is
the width of the pool, not a ceiling on the count. The count is bounded earlier
instead, by what was written into `0x02339f48` in the first place.

> **`0x02339f48` is not a standalone global.** It is `DSCAPS + 0x40`, a field
> inside the `DSCAPS` struct at `0x02339f08` that `GetCaps` zeroes and fills at
> `0x008865e0..0x008865fe` with `dwSize 0x60`. It therefore starts out holding the
> driver's own report of free hardware 3D streaming buffers, and is then
> **overwritten with the probe's result** before `Open` reads it. Read after
> `Open`, it tells you what the engine managed to allocate, not what the card can
> do.

### The capability probe

`FUN_00894430` creates `DSBuffer`s into a caller-supplied array until either
creation fails or a caller-supplied limit is reached, and returns how many it
made. It is a real capability test, but it is only ever asked a small question.

`Engine::Open` supplies the limit as `gMaxVoices + 8`:

```asm
008866e4  MOV EDX,[ESP+0x12c4]     ; gMaxVoices
008866eb  ADD EDX,0x8              ; + 8 is the probe's limit
```

and a 40-element stack array to match. `gMaxVoices` is `0x00add474`, set from the
command line and clamped to `[8, 32]` at `0x00446a39` / `0x00446a4d` /
`0x00446a56`. So on a machine whose driver reports 129 free hardware 3D buffers
the probe stops at 40, `Open` computes `40 - 8 = 32`, and the pool it fills is 32
slots wide in any case - three self-imposed limits all landing on the same
number.

The array is the awkward part. There is one array, referenced four times, at four
different `ESP` displacements, because `ESP` moves underneath it between the
references:

| Reference | modtools | Instruction |
|-----------|----------|-------------|
| construct | `0x008866b8` | `LEA EAX,[ESP+0x8ac]` |
| probe argument | `0x008866f0` | `LEA EAX,[ESP+0x8a4]` |
| post-probe consume | `0x00886738` | `LEA EDX,[ESP+0x8a0]` |
| destruct | `0x0088678b` | `LEA EAX,[ESP+0x8a8]` |

with the `PUSH imm8` element count appearing twice, at `0x008866b4` and
`0x00886787`. Four displacements for one object is what makes this look simpler
than it is: miss one and the engine constructs, probes, consumes and destroys two
different buffers.

### The software mixer cap, why it does not bite under EAX, and how it is lifted

`SoftOutput` is created in mixConfig 1 **and** 2 (the gate is `0x00886958`), so
its existence says nothing about which path is carrying audio.

`Voice::Initialize` asks it for a mixer input through `GetUnconnectedInput`
(`0x008a0040`), whose search is bounded by `CMP ESI,0x20` at `0x008a0054` - 32
inputs. When that returns `-1` the voice does not fail. It jumps straight to
creating the voice's own DirectSound buffer (`0x0089e2df`), which under EAX is
what carries the audio anyway, so a voice past 32 is still fully audible.

In mixConfig 1 the opposite holds. There `SoftOutput` *is* the mixer, and a voice
that cannot get an input still occupies a pool slot and is still handed out by the
manager - it just produces nothing, which is strictly worse than never having been
granted the slot.

**Widening it.** `SoftOutput` owns four per-input arrays packed back to back
inside itself with no slack:

| Offset | Array | Per input | Stock |
|--------|-------|-----------|-------|
| `+0x0cc` | gain / ramp matrix | `2 * outChannels` ints | `0x200` |
| `+0x2cc` | connection table | 8 bytes | `0x100` |
| `+0x3cc` | packet holders | 8 bytes | `0x100` |
| `+0x4cc` | per-input offsets | 4 bytes | `0x080` |

Every runtime access goes through a pointer the mixer stores, and the addresses
are only ever formed in `SetOutputBufferSize`, as four 6-byte
`LEA r32,[ESI+disp32]` (`0x0089fb3d`, `0x0089fb4c`, `0x0089fb62`, `0x0089fb69`)
next to the count at `0x0089fb3b`. A 5-byte `MOV r32,imm32` plus a `NOP` fits
inside each, so all four relocate to DLL-owned buffers without moving a single
struct offset.

Three constraints, all load-bearing:

- The mix pass clears the per-input bitmap with a `count >> 3` byte loop
  (`0x008980df`..`0x008980fb`), so a count that is not a multiple of 8 leaves the
  top `count mod 8` bits stale. Round the count up - spare inputs simply go
  unconnected.
- The gain matrix's second plane is indexed at `outChannels * count`, recomputed
  from the *current* stored count on every access. Raising the count without
  relocating that matrix writes far past its `0x200` bytes on the first mix pass.
- `GetUnconnectedInput` never consults the stored count, and
  `SoftOutput::ConnectInput` (`0x0089fce0`) is an unchecked two-instruction thunk,
  so `CMP ESI,0x20` is the only gate on how many inputs are handed out and has to
  be raised as well.

The element constructors and destructors are deliberately left alone. They run
over the original in-struct arrays with their own count of 32, which is exactly
right for a region that is 32 entries wide and now goes unused - and it leaves the
two exception-unwind funclets at `0x00a120cf` / `0x00a1210f` correct without
having to patch them in lockstep.

> **Correction to an earlier belief.** `StreamMixer` does not carry a fixed
> `uchar[32]` input array. `FUN_00898f40` is only `obj[0] = ptr; obj[1] = count` -
> a stored pointer and a count. The 32 lives in `GetUnconnectedInput`'s bound and
> in `SetOutputBufferSize`'s `PUSH`, not in the struct.

The renderer is embedded in the voice at `Voice + 0xe8` (`0x0089e326`), which is
how a renderer `this` seen in an audio-path hook is turned back into a voice
index.

### Retail sites (ported 2026-08-21)

Every address below was read from its own image; none was derived by offset from
another build. `Snd::Engine::Open` was identified by its nine-parameter signature
and its eight reads of `smVoices`, the pool bounds by the `0xA800` immediate
(`32 * sizeof(Voice)`, and `sizeof(Voice)` is `0x540` on all three builds), and
`gMaxVoices` by being parameter 7 at the call site *and* by carrying the same
`sscanf` / clamp-to-`[8,32]` shape.

| Site | modtools | Steam | GOG |
|---|---|---|---|
| `Snd::Engine::Open` | `0x00886420` | `0x00731E90` | `0x00732F80` |
| `VoiceManager::Open` | `0x00882C20` | `0x00734400` | `0x007354F0` |
| `gMaxVoices` | `0x00ADD474` | `0x007E68E8` | `0x007E78E4` |
| clamp `CMP` imm8 | `0x00446A4F` | `0x00479E47` | `0x00479E47` |
| clamp value imm32 | `0x00446A5C` | `0x00479E49` | `0x00479E49` |
| pool ptr imm32 | `0x00882C49` | `0x00734428` | `0x00735518` |
| stock pool | `0x00EDFE18` | `0x009D8420` | `0x009D98C0` |
| Open bound | `0x00882CF7` | `0x007344B6` | `0x007355A6` |
| Update bound | `0x00882825` | `0x00734605` | `0x007356F5` |
| Close bound | `0x00882B5A` | `0x00733F39` | `0x00735029` |
| CentrePeak bound | `0x008851A0` | `0x00732C4D` | `0x00733D3D` |
| HW ceil `CMP`/load | `0x00886B58` / `0x00886B62` | `0x00732628` / `0x00732632` | `0x00733718` / `0x00733722` |
| SW ceil `CMP`/load | `0x00886BDA` / `0x00886BE0` | `0x007326AB` / `0x007326AF` | `0x0073379B` / `0x0073379F` |
| probe ctor / dtor count | `0x008866B5` / `0x00886788` | `0x00732146` / `0x0073222B` | `0x00733236` / `0x0073331B` |
| probe array refs | `0x008866B8`, `0x008866F0`, `0x00886738`, `0x0088678B` | `0x00732149`, `0x00732183`, `0x007321D6`, `0x0073222E` | `0x00733239`, `0x00733273`, `0x007332C6`, `0x0073331E` |
| SW voice-count pin | `0x00886BB0` | `0x00732681` | `0x00733771` |

Three differences that would each have silently broken a copied patch:

1. **The upper clamp is a `CMOVG` on retail.** `CMP EAX,0x20 / MOV ECX,0x20 /
   CMOVG EAX,ECX / MOV [gMaxVoices],EAX`, against modtools' `CMP EAX,0x20 / JLE /
   MOV [gMaxVoices],0x20`. Both operands still have to move, but not one byte of
   the encoding is shared.
2. **The probe array is EBP-relative on retail**, so all four references carry the
   *same* displacement (`LEA EAX,[EBP-0x1250]`, 6 bytes) where modtools has four
   *different* ESP-relative ones (7 bytes) because ESP moves underneath it. The
   replacement `MOV EAX,imm32` is 5 bytes either way, so the NOP padding differs.
3. **The software voice count is only 3 bytes on retail** — `MOV EAX,[EBP+0x20]`
   against modtools' 7-byte `MOV EBX,[ESP+0x12C4]`. There is no room for a
   `MOV r32,imm32`, so it is pinned with `PUSH 0x20 / POP EAX` (`6A 20 58`),
   which is exactly 3 bytes and stack-neutral.

Steam and GOG share the command-line clamp addresses exactly, but **not** the
`gMaxVoices` global, and their `Engine::Open` call sites push the two neighbouring
globals in the opposite order — so GOG is not Steam plus a fixed delta. The
`+0x10F0` delta that does hold across the sound engine breaks down completely in
the command-line and data regions.

The pool cannot grow in place on any build: stream storage begins exactly `0xA800`
after it every time (modtools, Steam `0x009D8420 -> 0x009E2C20`, GOG
`0x009D98C0 -> 0x009E40C0`), which is why both the pool and the probe array are
relocated to process-lifetime `VirtualAlloc` commits.

Retail is **untested in play** — only byte-verified.

### VoiceVirtual layout on retail, and a bug this exposed (2026-08-21)

The diagnostic's manager walk was ported to retail only after the offsets were
re-derived from retail's own instruction arithmetic. **They are identical on all
three builds**, and each was cross-checked against Phantom's PDB:

| Offset | modtools | Steam | GOG |
|---|---|---|---|
| node -> object adjust | `-0x94` | `-0x94` | `-0x94` |
| `VoiceVirtual::mVoice` | `+0xA0` | `+0xA0` | `+0xA0` |
| `Mgr::mActiveList` (head) | `+0x00` | `+0x00` | `+0x00` |
| `Mgr::mInactiveList` | `+0x0C` | `+0x0C` | `+0x0C` |
| `Mgr::mVoiceList` | `+0x18` | `+0x18` | `+0x18` |
| `Mgr::mNumManagedVoices` | `+0x1C` | `+0x1C` | `+0x1C` |
| `smVoiceVirtualManager` | `0x02331170` | `0x007E3450` | `0x007E4450` |

The `0x90` vs `0x94` split is real and must not be "tidied": `&mNode` is object
`+0x90`, but `PblListDoubleSorted` links pointers to `Node+4`, so every value you
walk is object `+0x94`.

Three things a porter must not get wrong. **The list terminator is a sentinel
pointer, not NULL** - stop on `p == MGR+0x00`, and never dereference it, because
`MGR+0x00 - 0x94` is not an object. **`mNumManagedVoices` is a live budget**, not
a constant 32: it is 0 before Open and after shutdown, otherwise the command-line
voice count. And **there is more than one VoiceVirtualManager** - `EngineBase::Update`
calls `VoiceVirtualManager::Update` twice, once on a manager reached from the list
at Steam `0x007E3860`, once on `smVoiceVirtualManager` itself.

#### The renderer-to-voice map was wrong on every build

`sound_diag.cpp` turned a `DSBufferRenderer*` back into a pool slot with
`voice = renderer - 0xE8`, on the stated belief that the renderer is embedded at
`Voice + 0xE8`. **It is not.** Phantom's PDB gives `Snd::Voice` offset 232 (`0xE8`)
as `StreamRenderer mRenderer`, size 748 (`0x2EC`); `DSBufferRenderer` is a
different class of 416 (`0x1A0`) bytes. So every voice index this diagnostic ever
printed was garbage. It never crashed only because the result was range-checked
against the pool and almost always rejected.

The replacement does not subtract at all. It **scans the pool and matches
pointers**, accepting either arm of the conditional in
`StreamRenderer::ConnectInternal` (heap-allocated, pointer at StreamRenderer
`+0xF8`; or constructed in place at `+0xFC`). A match is proof; a wrong
assumption degrades to "unknown" instead of a confident wrong index, which is
precisely the failure mode that shipped before.

`DSBufferRenderer::UpdateGain` and `WriteData` are Steam `0x0073E490` /
`0x0073EB00` and GOG `0x0073F580` / `0x0073FBF0`. UpdateGain takes its float on
the **stack** at `[EBP+0xC]` (`RET 8`) on retail - it is NOT the XMM0 private
convention that applies to `ControllerManager::Update`; do not generalise that
one. `mGain` is `+0x2C` and the Q15 gain pair `+0x190`/`+0x198` on all builds.

---

## The EAX crackle - investigated, not found in BF2

Symptom: a random, loud, distorted burst during matches with EAX enabled. Seven
candidates were raised and all seven are eliminated. Five fell to static analysis.
The two that survived on paper were both conditional on runtime state, so they
were settled by instrumentation instead. Recorded here so nobody repeats the work.

| Candidate | modtools | Outcome |
|-----------|----------|---------|
| `DSBufferRenderer::UpdateGain` converts gain to Q15 as `(int)(gain * 32767.0f) << 16` with no clamp, so any gain above 1.0 would push bit 15 into the sign bit | `0x008997c0` | **Refuted at runtime.** 483,253 calls, every one of them on the unclamped path, and the gain product never exceeded 1.0 - maximum seen exactly 1.000. The same measurement showed flags bit `0x10` is never set, so the float gain path beside it is unreachable dead code. |
| `StreamResampler::GetPacket` publishes `mOutputPacket.mBufferUsed` in **samples** on the unity-rate path but in **bytes** on the interpolating path, while consumers read bytes | `0x008a59a8` / `0x008a5a99` | **Real, but harmless.** `mOutputPacket` is refcount-protected (`0x008a58b8`, `INC word ptr [EDI+0x34]`) and `GetPacket` returns NULL rather than overwrite a held packet. Measured 7,084 unity-rate calls, zero of them with an input packet held. The feared `WriteData` runaway is refuted too: maximum packet cursor 110,544 against a packet size of 110,592 across 1.88 million writes, so the exact-equality release does fire. |
| Five earlier candidates | - | Refuted by static analysis before any instrumentation was written. |

Underneath both of those the output itself was watched theory-free, for runs of
samples pinned at full scale. There were none. BF2's own output never comes close:
the loudest single sample all session was 9,829 of 32,767, about -10.5 dBFS.

**Conclusion.** No mechanism inside BF2 that could produce a loud burst survives.
A machine reporting 129 free hardware 3D buffers is by definition not on native
DirectSound - Windows Vista and later report zero - so a DirectSound wrapper is in
the path, and the wrapper is the remaining suspect. This is a conclusion about
where not to look inside BF2, not a fix.

---

## Applied in BF2GameExt

`loading_screen/lifecycle.cpp` owns every sound it starts, one-shots included, in
a `GameSoundControllable` so they stay reachable. `snd_ctrl_retire()` clears the
VoiceVirtual loop bit and releases ownership; `loading_screen_stop_all_sounds()`
runs on every exit path out of the loading screen so no loop can survive into the
match. The pointer is validated against the handle before any write, so a recycled
voice is never touched.

`util/voice_limit.cpp` raises the concurrency ceiling, on modtools only. It has to
move all of it together: `gMaxVoices` (so the probe's limit follows for free), the
probe's array (relocated off the stack into a DLL buffer, because enlarging a
`0x1298`-byte stack frame would mean re-deriving every ESP-relative offset in a
2300-byte function), the `Voice` pool and its four loop bounds, and both ceilings.
`[LimitIncreases] VoiceLimit` is 0 for stock 32, otherwise 33 to 119. The 119 is an
encoding limit rather than a preference: the probe's array count is a `PUSH imm8`
carrying `N + 8`, and the voice ceilings are sign-extended `imm8` compares.
The software mixer is widened to match, rounded up to a multiple of 8; if any of
its sites fails verification the software branch is pinned back to 32 rather than
handing out voices that cannot reach a mixer input. Every site is checked against its expected bytes before anything is written
and one mismatch disables the whole feature, so it cannot half-apply. It costs
about 1.4 KB per voice. It also needs EAX live and a DirectSound layer with
hardware buffers to spare - native Windows Vista and later report zero hardware 3D
buffers, so in practice that means a wrapper such as DSOAL, Creative ALchemy or
IndirectSound. Measured at `VoiceLimit=119` against a driver reporting 129 free
buffers: the probe asked for 127 and got 127, `mNumManagedVoices` reached 119,
peak demand was 113, and all 113 were bound.

`util/sound_diag.cpp` is the instrumentation the voice-pressure, gain, cursor and
output numbers above came from. `[Diagnostic] SoundDiagnostic`, off by default,
modtools only. It reports the voice ceiling actually in force and how many sounds
are dropped for want of a voice, and watches the PCM leaving the engine for runs
pinned at full scale, logging them with a timestamp and voice index. It hooks
`Snd::EngineBase::Update`, `DSBufferRenderer::UpdateGain` and
`DSBufferRenderer::WriteData` and nothing else, so the unity-rate `GetPacket`
counts are not its work.
