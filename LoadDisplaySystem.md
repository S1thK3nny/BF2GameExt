# BF2 LoadDisplay System

Everything reverse-engineered about SWBF2's (BF2_modtools exe) loading screen
subsystem.

---

## Overview

`LoadDisplay` is the class responsible for managing the loading screen. It owns
the backdrop texture, the progress bar (`ProgressIndicator`), the loading-text
blink animation, and the render callback chain that draws everything each frame.

One global `LoadDisplay` instance lives for the duration of a level load. It is
created by `LoadDisplay::Create` inside `GameState::PreStateInit`, driven by
`ScriptInit` (via Lua), and torn down by `LoadDisplay::End`.

---

## Class Layout (known fields)

All offsets are relative to the `LoadDisplay*` (`ecx`).

| Offset | Type | Name / Purpose |
|--------|------|----------------|
| `+0x00` | `uint8_t` | **Active flag.** Non-zero while LoadDisplay is alive. `End()` zeroes this after freeing textures. Calling `Render()` with this zeroed → crash on freed D3D resources. |
| `+0x14c0` | `uint32_t` | **Backdrop hash.** Hash of the texture drawn as the random backdrop. Passed directly to `PlatformRenderTexture`. Zeroing this suppresses the draw (PRT skips hash == 0). |
| `+0x14cc` | `uint32_t` (QPC low) | **Blink timer.** Written by `Update()` to drive the "Loading..." text blink animation. |
| `+0x15f8` | `int` | **Loaded texture count.** Incremented as lvl chunks are processed. Readable before/after `LoadDataFile` to count how many textures a file contributed. |
| `+0xd30` | — | **`ProgressIndicator` sub-object.** Passed as `ecx` to `SetAllOn`, `ProgressIndicator::Update`, etc. |

---

## Globals

| Symbol | Address | Notes |
|--------|---------|-------|
| `s_loadHeap` | `0x00ba111c` | Heap index saved by `LoadDisplay::Create` at the time of creation (= TempLoadHeap = 3 normally). `Update()` calls `_RedSetCurrentHeap(s_loadHeap)` before rendering. |
| QPC stamp | `0x00ba2f60` | Low word of QPC counter. Written by `Update()` **only** when the 50 ms throttle passes and `Render()` is actually called. Readable before/after `g_orig_load_update` to detect whether Update rendered. |

---

## Functions

### `LoadDisplay::Create`

Called from `GameState::PreStateInit` (`0x0044f600`) while
`__RedCurrHeap = TempLoadHeap`. Stores the current heap index into `s_loadHeap`
(`0x00ba111c`). This is why `s_loadHeap` normally equals 3 (TempLoadHeap).

---

### `LoadDisplay::LoadDataFile` — `0x0067e2b0`

```c
// __thiscall (mirrored as __fastcall)
void LoadDisplay::LoadDataFile(const char* lvlPath);
```

Full pipeline: `MakeFullName` → open `PblFile` → `ChunkProcessor`. Processes
all `tex_` chunks in the lvl and registers them in the global texture hash table
(`0x00d4f994`, size `0x2000`). The texture count at `ecx+0x15f8` increments for
each texture registered.

---

### `LoadDisplay::LoadConfig` — `0x0067c650`

```c
// __thiscall (mirrored as __fastcall)
void LoadDisplay::LoadConfig(uint32_t* fileHandle);
```

Parses the level's `LoadConfig` block from a `PblConfig` stream. Known parsed
keys and their scopes are handled by `hooked_load_config` to populate the
`Bf1LoadExt` config struct.

---

### `LoadDisplay::Update` — `0x0067c1d0`

```c
// __thiscall (mirrored as __fastcall)
void LoadDisplay::Update(void* ecx, void* edx);
```

**QPC-throttled at 50 ms.** Internal sequence:

```
_RedSetCurrentHeap(s_loadHeap)     saves previous, sets to s_loadHeap (= 3)
if (now - lastQPC >= 50ms):
    Render()                       draws the frame; writes QPC stamp
    DAT_00ba2f60 = QPC_low         stamp written ONLY when Render fires
ProgressIndicator::Update(…)
_RedSetCurrentHeap(savedHeap)      RESTORES previous heap (TempLoadHeap)
```

Key points:
- `Update()` always saves and restores `__RedCurrHeap`. After it returns,
  `__RedCurrHeap` is back to whatever it was before the call (TempLoadHeap in
  the loading loop).
- At most 20 renders/second (50 ms gate).
- The QPC stamp at `0x00ba2f60` is a reliable way to detect from outside whether
  `Update()` actually rendered this call: read it before, call `Update()`, check
  if it changed.

---

### `LoadDisplay::Render` — `0x00402b71` (thunk)

```c
// __thiscall (mirrored as __fastcall)
void LoadDisplay::Render(void* ecx, void* edx);
```

**Not throttled.** Thin wrapper that calls into `PlatformRender()`, which:

1. Sets up camera/lighting.
2. Calls the `RenderScreen` callback (`LoadDisplay::RenderScreen` or our hook).
3. Tears down render state.

`PlatformRender()` may allocate MemoryPool slabs and SortHeap buffer space from
`__RedCurrHeap` before the callback fires.

---

### `LoadDisplay::RenderScreen` — `0x0067a1b0`

```c
// __thiscall (mirrored as __fastcall)
void LoadDisplay::RenderScreen(void* ecx, void* edx);
```

The per-frame draw function. In vanilla BF2 the only thing it draws is the
`RandomBackdrop` texture stored at `ecx+0x14c0`, using a single full-screen
`PlatformRenderTexture` call. Everything else (progress bar, loading text) is
drawn by `PlatformRender()` itself or by other render callbacks.

Hooked by `hooked_render_screen` to:
- Suppress the vanilla backdrop draw in BF1 mode (temporarily zeros the hash).
- Draw the BF1 overlay elements on top.

---

### `LoadDisplay::End` — `0x0067de10`

```c
// __thiscall (mirrored as __fastcall)
void LoadDisplay::End(void* ecx, void* edx);
```

Called from Lua (`ScriptCB_ShowLoadDisplay(false)`) when all assets have loaded.
Zeroes the active flag at `ecx+0x00`, frees D3D textures. After this call,
calling `Render()` or accessing texture resources → crash.

Called via the thunk at `0x0041451f` (unconditional JMP), which is what we hook.

---

### `LoadDisplay::ProgressIndicator::SetAllOn` — `0x0040786f`

```c
// __thiscall (mirrored as __fastcall); ECX = LoadDisplay* + 0xd30
void ProgressIndicator::SetAllOn(void* ecx, void* edx);
```

Instantly fills every segment of the progress bar to 100%. Used in
`hooked_load_end` before the BF1 end-animation spin-loop to show a full bar
during the zoom-out sequence.

---

## Loading Loop Call Chain

```
GameState::PreStateInit (0x0044f600)
  ├─ BuildHeaps                     (TempLoadHeap created)
  ├─ _RedSetCurrentHeap(TempLoadHeap)
  ├─ LoadDisplay::Create            ← s_loadHeap = 3
  └─ _RedSetCurrentHeap(RunTimeHeap)

FUN_00734040 (level load body)
  ├─ MemoryPool::Setup × many
  ├─ LuaHelper::CallGlobalProc("ScriptInit")
  │     ↳ Lua drives loading; calls ScriptCB_ShowLoadDisplay(true/false)
  │     ↳ Repeatedly calls Update() from engine tick (every engine frame)
  ├─ LoadDisplay::End()
  └─ GameMemory::ReleaseTempHeap()
```

`ScriptInit` runs for the entire duration of asset loading. During this window
`__RedCurrHeap = TempLoadHeap`; the engine does not reset it between ticks.

---

## `PlatformRenderTexture` — `0x004165fe` (thunk → `0x006d0650`)

```c
// __stdcall — 15 arguments
void PlatformRenderTexture(
    uint32_t texHash,
    float x0, float y0, float x1, float y1,   // normalized 0.0–1.0 screen coords
    void*    colorPtr,                          // global render-state ptr (0xae2150)
    int      alphaBlend,
    float    u0, float v0, float u1, float v1, // UV: identity = (0,0,1,1)
    float    r, float g, float b, float a       // always (1,1,0,0) for standard draws
);
```

- Coordinates are normalized: `(0,0,1,1)` = full screen. Confirmed from
  disasm: the game's own `RenderScreen` pushes `0x3f800000` (= `1.0f`) for
  the right edge of its full-width backdrop draw.
- Skips the draw silently when `texHash == 0`. This is used to suppress the
  vanilla backdrop draw by temporarily zeroing `ecx+0x14c0`.
- Internally calls `PblHashTableCode::_Find` (`0x007e1a40`) on the global texture
  table (`0x00d4f994`, size `0x2000`) to look up the texture by hash.
- Pushes a command onto the SortHeap for deferred sorted rendering.

---

## Texture System

### Global texture table

| Symbol | Address | Notes |
|--------|---------|-------|
| `tex_hash_table` | `0x00d4f994` | Pointer to the flat hash table |
| Table size | `0x2000` | Passed to every `_Find` call |

### `PblHashTableCode::_Find` — `0x007e1a40`

```c
// __cdecl
void* _Find(void* tablePtr, uint32_t tableSize, uint32_t hash);
```

Returns a pointer to the found entry, or NULL if the hash is not registered.
Used inside `PlatformRenderTexture` (observed call at `0x006d07ea`).

### `HashString` — `0x007e1b70`

```c
// __cdecl — inner function (no ECX indirection)
uint32_t HashString(const char* str);
```

The game's own FNV-1a case-insensitive hash. Must be used instead of any
custom implementation to guarantee hashes match what the lvl loader stored
during `tex_` chunk processing. The `__thiscall` wrapper at `0x007e1bd0`
stores the result via ECX; the inner function at `0x007e1b70` simply returns
it in EAX — correct for direct calls.

---

## Hooks Applied by `bf1_load_ext`

| Function | Hook type | Purpose |
|----------|-----------|---------|
| `LoadDisplay::LoadDataFile` (`0x0067e2b0`) | Detour | Log texture counts; inject second lvl load for BF1 custom textures |
| `LoadDisplay::LoadConfig` (`0x0067c650`) | Detour | Parse `LoadConfig` block for BF1Ext config (EnableBF1, textures, sounds, etc.) |
| `LoadDisplay::RenderScreen` (`0x0067a1b0`) | Detour | Suppress vanilla backdrop; draw BF1 overlay elements |
| `LoadDisplay::End` (`0x0067de10`) | Detour | Delay teardown until BF1 end animation completes |
| `LoadDisplay::Update` (`0x0067c1d0`) | Detour | Inject extra render calls (up to ~30 fps); redirect s_loadHeap |
| `LoadDisplay::Render` (`0x00402b71`) | **Not hooked** — called directly | Used directly to inject frames at controlled times |
| `ProgressIndicator::SetAllOn` (`0x0040786f`) | **Not hooked** — called directly | Called once at the start of the `hooked_load_end` spin-loop |

---

## `hooked_load_update` — Behaviour

```
if (g_inRealEnd) return   ← End() is tearing down; Update() must not run

redirect s_loadHeap → RunTimeHeap
call g_orig_load_update(ecx, edx)
restore s_loadHeap

if (bf1Enabled && orig rendered naturally):
    record g_lastRenderMs

else if (bf1Enabled && ≥33 ms since last render && LoadDisplay still active):
    switch __RedCurrHeap → RunTimeHeap
    g_orig_load_render(ecx)
    restore __RedCurrHeap
```

The `g_qpc_stamp` (`0x00ba2f60`) read before/after `g_orig_load_update` detects
whether Update's internal 50 ms throttle fired. If it did, `Update()` already
rendered; we skip the injected call to avoid double-renders.

The 33 ms gate caps injected renders at ~30 fps. Combined with Update's 20 fps
ceiling this gives a net frame rate of at most ~30 fps during asset loading.

---

## `hooked_load_end` — Behaviour

```
if (g_endProcessed) return   ← prevent double-End() crash

if (bf1Enabled):
    g_orig_set_all_on(ecx + 0xd30)     ← fill progress bar to 100%
    play exit sound
    spin-loop until zoom animation done:
        every 200 ms:
            redirect s_loadHeap → RunTimeHeap
            g_orig_load_update(ecx)     ← advance blink timer
            restore s_loadHeap
        every 33 ms:
            switch __RedCurrHeap → RunTimeHeap
            g_orig_load_render(ecx)     ← draw frame
            restore __RedCurrHeap
        Sleep(1)                        ← yield CPU

g_endProcessed = true
g_inRealEnd = true
g_orig_load_end(ecx, edx)              ← real teardown
```

The `g_inRealEnd` flag prevents `hooked_load_update` from injecting any more
renders while the real `End()` is freeing D3D resources.

---

## BF1 Overlay Elements

Drawn inside `hooked_render_screen`, using `PlatformRenderTexture` with
normalized screen coordinates. Draw order (back to front):

1. **Backdrop** — one of `backdropHashes[]`, chosen by current level index.
2. **Animated texture** — one of `animHashes[]`, selected and alpha-blended
   over time to create a looping animation.
3. **Planet image** — per-level `PlanetEntry` (hash + position + size),
   matched to level index.
4. **Zoom selector crosshair** — tiled from three textures (horizontal strips,
   vertical strips, corners), scaled by `ZoomSelectorTileSize`.
5. **Scan lines** — full-screen overlay (`scanLineTexHash`), drawn last/on top.

---

## `PblConfig` Parsing API

Used by `hooked_load_config` to read the level's `LoadConfig` block.

| Function | Address | Signature |
|----------|---------|-----------|
| `PblConfig::PblConfig(fh)` | `0x00821000` | ctor, RETN 4 |
| `PblConfig::PblConfig(parent, share)` | `0x00821080` | copy ctor, RETN 8 |
| `PblConfig::ReadNextData(buf)` | `0x008210f0` | writes hash/argc/args into buf, RETN 4 |
| `PblConfig::ReadNextScope(buf)` | `0x00821140` | enters next scope, returns buf ptr, RETN 4 |

---

## Sound Integration

| Function | Address | Signature |
|----------|---------|-----------|
| `Snd::Sound::Properties::FindByHashID(hash)` | `0x0088c500` | `__cdecl(uint32_t) → Properties*` |
| `Snd::Sound::Play(entity, props, p3, p4, p5)` | `0x0088cc10` | `__cdecl` |

Sounds are played by hash: `FindByHashID` → `Snd::Sound::Play(0, props, 0, 0, 0)`.
Used for the BF1 exit-sequence sound triggered at the start of `hooked_load_end`.
