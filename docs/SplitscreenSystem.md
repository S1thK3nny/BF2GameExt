# Splitscreen System (RE notes from Phantom build, PDB-accurate)

Investigated 2026-07-05 on Battlefront2_Phantom.exe (Ghidra :8194, real PDB applied).
All addresses unrelocated. Phantom is release-family, and Steam codegen matches it
byte-for-byte where checked; modtools (debug family) has the same logic with different codegen.

## Architecture

The master variable is **`GameLoop::mNumCameras`** — it IS the number of local players:

- `NetGame::GetNumLocalPlayers()` literally `return GameLoop::mNumCameras;`
- `NetGame::GetLocalPlayer(n)` — valid for `n < mNumCameras`, resolves via `NetComm::GetLocalIndex(n)`
- `NetComm` keeps `netLocalIndex[2]` — the **net layer supports 2 local players** (console splitscreen
  worked in online games), even in the PC build.

Consumers that loop `0..mNumCameras` generically (all still present on PC):
`PauseMenu::Init` (per-player instances, heap block @`0xa97b94` phantom, stride 0x64),
`HUD::GameEvents::Update`, `GuiManager::UpdateAll` / `CollectInputs(viewport, ...)`,
`SpawnManager::InitialUpdate`, `SkyManager::ReadSplitOptions` (sky LOD config for split mode),
`GameState::PreStateInit`, popup/pause handling, hero rules, etc.

### Camera/viewport side — `CameraManager::SetNumCameras(this, n)`

Phantom body @ `0x49a6f0` (thunk `0x401663`), instance ptr @ `0xabce60`.
Called from `ShellLoop::Init`, `LoadDisplay::Render` (temporarily forces 1), etc.

- `n == 1`: one full-screen `RedViewport {0,0,1,1}`, `RedCamera::SetPerspective(mRedCamera[0], 0.7, 120, 0.8377, aspect)`, `FLRenderer::AddView`.
- `n == 2`: **the 2-player branch survives in the PC binary** — two stacked viewports
  `{0,0,1,0.5}` (top) and `{0,0.5,1,0.5}` (bottom), each `SetPerspective(..., aspect*0.5)`,
  two `FLRenderer::AddView` calls, plus per-camera sound `Listener` activation.

BUT: the PC build was **compiled with all per-camera arrays sized [1]**
(`CameraManager` = 536 bytes: `mRedCamera[1]`, `mChaseCamera[1]`, `mFreeCamera[1]`,
`mDeathCamera[1]`, `mMapCamera[1]`, `mListener[1]`, `mListenerPos[1]`, `m_hView[1]`).
The `n==2` branch's `mRedCamera[1]` access therefore lands on **`mChaseCamera[0]`**
(a 12-byte `GameCamera {vfptr, mManager, mView}` — NOT a RedCamera), `m_hView[1]` lands on
`m_uiVisibleIconMask[0]`, `mListener[1]` lands on `mListenerPos[0]`. The constructor also only
allocates ONE of each camera type. → Forcing `mNumCameras=2` on PC **corrupts memory
deterministically**; the branch is dead-but-present out-of-bounds code.

### Input side — `FLInputManager` (the amputated part)

`FLInputManager::s_instance` @ `0xafa280` (phantom), **4444 bytes — compiled with
MAX_LOCAL_PLAYERS = 1**: `m_joysticks[1]` (RawControllerInputs, 3720 B each), `m_inputs[1]`
(ProcessedInputs), `m_rumble[1]`, `m_guiInputs[1]`, `m_HWPort[1]`, `m_HWPort_Full[1]`,
`mActiveControllers[1]`. Console builds size these [4].

- `GetPlayerInputs(n)` (`0x41b61c`) / `GetPlayerJoystick(n)`: `return n==0 ? &array[0] : NULL;` — hard-compiled.
- `SetSecondaryPort` (`0x5c4cc0`): gutted — only accepts port 0 — and has **zero callers**.
  So `mSecondaryPort` (@`0xafb2b8` = s_instance+0x1038) is **always -1** on PC.
- `UnbindController` (`0x5c4e00`) only handles player-index 1 (i.e. releasing the second binding).
- `ScriptCB_GetSecondaryController` returns nil when port < 0 (shell scripts use this to hide split UI).

The **hardware layer below is fine**: `RedInputManager::pcEnumJoystickDevice` (`0x8d9473`)
enumerates up to **10 DirectInput joysticks** (`m_numJoysticks` @ `0x272f8bc`,
`GetNumJoystickPorts` @ `0x8d8e50`). Only the game-side FLInputManager wrapper is single-player.

## The three PC disable gates

1. **`ScriptCB_SetSplitscreen`** clamps the argument to `[1,1]` (console: presumably `[1,2]`).
   Identical CMOVL/CMOVG clamp in Phantom and Steam; same logic in modtools (debug codegen).
2. **`ShellLoop::Init`**: after `SetHotController(-1)`,
   `if (s_instance.mSecondaryPort < 0) GameLoop::mNumCameras = 1;` then
   `CameraManager::SetNumCameras(0)` + `SetNumCameras(mNumCameras)`. Since SetSecondaryPort is
   never called, this always forces 1. (Phantom gate: `CMP [0xafb2b8],0 / JGE` @ `0x742f12`,
   write @ `0x742f1b`.) Also stores `ShellLoop::mLastGameNumCameras` (@`0xc3c924`) for
   `ScriptCB_WasSplitscreen`.
3. **FLInputManager compiled 1-player** (above) — the real hard blocker.

Additionally `ScriptCB_IsHorizontalSplitScreen` hardcodes `false` and
`SetHorizontalSplitScreen` is a no-op on PC (console offered vertical/horizontal split choice;
the PC binary only retains the stacked-horizontal layout in SetNumCameras).

Dev leftovers that bypass gate 1: `ScriptCB_AutoNetJoin` and `ScriptCB_SetNetGameDefaults`
do `if (netNumCameras > 0) mNumCameras = netNumCameras;` — but `netNumCameras` (@`0xb39524`)
has no writers in the shipped binary.

## Address table (ported anchors)

| Item | Phantom (:8194) | Modtools (:8193) | Steam (:8192) |
|---|---|---|---|
| `GameLoop::mNumCameras` | 0x00a9045c | 0x00adb1e8 | 0x007e668c |
| `ScriptCB_SetSplitscreen` | 0x0065e750 | 0x00478a10 | 0x00596b70 |
| clamp write instr | 0x0065e7a1 | 0x00478a5f | 0x00596bc1 |
| `ScriptCB_IsSplitscreen` | 0x0065e810 | 0x00478ab0 | 0x00596be0 (likely, next table entry) |
| `CameraManager::SetNumCameras` | 0x0049a6f0 | — | — |
| `CameraManager::sInstance` (ptr) | 0x00abce60 | — | — |
| `FLInputManager::s_instance` | 0x00afa280 | — | — |
| `…mSecondaryPort` | 0x00afb2b8 (+0x1038) | — | — |
| `FLInputManager::SetSecondaryPort` | 0x005c4cc0 | — | — |
| `FLInputManager::UnbindController` | 0x005c4e00 | — | — |
| `FLInputManager::GetPlayerInputs` | 0x0041b61c | — | — |
| `ShellLoop::Init` secondary gate | 0x00742f12..0x00742f25 | — | — |
| `NetGame::GetNumLocalPlayers` | 0x00690ed0 | — | — |
| `NetGame::GetLocalPlayer` | 0x006908d0 | — | — |
| `ShellLoop::mLastGameNumCameras` | 0x00c3c924 | — | — |
| `netNumCameras` | 0x00b39524 | — | — |
| `RedInputManager::m_numJoysticks` | 0x0272f8bc | — | — |
| `RedInputManager::GetNumJoystickPorts` | 0x008d8e50 | — | — |

Note (per project memory): debug-vs-release struct offsets differ — modtools offsets must be
re-derived, not copied. Steam↔Phantom porting is near-mechanical (same release codegen).

## Porting assessment

**Just unclamping does NOT work.** Patching the clamp + ShellLoop gate would run the game with
`mNumCameras=2`, but:
- `CameraManager::SetNumCameras(2)` executes OOB accesses (corrupts `mChaseCamera[0]`, treats a
  GameCamera as RedCamera, bogus second view) — crash/corruption.
- `FLInputManager::GetPlayerInputs(1)` returns NULL → every player-2 input consumer crashes or no-ops.
- Per-player heap blocks (PauseMenu instances, spawn displays, GuiManager instance) sized for 1.

**Viable DLL strategy** (storage-substitution, like the anim-bank 128-slot relocate):
1. Patch the `ScriptCB_SetSplitscreen` clamp (allow 2) — trivial, gives Lua control.
2. NOP/hook the `ShellLoop::Init` secondary-port gate (or hook it to bind a second joystick and
   set `mSecondaryPort` ourselves).
3. **Replace `CameraManager::SetNumCameras`** with a DLL reimplementation: allocate our own second
   RedCamera/ChaseCamera/DeathCamera/listener set, do the two `FLRenderer::AddView` calls
   (multi-view rendering is generic in FLRenderer), and hook every `CameraManager` accessor that
   indexes per-camera arrays (`GetCamera(i)`, update loop, map mode…) to serve index 1 from DLL
   storage. This is the biggest work item — need to enumerate all per-camera accessors first.
4. **Hook `FLInputManager::GetPlayerInputs/GetPlayerJoystick/GetPlayerGuiInputs`** to return
   DLL-side P2 buffers for index 1, and pump them per frame from `RedInputManager::GetJoystick(1)`
   (replicating RawControllerInputs→ProcessedInputs processing, or calling the existing processing
   routine on our buffer).
5. Per-player heap blocks: either enlarge at their allocation sites (find `new[mNumCameras]`
   callers) or accept missing P2 pause menu/HUD initially.
6. Player-2 character creation should come free via the net-local-player path
   (`netLocalIndex[2]`, SpawnManager loops) once `mNumCameras==2` — verify at runtime.

Open questions for the implementation phase:
- Enumerate ALL `CameraManager` methods touching per-camera arrays (xref sweep on sInstance).
- How `ProcessedInputs` get produced from `RawControllerInputs` (FLInputManager::Update pipeline) —
  needed for step 4.
- PauseMenu/SpawnDisplay/GuiManager allocation sites and capacities.
- Whether HUD scenes (`GuiManager::mInstances`, `mCurViewport`) hard-NULL viewport 1 like the
  UnbindController inline suggests.
