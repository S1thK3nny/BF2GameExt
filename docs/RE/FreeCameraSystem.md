# FreeCamera - RE notes

Engine internals for the free camera and its attached light. For how to actually *use* the
commands, see the Free camera section of [RedConsoleCommands.md](RedConsoleCommands.md).

Addresses are unrelocated (imagebase `0x400000`).

## Build availability

This is the first thing to check before porting anything here.

| | modtools | Steam | GOG |
|---|---|---|---|
| `FreeCamera` class + `Update` | yes | yes | yes |
| `ScriptCB_Freecamera` (Lua) | yes | **yes** | **yes** |
| `debugmenu.ToggleFreeLook` | yes | no | no |
| `FreeCameraStop` / `InvertAxis` / `QuakeMode` | yes | no | no |
| `SnapCamera` | yes | no | no |
| `debugmenu.SetFreecamTarget` and the other three | yes | no | no |
| `freecamlight.*` | yes | no | no |

The retail builds ship the whole `RedCommandConsole` debug command table stripped: none of
the command name strings survive, and neither does `"Unrecognized Command"`. What *is* still
registered and working on retail is the Lua side - `ScriptCB_Freecamera`,
`ScriptCB_DoConsoleCmd` and `ScriptCB_GetConsoleCmds` are real bodies in the Lua binding
table, not stubs. Steam's `ScriptCB_Freecamera` (`0x0057F850`) is the same shape as
modtools': `lua_gettop`, `GameState::GetState`, compare against `0x11E1FC01`, then
`PauseMenu::SetFreeCamera`. So the free camera itself is fully usable on retail; only the
console-side toggles are gone.

The freecam **light** is dead code on retail. `SetFreeCamLightCallback` was stripped with its
four command registrations, and it is the only thing that ever allocates the light, so
`s_pFreeCamLight` can never become non-null. Counting direct callers of the
`RedOmniLight` constructor confirms it: modtools has 8, Steam has 7, and the missing one is
the freecam callback. The positioning code still sits inside `FreeCamera::Update` on retail
(Steam calls `RedOmniLight::SetPosition` at `0x0052DD21`) but is permanently gated off.

## Class layout

`FreeCamera` is one of five cameras `CameraManager` owns (`mChaseCamera`, `mFreeCamera`,
`mDeathCamera`, `mMapCamera`, plus the `mRedCamera` they all feed). 112 bytes:

| Offset | Field |
|--------|-------|
| +0x10 | `mMatrix` |
| +0x50 | `mPitch` |
| +0x54 | `mYaw` |
| +0x58 | `mMultiplier` (move speed) |
| +0x5c | `mTurnMultiplier` |
| +0x60 | four button-edge bools |
| +0x64 | `mVelocity` |

`FreeCamera::Update(this, float dt)` is vtable slot 1: modtools `0x004AE1B0`, Steam/GOG
`0x0052D7B0`. `hover_springs.cpp` and `weapon_ranges.cpp` already hook it as their
freecam-time draw tick.

`FreeCamera::SetMatrix` back-solves `mPitch = asin(-m[2][1])` and `mYaw = atan2(...)` from
the incoming matrix, which is why entering the free camera does not snap the view.

## Entering and leaving

| Route | Chain | Effect |
|-------|-------|--------|
| `debugmenu.ToggleFreeLook` | `JToggleFreeLook` -> `CameraManager::FreeLook` | Camera only. Leaves `GameLoop::sFreeMode` false. Exits to chase camera in game state `0x8FF60339`, map camera otherwise. |
| `ScriptCB_Freecamera` | -> `PauseMenu::SetFreeCamera` -> `_SetFreeCamera` | Pops the pause screen, `Enable(false)`s all ten HUD displays (Status, Reticule, StatIcon, Capture, Team, Message, Boot, HeroMessage, ChangeClass, Spectator), then `GameLoop::SetFreeMode(true)` -> `FreeLook`. |

`GameLoop::sFreeMode` has exactly three references in the binary and exactly one consumer:
`SnapCamera`.

## Per-frame update

`FreeCamera::Update` in order:

1. Fetch `GUIInputs` and `ProcessedInputs` for the viewport. **Returns immediately if
   `GUIInputs` is null.**
2. Stash the current position for the velocity calculation.
3. Follow/tether. If following, it builds a look-at basis, calls `SetMatrix`, and **returns
   early** - which is why manual input is dead while follow is on.
4. Guarded by `!GameLoop::IsGameOver() && !sStop`, the three control helpers below.
5. `mVelocity = (newPos - oldPos) / dt` when `dt > 0`. This feeds `GetVelocity`, which the
   sound listener reads.
6. The collision debug ray riders.
7. The freecam light.

### Control helpers

| Helper | Reads | Tunables |
|--------|-------|----------|
| `UpdateControlSpeed` | GUI inputs `0x16`/`0x18` (move speed), `0x17`/`0x19` (turn speed); threshold 0.125; edge-triggered on release | `mMultiplier` and `mTurnMultiplier`, `*= 1.25` / `*= 0.8`. Unclamped, never reset. Gated behind `AIDebug::AllowUpdateFreeCamSpeeds()`, which is a hardcoded `return true`. |
| `UpdateOrientation` | `RawControllerInputs::mRawInputFloat[0x40]`/`[0x41]` (struct offsets `+0x110`/`+0x114`) - the **raw** look axes, not the processed aim axes, so mouse sensitivity and aim tuning do not apply | `sTurnScale` = 1.5 (modtools `0x00A90344`). No pitch clamp, so the camera can pitch past vertical. |
| `UpdateTranslation` | processed `mFloatInputs[0]`/`[1]` (the same move axes the soldier walks on), plus GUI inputs `0x12`/`0x13` for altitude | `sMoveScale` = 15.0 (`0x00A90340`), `sHeightScale` = 3.0 (`0x00A90348`) |

`sQuakeControl` selects the basis handed to `D3DXVec3TransformNormal`: `mMatrix` when on, a
yaw-only `D3DXMatrixRotationY(mYaw)` when off.

### GUI input index -> physical key

Established by play test, not from the binary - the bindings live in
`RawControllerInputs::mCurKeyboardBindings`, which is populated from data rather than
hardcoded. Positions are US-layout; the reporter was on a German keyboard, given in
brackets.

| GUI index | Key | Effect |
|-----------|-----|--------|
| `0x12` | Home | Altitude up |
| `0x13` | End | Altitude down |
| `0x16` | `=` [`´`] | Move speed faster (`mMultiplier *= 1.25`) |
| `0x18` | `-` [`ß`] | Move speed slower (`*= 0.8`) |
| `0x17` | `[` [`ü`] | Turn speed faster |
| `0x19` | `]` [`+`] | Turn speed slower |

`M` was also reported as pause / unpause. That is **not** explained by the console shortcut
table, which binds `M` to `ToggleSoldierModels` and puts `stepframe` on `;` and `/`, so it
is presumably a separate game binding. Unresolved.

Statics, modtools: `sQuakeControl` `0x00AFB7C0`, `sInvertAxis` `0x00AFB7C1`, `sStop`
`0x00AFB7C2`. None of them, nor the four follow/tether globals, is reset on entering or
leaving the camera.

### Follow and tether

- `gSetTargetObj` - one-shot, self-clearing. 500-unit forward ray with collision flags `0`;
  on a hit, stores `followThisObj` and sets `gIsFollowingObj`.
- `gIsFollowingObj` - builds a look-at basis toward the target biased to half its height, via
  three normalized cross products, then `SetMatrix` and early return.
- `gSetTetherPosition` - one-shot. Records `followObjTether = targetPos - cameraPos` and sets
  `gFollowingTethered`.
- `gFollowingTethered` - also slaves position to `targetPos - followObjTether` each frame.
  The offset is world space, so the angle stays constant while tethered.

Nothing clears `followThisObj` when the target entity is destroyed - see Engine defect 4.

### Collision debug ray

If any of `gRenderRayHit{Ordnance,Rigid,Soft,Static,Terrain}CollisionFlag`,
`gRenderRayHitAABB`, `gRenderRayHitSphereFlag`, `gRenderRayHitLocationsFlag` or
`gShowSegmentCount` is set, `Update` fires a 500-unit forward ray (collision flags `0xBF`)
every frame and draws a crosshair cluster at the hit, red on a miss and yellow on a hit. On
a hit it can also print `S:%d`, `V:%d`, the geometry name, `Mesh: %d verts` or
`No collision mesh`, render the `CollisionModel` filtered per category mask, draw the AABB,
and draw the collision sphere with its transformed OBB. It always prints `CamPos(...)` and,
on a hit, `HitPos(...) / Dist = ...`.

`gRenderNoCollisionFlag` is the master reset: when set it clears the other six.

**This is why those debug views only work in the free camera** - the ray that feeds them
lives inside `FreeCamera::Update` and nowhere else.

## The freecam light

modtools only (see Build availability). A `RedOmniLight`, `0x120` bytes, allocated from
`RedOmniLight::sMemoryPool` (`0x00EDC680`) - the same pool `EntityLight`, `EntityHologram`,
`LightFlash` and `ReadLight` draw from.

Globals: `s_pFreeCamLight` `0x00B76C3C`, `s_freeCamLightColor` `0x00B76C58` (default
`RedColor::MAGENTA` normalized at static-init), `s_freeCamLightRadius` `0x00ACC934`
(default 6.0).

`SetFreeCamLightCallback` (`0x004AD8xx`) dispatches on the **PblHash of the leaf command
name**, not the full path: `enable` `0xAF8BB8CE`, `color` `0x3D7E6258`, `radius`
`0x0DBA4CB3`, `freeze` `0x30C707A2`.

Positioning, in `Update`: a backwards ray along the camera forward with collision flags
`0x90`, placing the light at `camPos - (dist*t - 1.25) * forward`, so it stands 1.25 units
off whatever surface is behind the camera. Always draws a yellow debug sphere at the light
and prints the hit object's name in 3D. One light, viewport 0 only.

The scalar deleting destructor (`0x00845D30`) returns the block to the pool correctly, so
`freecamlight.enable 0` is not itself a bad free.

## Engine defects

### 1. Null dereference when the light pool is full

The allocation result is tested, and the failure branch (modtools `0x004ADA1C`) is:

```
004ad9d1: call MemoryPool::Allocate
004ad9d6: test eax, eax
004ad9d8: je   0x4ada1c
...
004ada1c: xor eax, eax
004ada1e: mov edx, [eax]      ; null dereference
```

It stores NULL into `s_pFreeCamLight` and immediately dereferences it. Read AV at address 0.

### 2. `s_pFreeCamLight` survives a level change, its block does not

**This is the `freecamlight.enable 0` crash.** `s_pFreeCamLight` (modtools `0x00B76C3C`) is
referenced from exactly two regions - `SetFreeCamLightCallback` and `FreeCamera::Update` -
and nothing clears it on level load or unload. The `0x120` block behind it does not survive
the level, so on the new map the disable path at `0x004ADA30` runs on a recycled block:

```
004ada30: mov ecx,[s_pFreeCamLight]   ; still the old block
004ada36: test ecx,ecx
004ada38: je   0x4ada59
004ada3a: mov eax,[ecx]               ; "vtable", actually recycled memory
004ada3c: call [eax+8]                ; RedLight::Deactivate -> anywhere   <-- AV
```

Seen both ways in captured crashes:

- `EAX = 0xCDCDCD00` - the block is fresh debug-CRT heap fill, so the pool's backing memory
  was reallocated. Faults reading `[0xCDCDCD08]`.
- `EAX = ECX + 0x120` - the block is on the pool free list and its first dword is the
  free-list link to the next block. `[EAX+8]` reads as 0, so it calls address 0 and the
  crash reports `EIP=00000000` with return address `0x004ADA3F`.

The re-enable is also a silent no-op, because the enable path opens with
`if (s_pFreeCamLight != 0) return;` - which is why the light "stops working" after a reload.

**Fixed** by `PatcherDLL/src/render/red_light_stale_node_fix.cpp`, which zeroes the pointer
from the `init_state` hook at every mission start. Disable becomes a no-op and enable builds
a fresh light, so the light works again after a reload rather than silently doing nothing.

### 3. `RedLight::Deactivate` unlinks a drained node

Separate defect, and **not** the freecamlight crash - that one faults before Deactivate is
ever entered. This one bites any light that outlives a list drain, which in practice means
long-lived lights we own rather than engine ones.

`RedLight::Activate` (`0x0082F7C0`) is the only writer of node1's owner field, and always
sets it to the light itself:

```
lea edx,[ecx+0x30]
mov [edx+0xc],ecx        ; owner = this
mov [edx],0xae3ae0       ; _pList = &s_GlobalList
...
or  [ecx+4],0x400
```

The teardown drain (`0x0082F770`, called from `0x00830090` and `0x008301C8`, which walk all
four list heads; `0x00830350` drains the three visible lists every frame) unlinks the fast
way - it zeroes each node's `_pList` and `owner` but leaves `_pPrev`/`_pNext` dangling, and
**never clears the light's `0x400` flag**.

`RedLight::Deactivate` (`0x0082F5E0`) then trusts that flag. Its *second* node is guarded on
`_pList != 0`; its first node has no guard at all:

```
test ah,4                ; flags & 0x400, the only check
je   ret
dec  [0x00AE3AF0]
mov  eax,[ecx+0x34]      ; _pPrev, stale after a drain
mov  [eax+8],esi         ; WILD WRITE
mov  [eax+4],esi         ; WILD WRITE
```

So a light that survives a drain and is later deactivated writes two pointers into freed
memory, and the crash lands later in whatever walks a light list next.

**Guarded** by `PatcherDLL/src/render/red_light_stale_node_fix.cpp`, which detours
`RedLight::Deactivate` and skips the unlink when node1's owner is no longer the light. Same
reasoning as `lightsaber_illumination.cpp`'s `still_linked()` / `forget()` pair, one level
lower. Kept as defence in depth for our own static-storage lights; it is not what fixed the
freecamlight crash.

`RedLight::Activate` and `Deactivate` are byte-identical on all three builds (verified
against all three images), so the guard ports unchanged:

```
modtools  8B 41 04 F6 C4 04                mov eax,[ecx+4] ; test ah,4
Steam/GOG 8B D1 F7 42 04 00 04 00 00       mov edx,ecx ; test [edx+4],0x400
```

### 4. `followThisObj` outlives the entity it points at

**This is the "spectate something that dies" crash.** `debugmenu.SetFreecamTarget` stores the
raycast hit in `followThisObj` (`0x00B76C9C`) as a raw pointer and nothing ever clears it, so
the first frame after the target is destroyed:

```
004ae332: mov edx,[0x00B76C9C]   ; followThisObj, still the dead entity
004ae345: test edx,edx           ; non-null, so the guard passes
004ae356: mov eax,[edx]          ; "vtable" of a freed block -> 0xDDDDDDDD
004ae35f: call [eax+4]           ; read AV at 0xDDDDDDE1
```

`0xDDDDDDDD` is the debug-CRT freed-block fill, so the block really was released. Captured
with `EAX=DDDDDDDD`, `ECX=EDX=213F6F3C` - `ECX` and `EDX` are the dead object, `EAX` is the
vtable pointer read out of it.

Three call sites dereference the pointer, so the pointer itself is what has to be validated:
the tether-capture block at the top of `Update`, the tethered follow branch above
(`0x004AE35F`), and the plain follow branch (`0x004AE3C0` / `0x004AE3D8`).

**Guarded** by `PatcherDLL/src/debug_commands/freecam_target_fix.cpp`, called from the
`FreeCamera::Update` hook in `hover_springs.cpp` before the original runs. It checks the
target's vtable pointer against the exe's `.rdata` bounds (modtools
`0x00A2A000..0x00AC3000`; every vtable in `game_addrs.hpp` falls inside it) and, on a miss,
clears `followThisObj`, `gIsFollowingObj` and `gFollowingTethered` so the camera drops back
to manual control. The one-shot request flags `gSetTargetObj` and `gSetTetherPosition` are
left alone, so re-locking in the same frame still works.

Limitation: a recycled block whose first dword happens to be a `.rdata` address passes the
check. The exact fix is clearing the pointer at entity destruction - the manager loop at
`0x0048FC70` is the natural hook - but the commands are modtools-only, so the cheap guard
was judged proportionate.

## Reading a `RedLight::Deactivate` crash

The function is two unlinks. Offsets from the modtools entry:

| Offset | Instruction | Node |
|--------|-------------|------|
| `+0x1D` | `mov [eax+8], esi` | global (light `+0x30..0x3C`) |
| `+0x26` | `mov [eax+4], esi` | global |
| `+0x3F` | `mov [edx+8], eax` | visible (light `+0x40..0x4C`) |
| `+0x48` | `mov [edx+4], eax` | visible |

An AV at `+0x1D`/`+0x26` means the *global* node was stale: that is defect 2, a light that
outlived a drain.

An AV at `+0x3F`/`+0x48` means the *visible* node's `_pPrev`/`_pNext` were garbage while its
`_pList` was non-null. Neither a fresh `RedOmniLight` (the ctor zeroes `+0x40..0x4C`) nor a
drained one (the drain zeroes `+0x40` and `+0x4C`) can be in that state, so it means **the
light block itself was written over** - use-after-free or a stray write, not a stale link.
Register values that look like negative floats there are world coordinates.

`EntityLight` layout, for walking such a stack: `_light` `+0x90`, `_LightBeam` `+0xA0`,
`m_bOn` `+0x2B0`, `_color` `+0x2B1`, `_lightClass` `+0x2D0`, size `0x2E0`. modtools
`EntityLight::TurnOff` `0x0051B3E0`, dtor `0x0051E100`, deleting dtor `0x0051EC30`. The
entity/effect manager loop that drives destruction is `0x0048FC70`: it walks a list, calls
virtual `Update`, deletes anything returning false, and keeps its cursor in the global at
`0x00B6A770` so callees can safely mutate the list.

## Shortcut keys

`RedCommandConsole::RegisterShortCutKey(dikCode, "command line")` is modtools
`0x007EC670`, and `AddCommand` is `0x007ED560`. Keys are **DirectInput scan codes**, stored
in a 256-entry `sKeyTable` capped at 128 bindings. Confirmed by the cursor-key cases in
`RedConsole::Update`'s line editor, which switch on `0xC7`/`0xC8`/`0xCB`/`0xCD`/`0xCF`/
`0xD0`/`0xD3` - exactly DIK_HOME/UP/LEFT/RIGHT/END/DOWN/DELETE - and by the four bindings
below that land on Caps Lock, numpad `+`, numpad `.` and Delete.

Dispatch is `RedCommandConsole::ProcessKeys`, reached from `RedConsole::Update` **only when
`mMode == 2` and only on key-down**. `SetMode(mMode + 1)` on DIK `0x29` (grave) resolves to
mode 1 (visible, text input) or mode 2 (hidden, hotkeys live) and never back to 0, so the
cycle after the first press is 1 -> 2 -> 1 -> 2.

All 16 modtools bindings, enumerated from the call sites rather than from the Phantom build:

| DIK | Key | Command |
|-----|-----|---------|
| `0x10` | Q | `FreeCameraQuakeMode` |
| `0x17` | I | `FreeCameraInvertAxis` |
| `0x1F` | S | `Renderer.ScreenshotSetup` |
| `0x20` | D | `ToggleDisplay` |
| `0x27` | `;` | `stepframe 1` |
| `0x28` | `'` | `FreeCameraStop` |
| `0x2C` | Z | `Screenshot` |
| `0x2D` | X | `PrintPlayerCoords` |
| `0x32` | M | `ToggleSoldierModels` |
| `0x33` | `,` | `ai.camdown 1` |
| `0x34` | `.` | `ai.camup 1` |
| `0x35` | `/` | `stepframe 0` |
| `0x3A` | Caps Lock | `ToggleDudes` |
| `0x4E` | Numpad `+` | `NighInfiniteReinforcements` |
| `0x53` | Numpad `.` | `Net.CloseJournal` |
| `0xD3` | Delete | `SelfDestruct` |

A 17th call site at `0x0044793D` is a two-argument forwarding wrapper, not an extra binding.

## Open

- **LODs break under freecam** (see `ROADMAP.md`). Nothing in this path retargets LOD
  scoring at the free camera, consistent with LOD grading against the player entity or the
  game camera rather than the active render camera.
