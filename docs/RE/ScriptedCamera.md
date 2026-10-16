# Scripted camera

Mission scripts taking the player's view: a shot from a point, a chase camera on any unit,
a flight along a ZeroEditor path, a look toward something, a zoom, a shake, and a blend back.
The user's ask, 2026-10-16: script the player camera, and in the end a storyboard or timeline
editor for cutscenes. This page has the engine facts the camera stands on, the design, and the
order it is built in. M = modtools, S = Steam, G = GOG, P = Phantom (PDB names).

## Where the view is decided

`ChaseCamera::SetupCamera` (M `0x004A2440`, S `0x00453D00`, G `0x00453CE0`; thiscall
(`RedCamera*`, float dt), RET 8) builds the local player's view every frame, in this order:

1. The aspect: screen and view sizes times `RedRenderer::GetScreenAspectRatio`, height over width.
2. The owner's zoom target (its vtable, through a float it writes, 1 unzoomed) and FOV target:
   `GetDefaultFOV` (48 degrees), handed to the owner to override, then widened for the screen
   when there is one camera (see [BF2's FOV model](#fov)).
3. The owner's `SetupCameraTrackMatrix` writes `mMatrix`, eased from last frame's; the chase
   camera's own interpolation and collision ray adjust it.
4. The stock shake, then `RedCamera::SetMatrix(camera, &mMatrix)`.
5. Zoom and FOV ease toward their targets by `exp(-k dt)`:
   `SetZoom(camera, (zoom - target) * e + target)` and
   `SetFOV(camera, (GetFOV() - target) * e + target, aspect)`.

GameExt's camera shake already detours it (`render/camera_shake.cpp`): it calls the original,
moves and turns `mMatrix`, and calls `RedCamera::SetMatrix` again. The scripted camera is one
more stage after that, inside the same detour: it hands `RedCamera::SetMatrix` its own matrix
and leaves `mMatrix` alone, so BF2's easing, the first-person arms and the aim keep following
the player's own camera, and a release has the live view to blend back to.

Because step 5 eases from the camera's current FOV, a FOV the script sets after
`SetupCamera` is where next frame's easing starts: when the script stops setting it, the game
eases back to its own target by itself.

### The render camera

`RedCamera` (layout in `game/Battlefront2/Source/RedCamera.h`), the same on every build:

| Field | Offset | |
|---|---|---|
| `_Matrix` | `+0x30` | rows right, up, back, position: the camera looks along minus the third row |
| `_MatrixInverse` | `+0x70` | world to camera, recomputed by `SetMatrix` |
| `_fUnzoomedTanHalfFOVWidth` | `+0x138` | `tan(fov / 2)` |
| `_fUnzoomedTanHalfFOVHeight` | `+0x13C` | the width's times the aspect (height over width) |
| `_fZoom` | `+0x140` | 1 unzoomed |

| Function | M | S | G | Contract |
|---|---|---|---|---|
| `RedCamera::SetMatrix` | `0x007FEED0` | `0x006CBEF0` | `0x006CCF90` | thiscall(const PblMatrix*), RET 4 |
| `RedCamera::SetFOV` | `0x007FEF90` | `0x006CBE60` | `0x006CCF00` | thiscall(float fov, float aspect), RET 8: writes `+0x138` and `+0x13C`, rebuilds the projection |
| `RedCamera::SetZoom` | `0x007FEF30` | `0x006CBFC0` | `0x006CD060` | thiscall(float), RET 4: writes `+0x140`, rebuilds the projection |
| `RedCamera::GetFOV` | `0x007FD1C0` | `0x006CBCA0` | `0x006CCD40` | `2 * atan(+0x138)` in ST0 |

The scripted camera calls only `SetMatrix` and `SetFOV`. It sets the zoom by writing `+0x140`
before its `SetFOV`, whose projection rebuild reads it, and works the FOV out from `+0x138`
itself.

The camera's facing: the free camera's follow mode (`FreeCamera::Update`, M `0x004AE1B0`)
builds its look-at with `back = normalize(camera - target)`, `right = normalize(cross(up,
back))` with up (0, 1, 0), and `up = cross(back, right)`. The scripted camera builds its
matrices the same way.

### <a id="fov"></a>FOV

The FOV value is horizontal at 4:3, and the shipped builds widen it per screen holding the
vertical (Hor+): `ChaseCamera::SetupCamera` passes it through `GetWidescreenFOVAdjustment`
(M `0x0049FFD0`, S `0x0044E540`, G `0x0044E520`) when there is one camera. A script's FOV is
taken in the same measure (48 is the game's default) and widened the same way, from the
camera's own aspect `a = +0x13C / +0x138`: `fov' = 2 * atan(0.75 * tan(fov / 2) / a)`.

## First person

The arms and the cockpit are drawn with the player's own view, so a shot from anywhere else
must show the player in third person.

- `Controllable::Update` (P `0x004EFE80`) calls `Trackable::UpdateFirstThirdPerson(playerId)`
  every update, which asks `Trackable::CheckFirstPerson(playerId)` which view the player should
  have and calls `Tracker::SetFirstPersonView` when it differs from
  `Tracker::IsFirstPersonView`. `SetFirstPersonView` stores the flag (Tracker `+0x14`) and
  activates or drops the first-person model (`FirstPerson::Activate`); a soldier going first
  person also lets its animator go.
- `CheckFirstPerson` answers from the controllable class's view lock (`+0x14C`: 1 third person,
  2 first person), then `IsForcedThirdPerson`, `IsForcedFirstPerson`, then the player's saved
  choice (the profile's soldier or flyer flag).

| `Trackable::CheckFirstPerson` | M `0x004BAC10` | S `0x00658830` | G `0x006598D0` |
|---|---|---|---|
| Contract | thiscall(int playerId), bool in AL, RET 4, on every build | | |

The scripted camera detours it with a stub that answers third person while a shot, a follow or
a path holds the view and otherwise jumps to the original untouched. The game then switches the
view itself, and when the shot is over `CheckFirstPerson` gives the player's choice again and
the game switches back.

## Units, points and matrices from Lua

- **A unit** is given as the wingmen functions take one: an entity pointer (light userdata), a
  world object's name, or a character index (its remote, else its vehicle, else its soldier,
  read each frame from the character array).
- **Names** are looked up every frame in `EntityEx::mIdMap_` (`entityex_id_map`, 0x800 slots,
  through `pbl_hash_table_find`), so a destroyed object simply stops being found. A pointer is
  turned into its name when it has one (`EntityEx::mId`, `+0x4`, found again in the map), else
  kept as a `PblHandle<GameObject>` (`mHandleId` at `+0x204`) and dropped when the handle goes
  stale. Its matrix is `EntityGeometry::mMatrix` at `+0xF0` (rows right, up, forward,
  position), for any object whose RTTI answers `EntityGeometry`.
- **A unit faces** along its matrix's third row. A shot from a unit or from a matrix looks the
  way it faces.
- **A matrix** is the stock one: a 64-byte full userdata holding a `PblMatrix`, as
  `GetPathPoint`, `GetEntityMatrix` and `CreateMatrix` push it (M `0x004701A0`: `lua_newuserdata(L,
  0x40)` and a copy; no metatable). An entity pointer is light userdata, so `lua_type` tells the
  two apart.
- **A point** is a table `{x, y, z}`.

| Lua API | M | S | G |
|---|---|---|---|
| `lua_type` | `0x007B7FE0` | `0x0069C680` | `0x0069D710` |
| `lua_newuserdata` | `0x007B8E90` | `0x0069BE30` | `0x0069CEC0` |

Both cdecl. Found through `luaB_type` (the base library's `type`, which calls `luaL_checkany`,
`lua_type`, `lua_typename`, `lua_pushstring`) and the matrix push helper `GetPathPoint` calls
(M `0x004701A0`, S `0x00591200`, G `0x005921A0`).

### Paths

`GetPathPoint` (M `0x00470380`, S `0x0058EFE0`, G `0x0058FF80`) shows the path data, the same on
every build:

- The path list: M `0x00AE33F0`, S `0x007DF840`, G `0x007E0840`. Its `+0x4` is the first list
  node; a list node's `+0x4` is the next and `+0xC` the path, a null path ending the list.
- A path: `+0x14` its name hash, `+0x18` its nodes, `+0x1C` how many.
- A node: 0x1C bytes, the position (3 floats) then the rotation (a quaternion, 4 floats).

`GetPathPoint` turns a node's rotation into a matrix through the conjugate quaternion and two
negated rows. Rather than copy that, a path flown with its nodes' rotations asks the game's own
`GetPathPoint` through Lua for each node when the command is given. Retail's `GetPathPoint`
checks no range, so the node count comes from the path itself first.

## The commands (first slice)

Every command is local: the camera is drawn by each machine for itself, and nothing reaches the
simulation or the network. They act while the player has a unit in the world (the chase
camera); with none, the second slice's cutscene mode uses the free camera.

| Command | What it does |
|---|---|
| `CameraShot(where, opts)` | The view from a matrix, a unit (looking the way it faces) or a point. `opts.lookAt` keeps it aimed at a unit or point, `opts.offset` moves it `{right, up, forward}` in `where`'s frame, `opts.fov` sets the FOV, `opts.blend` blends in over seconds (0 cuts), `opts.time` releases it after so many seconds, `opts.release` is that release's blend. |
| `CameraFollow(unit, opts)` | A chase camera on any unit: `opts.offset` in the unit's frame (default `{0, 4, -12}`: 4 m up, 12 m behind), `opts.lag` seconds of trailing (default 0.3), `opts.world` to keep the offset in world axes, and `lookAt`, `fov`, `blend`, `time` and `release` as above. It aims at the unit (`opts.aim`, default `{0, 1, 0}`, in its frame) unless `lookAt` says otherwise. |
| `CameraPath(path, seconds, opts)` | Flies the camera along a ZeroEditor path in `seconds`, smoothly through every node. It aims along the way it travels, at `opts.lookAt`, or with `opts.aim = "nodes"` by each node's rotation. `opts.ease` is `"inout"` (the default), `"in"`, `"out"` or `"linear"`; `opts.loop` starts over at the end; otherwise the last view holds until a release, or `opts.release` releases it there. |
| `CameraLookAt(target, opts)` | With a scripted view: aims it at `target` (nil goes back to what the command said). Without one: turns the player's own view toward `target` from where it is for `opts.time` seconds (default 2), blending in over `opts.blend` and back over `opts.release` (0.5 each). |
| `CameraFOV(fov, seconds)` | The FOV of whatever view is drawn, the player's own included, reached over `seconds`. `CameraFOV()` hands it back to the game, which eases to its own. |
| `CameraShake(amount, seconds)` | A shake through the stock queue, drawn as GameExt's blast: amount as an explosion's (a grenade 0.5, a detpack 2), default 1 second. It shakes a scripted view too. |
| `CameraRelease(seconds)` | Blends back to the player's own camera over `seconds` (default 1; 0 cuts). |
| `GetCameraMatrix()` | The view being drawn, as a stock matrix: to hand to `CameraShot` later, or to print a view framed by flying the free camera. |
| `GetCameraPosition()` | Its position, as three numbers. |
| `IsCameraScripted()` | True while a script holds the view, a release included. |

Rules the commands share:

- A new view command replaces the one before it, blending from the view on screen.
- A unit that is destroyed or deleted leaves the camera where it last was; a follow holds its
  last spot.
- While a shot, a follow or a path holds the view, the player is drawn in third person, and
  their own setting comes back once the release ends. The cut waits one frame if the player is
  in first person, so the switch happens first.
- The stock shake queue, explosions and `CameraShake`, shakes a scripted view; the player's own
  shakes (firing, landing, hits) stay with the player's camera.
- The reticule keeps to the player's own view, as it does under camera shake.
- The HUD stays up in this slice. Hiding it, the letterbox and locking the controls are the
  second slice's cutscene mode.

## The slices

1. **Camera commands** (above), built 2026-10-16 on camera shake's detour, `CheckFirstPerson` and
   the path data: modtools, Steam and GOG (`render/scripted_camera.cpp`, `lua/lua_camera.cpp`).
   `tests/scripted_camera_tests.cpp` runs the maths (look-at frames, rotation blends, object
   facing, the FOV widening, the follow trail at any frame rate, the path curve);
   `tests/scripted_camera_abi_tests.py` audits every address, guard and layout above on all three
   builds, and fails on a wrong address, guard byte or offset. Not played yet.
2. **Cutscene mode.** `CutsceneMode(on, opts)`: controls locked, the HUD's ten displays switched
   off the way the stock free camera does it, the player's unit unhurt, a letterbox (a HUD event
   for `.hud` files, so VisualHUD gets it too), and the free camera for moments with no player
   unit.
3. **Cutscene files.** A cutscene as data: tracks of timed camera keys, unit orders, waits,
   subtitles, sound and marks on one clock, played by `PlayCutscene` and skippable.
4. **The in-game director** (modtools): fly the free camera, drop a key at the view, scrub, play
   from any time, save the file.
5. **The timeline editor:** a desktop app beside VisualHUD, with a storyboard strip of shots
   grabbed from the game and a live link that plays the cutscene in the running game.

Not built and not planned yet: slow motion (the game clock is not traced), depth of field and
motion blur (the engine draws neither).
