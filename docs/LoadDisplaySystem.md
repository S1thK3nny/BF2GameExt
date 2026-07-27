# BF2 LoadDisplay System

Everything reverse-engineered about SWBF2's loading screen subsystem.

Field names in this document come from the Phantom build's real PDB
(`LoadDisplay`, 65 fields, 7504 bytes). Addresses default to the modtools build
unless stated.

---

## Overview

`LoadDisplay` is the class responsible for managing the loading screen. It owns
the backdrop texture, the progress bar (`ProgressIndicator`), the loading-text
blink animation, and the render callback chain that draws everything each frame.

One global `LoadDisplay` instance (`LoadDisplay::sInstance`) lives for the
duration of a level load. It is created by `LoadDisplay::Create` inside
`GameState::PreStateInit`, started by `LoadDisplay::Begin`
(`ScriptCB_ShowLoadDisplay(true)`), driven by `ScriptInit` via Lua, and torn down
by `LoadDisplay::End` (`ScriptCB_ShowLoadDisplay(false)`).

---

## Class Layout

The modtools and retail (Steam/GOG) layouts are **identical to each other**, and
both equal the Phantom layout with a constant `-1872` (`0x750`) shift applied to
everything from `m_progressBar` onward. Fields up to and including `m_tipsBox`
are unshifted. The entire divergence is one struct: `BorderedBox m_tipsBox` is
608 bytes on modtools/Steam versus 2480 on Phantom.

Verified 2026-07-27 at six independent points on modtools disassembly
(`PostLoad` 0x67bd50, `LoadDataChunk` 0x67dea0, `RenderScreen` 0x67a1b0).

| Phantom | modtools / Steam / GOG | Type | Name / Purpose |
|--------|------|------|----------------|
| `+0` | `+0x00` | `bool` | **`m_bDisplay`.** Set true by `Begin()`, false by `End()`. Every `Update()` early-returns while it is false. Not a liveness flag: the object outlives it. |
| `+4` | `+0x04` | `uint32_t` | **`m_missionHash`.** Mission script name truncated at the first `_` **and** with the last character stripped (`geo1c_con` gives `geo1`). Matched against `World()`/`Map()` scope names. |
| `+8` | `+0x08` | `uint32_t` | **`m_entireMissionHash`.** Full mission script name (`geo1c_con`). Also matched against `World()`/`Map()`. |
| `+2416` | `+0x970` |  | **`m_groupLoadingTips`.** Screen group holding the tips box and text. Positioned at screen-relative (0.35, 0.18). |
| `+5248` | `+0xd30` |  | **`m_progressBar`.** The `ProgressIndicator` sub-object. `ecx` for `SetAllOn` / `ProgressIndicator::Update`. |
| `+5456` | `+0xe00` |  | **`m_groupTopLeft`.** Holds `m_textMissionName`. |
| `+5616` | `+0xea0` |  | **`m_groupTopRight`.** Holds `m_textModeName`. |
| `+5776` | `+0xf40` |  | **`m_groupBottomLeft`.** Holds `m_textLoading` and `m_progressBar`. |
| `+5936` | `+0xfe0` |  | **`m_groupBottomRight`.** Holds the four team-icon models. |
| `+7184` | `+0x14c0` | `uint32_t` | **`m_backDropHash`.** Texture drawn as the backdrop. Passed straight to `PlatformRenderTexture`; zeroing it suppresses the draw (PRT skips hash == 0). |
| `+7196` | `+0x14cc` | `float` | **`m_tipTimer`.** Counts *down* by `dt`; on reaching 0 it reloads `m_timePerTip` and picks a new random tip. **Not** the blink timer. |
| `+7200` | `+0x14d0` | `float` | **`m_totalTime`.** Accumulated load time. Drives the blink: `sin(m_loadingTextBlinkRate * 2pi * m_totalTime)`. |
| `+7212` | `+0x14dc` | `RedModel*[10]` | **`m_models`.** See the array-overflow warning below. |
| `+7252` | `+0x1504` | `RedTexture*[50]` | **`m_textures`.** |
| `+7452` | `+0x15cc` | `RedSkeleton*[10]` | **`m_skeletons`.** |
| `+7492` | `+0x15f4` | `int` | **`m_numModels`.** |
| `+7496` | `+0x15f8` | `int` | **`m_numTextures`.** Readable before/after `LoadDataFile` to count how many textures a file contributed. |
| `+7500` | `+0x15fc` | `int` | **`m_numSkeletons`.** |

`ProgressIndicator` (208 bytes, same on all builds, offsets relative to its own base):

| Offset | Type | Name |
|--------|------|------|
| `+144` | `RedBitmapElement*` | `m_pLED` |
| `+148` | `float*` | `m_pIntensity` (per-LED intensity, **not** alpha) |
| `+152` | `RedColor*` | `m_pColor` |
| `+156` | `int` | `m_curOnLED` |
| `+160` | `int` | `m_progressBarDirection` |
| `+164` | `uint32_t` | `m_LEDTexHash` |
| `+172` | `int` | `m_numLEDs` |
| `+176` | `float` | `m_OnLEDFraction` |
| `+180` | `float` | `m_LEDSpeed` |
| `+192` | `LEDModeT` | `m_mode` |

> **Ghidra hazard.** The modtools program (`BF2_modtools_MemExt.exe`) currently has
> Phantom's `LoadDisplay` struct applied to it. It is wrong for everything after
> `m_tipsBox`: modtools `RenderScreen` decompiles the backdrop hash as
> `m_progressBar.field_0x40`. Read raw displacements from disassembly, not field
> names, when working on `LoadDisplay` in that program.

---

## Globals

| Symbol | Address | Notes |
|--------|---------|-------|
| `s_loadHeap` | `0x00ba111c` | Heap index saved by `LoadDisplay::Create` at the time of creation (= TempLoadHeap = 3 normally). `Update()` calls `_RedSetCurrentHeap(s_loadHeap)` before rendering. |
| `prevTicks` (QPC stamp) | `0x00ba2f60` | Low word of the QPC counter. Written by `Update()` when the 50 ms throttle passes and `Render()` fires, **and also** by the `m_firstUpdate` branch, which sits outside the gate. Readable before/after `g_orig_load_update` to detect whether Update rendered, with one false positive on the first `Update()` after `Begin()`. |
| `LoadDisplay::sInstance` | see `Create` | The single global instance. `ScriptCB_ShowLoadDisplay` calls `Begin`/`End` on it. |
| `sTipsDatabase` / `sLVLDatabase` | - | `IDDatabase` globals. Both are re-initialised by `Begin()`, so tips do not accumulate across level loads. |
| `RedModel::pc_shareBuffers` | mt `0x00ae2454` | Cleared for the duration of `LoadData`. |

---

## Functions

### `LoadDisplay::Create`

Called from `GameState::PreStateInit` (`0x0044f600`) while
`__RedCurrHeap = TempLoadHeap`. Stores the current heap index into `s_loadHeap`
(`0x00ba111c`). This is why `s_loadHeap` normally equals 3 (TempLoadHeap).

---

### `LoadDisplay::LoadData` - mt `0x0067e360`

Zeroes the three counts, switches to `__RedTempHeap`, clears
`RedModel::pc_shareBuffers` (mt `0x00ae2454`), then calls
`LoadDataFile(this, "Load\\load")`. The `"Load\\load"` literal is the operand our
`SetLoadDisplayLevel()` Lua function repoints.

If `m_randomBackDropFileName` is set (the `RandomBackDrop` config key), it then
calls `LoadRandomLVLChunk`, which enumerates the `lvl_` children of that .lvl,
collects their chunk IDs into `sLVLDatabase`, picks one at random, and loads only
that single chunk. Its ID becomes `m_backDropHash`. This is the engine's native
lazy "pick one backdrop of N" path.

---

### `LoadDisplay::LoadDataFile` - `0x0067e2b0`

```c
// __thiscall (mirrored as __fastcall)
void LoadDisplay::LoadDataFile(const char* lvlPath);
```

`MakeFullName` then `PblFile::Exists` then `PblFile::Open` then
`LoadDataChunk` on the root chunk.

---

### `LoadDisplay::LoadDataChunk` - mt `0x0067dea0`, steam `0x005776e0`

Walks the chunk's children and dispatches by four-CC:

| Chunk | Action |
|-------|--------|
| `modl` | `m_models[m_numModels++] = RedModel::Read(...)` |
| `tex_` | dedup against existing entries, then `m_textures[m_numTextures++] = RedTexture::Read(...)` |
| `skel` | `m_skeletons[m_numSkeletons++] = RedSkeleton::Read(...)` |
| `load` | `LoadConfig(...)` |

> **Engine bug: no bounds checks.** All three appends are unguarded, on every
> build. The arrays are contiguous (`m_models[10]`, `m_textures[50]`,
> `m_skeletons[10]`, then the three counts), so overflow order is
> models into textures, textures into skeletons, skeletons into `m_numModels`.
> An 11th skeleton overwrites a count field, after which `DeleteData` iterates a
> garbage `m_numTextures` calling `~RedTexture` and `operator delete` on
> non-objects.
>
> A modder reaches this simply by adding art to `load.lvl`. BF2GameExt installs
> a bounds guard for it: see `loading_screen/data_guard.cpp`.

---

### `LoadDisplay::LoadConfig` - `0x0067c650`

```c
// __thiscall (mirrored as __fastcall)
void LoadDisplay::LoadConfig(uint32_t* fileHandle);
```

Parses the level's `LoadConfig` block from a `PblConfig` stream.
`hooked_load_config` re-runs the same stream afterwards to pick up the BF1-ext
keys documented in [user/LOADING_SCREEN.md](user/LOADING_SCREEN.md).

#### Stock keys, `LoadDisplay { }` scope (hash `0x8689c861`)

| Hash | Key | Target |
|------|-----|--------|
| `0x93c0f4f7` | `BackDrop(texName)` | `m_backDropHash` |
| `0x97d04fa8` | `RandomBackDrop(lvlPath)` | `m_randomBackDropFileName`, max 63 chars |
| `0x664a1f44` | `SunColor(r, g, b)` | `m_sunColor` |
| `0xe1907cf6` | `SunDirection(x, y, z)` | `m_sunDirection`, normalized on read |
| `0x627ebb4a` | `AmbientColor(r, g, b)` | `m_ambientColor` |
| `0xd6c2b5f9` | `TeamModel(teamName, modelName)` | `m_modelTeamIcon[teamNum]` |
| `0x26455a06` | `TeamModelRotationSpeed(f)` | `m_teamModelOmega` |
| `0x55c13372` | `TipsTime(seconds)` | `m_timePerTip` |
| `0x6c30ed87` | `TipsBoxTexture(tex, borderW, borderH)` | `m_tipsBox` |
| `0xd1eb1b5a` | `TipsColor(r, g, b [, a])` | `m_textLevelTips` colour |
| `0x31a6bc76` | `ProgressBarTotalTime(seconds)` | `m_totalProgressTime`, default 10 |
| `0xee4ccfc3` | `ProgressBarNumLEDs(n)` | `m_numLEDs`, default 20 |
| `0x533f1e37` | `ProgressBarLEDColor(r, g, b [, a])` | `m_LEDColor` |
| `0x0925227b` | `ProgressBarLEDTexture(texName)` | `m_LEDTextureHash` |
| `0xc53b850c` | `ProgressBarLEDWidth(f)` | `m_LEDWidth`, default 5 |
| `0x836ca5fb` | `ProgressBarLEDHeight(f)` | `m_LEDHeight`, default 10 |
| `0x4d649461` | `ProgressBarLEDSpacing(f)` | `m_LEDSpacing`, default 5 |
| `0xdfe9346c` | `LoadingTextBlinkRate(hz)` | `m_loadingTextBlinkRate`, default 1.0 |
| `0xa6fb2870` | `LoadingTextColorPallete { }` | sub-scope, 16 entries max |
| `0x3d7e6258` | `Color(r, g, b [, a])` inside that scope | `m_loadingColorPallete[]` |
| `0x904a07ed` | *(name unresolved)* | global `LED_OFF_INTENSITY`, default 0.5 |
| `0xc5ad880f` | *(name unresolved)* | tips file, feeds `AddTipsToDatabase` |

#### Stock keys, `World(name)` / `Map(name)` scope

Hashes `0x37a3e893` and `0xdfa2efb1`. Treated identically by the engine: the name
argument is hashed and matched against `m_missionHash` **or**
`m_entireMissionHash`, and the body is only applied on a match.

Accepts `BackDrop`, `RandomBackDrop`, `ProgressBarTotalTime`, `TipsTime`, plus
`CampaignLayout` (`0x001de6b9`) and `CampaignName` (`0xd169eaf8`). The tips-file
key (`0xc5ad880f`) also works here and does an `IDDatabase::RemoveAll` first, so
a per-map tips list replaces rather than appends.

Hash values are FNV-1a, case-insensitive:
`h = 0x811c9dc5; for c in s: h = (h ^ tolower(c)) * 0x01000193`.

---

### `LoadDisplay::PostLoad` - mt `0x0067bd50`

Adds every screen group to `m_loadingScreen`, positions them
(tips at 0.35/0.18, title bar at 0.5/0.0, the four corner groups at the corners),
assigns the two teams' icon models, and initialises the progress bar:

```c
ProgressIndicator::Init(&m_progressBar, m_numLEDs, m_LEDSpacing,
                        &m_LEDTextureHash, 1, &m_LEDColor, 1,
                        (float)m_numLEDs / m_totalProgressTime,   // m_LEDSpeed
                        LED_MODE_TIMER);
```

> **The progress bar is a timer, not a progress bar.** It fills `m_numLEDs` LEDs
> over `m_totalProgressTime` wall-clock seconds (20 LEDs over 10 s by default)
> and reads nothing about actual load state. If the level loads faster the bar is
> mid-way when `End()` slams it full; if it loads slower the bar sits full and
> waits.
>
> `ProgressIndicator::Update` also implements `LED_MODE_WRAP`, `LED_MODE_RANDOM`
> and a ping-pong mode (via `m_progressBarDirection`), but `PostLoad` is the only
> caller of `Init` and it always passes `LED_MODE_TIMER`.

The team icons are given colour, scale, spin (`m_OmegaY = ±m_teamModelOmega`) and
positions (±100 in `m_groupBottomRight`), then have their visibility setter called
with 0. `TeamModel` and `TeamModelRotationSpeed` are parsed but nothing turns the
elements on.

---

### `LoadDisplay::Update` - `0x0067c1d0`

```c
// __thiscall (mirrored as __fastcall)
void LoadDisplay::Update(void* ecx, void* edx);
```

**QPC-throttled at 50 ms.** Actual internal sequence:

```
if (PblJournal is reading or writing) return
if (g_bNoRender) return
if (!m_bDisplay) return

if (m_firstUpdate):
    prevTicks = QPC()              <-- stamp written HERE too, outside the gate
    m_firstUpdate = false

dt = (QPC() - prevTicks) / freq    dt accumulates across skipped ticks

if (dt >= 0.05):
    saved = _RedSetCurrentHeap(s_loadHeap)
    prevTicks  = QPC()
    m_totalTime += dt
    m_tipTimer  -= dt
    if (m_tipTimer <= 0):          reload m_timePerTip, pick a new random tip,
                                   resize m_tipsBox to the new text
    ScreenTransition::sInstance->Update(dt)
    blink = sin(m_loadingTextBlinkRate * 2pi * m_totalTime) * 0.5 + 0.5
    m_textLoading colour = lerp across m_loadingColorPallete[] by blink
    ProgressIndicator::Update(&m_progressBar, dt, NULL, 2)
    Render()
    _RedSetCurrentHeap(saved)
```

Key points:
- The heap save/restore pair and `ProgressIndicator::Update` are **inside** the
  50 ms gate, not around it. On a tick where the gate does not fire, `Update()`
  touches nothing at all. Any `s_loadHeap` redirection by a hook therefore only
  has an effect on gate-fire ticks.
- At most 20 renders/second (50 ms gate).
- The QPC stamp global (`prevTicks`, `0x00ba2f60`) is *mostly* a reliable way to
  detect from outside whether `Update()` rendered: read it before, call
  `Update()`, check if it changed. **Caveat:** it is also written by the
  `m_firstUpdate` branch, which is outside the gate. That produces exactly one
  false positive per loading screen, on the first `Update()` after `Begin()`.
- `Begin()` ends with two back-to-back `Update()` calls. Both fail the 50 ms
  gate (`dt` is ~0), so the first frame is not painted until the load loop has
  been running for 50 ms.

---

### `LoadDisplay::Render` - `0x00402b71` (thunk)

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

### `LoadDisplay::RenderScreen` - `0x0067a1b0`

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

### `LoadDisplay::End` - `0x0067de10`

```c
// __thiscall (mirrored as __fastcall)
void LoadDisplay::End(void* ecx, void* edx);
```

Called from Lua (`ScriptCB_ShowLoadDisplay(false)`) when all assets have loaded.

```
if (PblJournal is reading or writing) return
if (g_bNoRender) return
ProgressIndicator::SetAllOn(&m_progressBar)   fills the bar to 100%
Render()                                      one last frame with the full bar
m_loadState = LOAD_STATE_FINISHED
DeleteData()                                  frees m_textures[], clears m_skeletons[]
m_bDisplay = false
PblProfile::SetPaused(s_profilePaused)        restores the pre-Begin profiler state
```

After this call, calling `Render()` or accessing texture resources gives a crash
on freed D3D resources.

Note that `End()` already performs `SetAllOn` + `Render` itself, so a hook that
wants a full bar during a delayed teardown does not need to call `SetAllOn` again
unless it renders *before* handing off to the original.

`DeleteData` frees the `m_textures[]` entries but leaves `m_models[]` untouched.
Both are allocated on `__RedTempHeap`, which is released wholesale at level exit,
so the models are reclaimed there rather than leaked.

Called via the thunk at `0x0041451f` (unconditional JMP), which is what we hook.

---

### `LoadDisplay::ProgressIndicator::SetAllOn` - `0x0040786f`

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

## `PlatformRenderTexture` - `0x004165fe` (thunk → `0x006d0650`)

```c
// __stdcall - 15 arguments
void PlatformRenderTexture(
    uint32_t texHash,
    float x0, float y0, float x1, float y1,   // normalized 0.0-1.0 screen coords
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

### `PblHashTableCode::_Find` - `0x007e1a40`

```c
// __cdecl
void* _Find(void* tablePtr, uint32_t tableSize, uint32_t hash);
```

Returns a pointer to the found entry, or NULL if the hash is not registered.
Used inside `PlatformRenderTexture` (observed call at `0x006d07ea`).

### `HashString` - `0x007e1b70`

```c
// __cdecl - inner function (no ECX indirection)
uint32_t HashString(const char* str);
```

The game's own FNV-1a case-insensitive hash. Must be used instead of any
custom implementation to guarantee hashes match what the lvl loader stored
during `tex_` chunk processing. The `__thiscall` wrapper at `0x007e1bd0`
stores the result via ECX; the inner function at `0x007e1b70` simply returns
it in EAX - correct for direct calls.

---

## Hooks Applied by `bf1_load_ext`

| Function | Hook type | Purpose |
|----------|-----------|---------|
| `LoadDisplay::LoadDataChunk` (`0x0067dea0`) | Detour (loop reimplemented) | Bounds-check the `m_models[10]` / `m_textures[50]` / `m_skeletons[10]` appends. `loading_screen/data_guard.cpp` |
| `LoadDisplay::LoadDataFile` (`0x0067e2b0`) | Detour | Log texture counts; inject second lvl load for BF1 custom textures |
| `LoadDisplay::LoadConfig` (`0x0067c650`) | Detour | Parse `LoadConfig` block for BF1Ext config (EnableBF1, textures, sounds, etc.) |
| `LoadDisplay::RenderScreen` (`0x0067a1b0`) | Detour | Suppress vanilla backdrop; draw BF1 overlay elements |
| `LoadDisplay::End` (`0x0067de10`) | Detour | Delay teardown until BF1 end animation completes |
| `LoadDisplay::Update` (`0x0067c1d0`) | Detour | Inject extra render calls (up to ~30 fps); redirect s_loadHeap |
| `LoadDisplay::Render` (`0x00402b71`) | **Not hooked** - called directly | Used directly to inject frames at controlled times |
| `ProgressIndicator::SetAllOn` (`0x0040786f`) | **Not hooked** - called directly | Called once at the start of the `hooked_load_end` spin-loop |

---

## `hooked_load_update` - Behaviour

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

## `hooked_load_end` - Behaviour

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

1. **Backdrop** - one of `backdropHashes[]`, chosen by current level index.
2. **Animated texture** - one of `animHashes[]`, selected and alpha-blended
   over time to create a looping animation.
3. **Planet image** - per-level `PlanetEntry` (hash + position + size),
   matched to level index.
4. **Zoom selector crosshair** - tiled from three textures (horizontal strips,
   vertical strips, corners), scaled by `ZoomSelectorTileSize`.
5. **Scan lines** - full-screen overlay (`scanLineTexHash`), drawn last/on top.

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
