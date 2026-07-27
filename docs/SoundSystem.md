# BF2 Sound System (Snd)

Reverse-engineering notes on the `Snd` namespace, focused on how a playing sound
is owned, stopped, and retired. Written while fixing the loading screen's harsh
sound cutoff.

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

## Applied in BF2GameExt

`loading_screen/lifecycle.cpp` owns every sound it starts, one-shots included, in
a `GameSoundControllable` so they stay reachable. `snd_ctrl_retire()` clears the
VoiceVirtual loop bit and releases ownership; `loading_screen_stop_all_sounds()`
runs on every exit path out of the loading screen so no loop can survive into the
match. The pointer is validated against the handle before any write, so a recycled
voice is never touched.
