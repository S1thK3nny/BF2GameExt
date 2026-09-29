# Stock HUD element properties

Every property a stock `.hud` element class reads, what it does, and which behaviours
are confined to one class but would help on others. Catalogued 2026-09-28 by naming
every compare constant in all 19 `ReadData` functions of Phantom (`Battlefront2.exe`,
**P**, with PDB names) and modtools (**M**): 125 keys from the PblHash of `.hud`
identifiers and exe strings, 3 by brute force. No constant is left unnamed in either
build. Usage counts come from all 50 `.hud` files under `C:\BF2_ModTools\data*\Common\hud`.

The only key modtools reads that Phantom does not is the group `EventAlpha`; Steam
has it too. `PropagateAlpha` is already in Phantom (P `005FC510`). GOG was not
checked. For the event system and object model see [HUDSystem.md](HUDSystem.md); for
authoring see [HUDAuthoring.md](HUDAuthoring.md).

## How a property reaches its class

Each class's `ReadData` handles its own keys and passes anything else to its base.
`Element::ReadData` is the floor for every visible class: after its own keys it tries
the Position/Rotation/Scale matrix readers (P `005F4540`/`4750`/`48F0`) and otherwise
returns false, which the loader logs as "Error reading parameter".

| Class (keyword) | P ReadData | M ReadData | Unknown keys go to |
|---|---|---|---|
| Element | `005F3E10` | `00693790` | matrix readers, then false |
| ElementGroupBase (`Group`) | `005FC510` | `00699FA0` | Element |
| ElementGroup | none; vtable `009FCEE0` +20 is `005FC510` | | |
| ElementGroupPlayer | `005FCEC0` | `0069B330` | GroupBase |
| ElementText (`Text`) | `0060C850` | `006AB1D0` | Element |
| ElementBitmapBase (`Bitmap`) | `005FA670` | `00698980` | Element |
| ElementBitmapMasked | `005FA9C0` | `00699370` | BitmapBase |
| ElementModel3D | `00606150` (Read `00605C70`) | `006A44F0` (Read `006A3BD0`) | Element |
| ElementBar (no keyword) | `005F6780` | `00694F10` | false |
| ElementBarBitmap | `005F7520` | `00695900` | Bar, then BitmapBase |
| ProceduralBarBitmap | `0061B8A0` | `006BA2D0` | BarBitmap |
| ElementBarSegmented | `005F8AA0` | `00696F80` | Bar, then GroupBase |
| ElementMultilineText | `00607EB0` (Read `00607DA0`) | `006A5780` | GroupBase |
| BorderedBox | `005ED9C0` | `0068DF00` | GroupBase |
| ObjectiveList | none; vtable `00A01554` +20 is `005FC510` | | GroupBase |
| ElementVehicleSeating | `0060E570`, a forwarder (Read `0060E300`) | not located | GroupPlayer |
| ElementMap | `00601260` (Read `006009E0`) | `0069CF10` | GroupPlayer |
| ElementTarget | `00609C50` (Read `006098C0`) | `006A75E0` | GroupPlayer |
| ElementTarget::Target | `00609D50` | `006A7130` | BitmapBase |
| ViewPort (not an Element) | `0061F170` (Read `0061EDF0`) | `006BDFB0` | false |
| Sound (not an Element) | `0061C190` | `006BA9E0` | false |

## Properties by class

### Element: every visible class

| Key | Arguments | Effect |
|---|---|---|
| `Position` | x, y, z [, mode] | Mode `Pixels`, `Screen`, `Viewport` or `Frame` (table at P `00A9154C`), or a bool, 1 = Viewport; default Pixels. Top-level elements get a widescreen X fix (P `005F2AC0`) |
| `Rotation` / `Scale` | exactly x, y, z | Rotation in degrees; Scale applied in PostReadSetup (P `005F3CA0`) |
| `ZOrder` | 0 to 255 | Draw order |
| `Alpha` | 0 to 1 | `mAlpha` (+AC). Update (P `005F5870`, M `006920F0`) writes the render alpha as fader × `Alpha` × 255 every frame |
| `Color` | exactly r, g, b | Current colour and `mColorDefault` (+DC); alpha forced to 255 |
| `ColorChange` | r, g, b | The colour a change flashes to (+E0) |
| `ColorChangeRate` | seconds, default 0.3 | Time to ease back to `mColorDefault` |
| `ColorPulseRate` | seconds, default off (`FLT_MAX`) | Snaps to `ColorChange` every N seconds |
| `UseChangeColor` | bool (+FC bit 0) | `Changed` (P `005F2F60`) snaps to `ColorChange`. Fired by `EventChanged`, `EventText`, `EventNumber`, `EventBitmap` and the bar setters |
| `FadeInTime` / `FadeHoldTime` / `FadeSustainTime` / `FadeOutTime` | seconds | Envelope Attack, Hold, Sustain, Release. Hold defaults to `FLT_MAX`; `EventDisable` moves Hold to Sustain, so `FadeSustainTime` is the delay before the fade-out (P `0060F040`, `0060EFE0`) |
| `BlendMode` | `Alpha` / `Additive` | Virtual setter, see [HUDBlendMode.md](HUDBlendMode.md). An unknown value is ignored silently |
| `Viewport` | 0 to 4, clamped | Screen group; children inherit it (P `005FC3F0`) |
| `EditOnly` | bool (bit 3) | Undocumented: the element goes to `gScreenGroupEdit` and only shows in the HUD editor (P `005F5C00`) |
| `Scalable` | bool (bit 5) | Follows the runtime HUD scale: `SetRuntimeScale` (P `005F5400`) from `SetRuntimeScaleAll` (P `005F5700`) from Lua `ScriptCB_SetHUDScale`, which the stock video options call (`ifs_opt_pcvideo.lua:89`) |
| `EventEnable` / `EventDisable` | any event; the value is ignored | Enable or disable, then Update(0.001) (P `005F3410` / `005F33E0`) |
| `EventChanged` | any event | Fires `Changed` |
| `EventColor` | Color | Sets `mColorDefault`, eased at `ColorChangeRate`; its alpha is discarded (P `005F3390`) |
| `EventPulseRate` | Int, Uint or Float | Sets the pulse period (P `005F3440`) |
| `EventFadeOut` | a new Bool event name (output) | Sent when the envelope reaches Sustain, Release or Inactive |
| `AnimEnable` / `AnimDisable` / `AnimChange` | anything | Accepted and ignored on both builds (`BDBFD84F`, `6242102C`, `3455B0A8`) |

### Group base

Applies to `Group` and to BarSegmented, MultilineText, BorderedBox, ObjectiveList,
Map, Target and VehicleSeating.

| Key | Arguments | Effect |
|---|---|---|
| `Rect` | w, h [, HAlign L/C/R [, VAlign T/C/B [, mode or bool]]] | Default 1 × 1, Viewport (P `005FB4D0`). PostReadSetup shifts the origin by −{0, ½, 1} of w and h (P `005FC7E0`). Sizes MultilineText, BorderedBox, ObjectiveList and BarSegmented |
| `PropagateAlpha` | bool | Bit 0 of the render group +84: at draw, each child's alpha is multiplied by the group's (P `008C38F0`) |
| `EventPosition` | Vector3 | Converted with the group's Rect mode, then the Rect anchor re-applied (P `005FB930`, M `0069A350`) |
| `EventScale` | Vector3 | Absolute axis lengths, replacing the authored Scale (P `005FBDE0`, M `0069A4B0`) |
| `EventRotation` | Vector3, degrees | Rebuilds rotation X·Y·Z, keeps the lengths (P `005FBAC0`, M `0069A740`) |
| `EventAlpha` | Float; modtools and Steam only | Render alpha = value × 255, for one frame (M `0069A6E0`); see [HUDSystem.md](HUDSystem.md#eventalpha-lasts-one-frame-read-on-modtools-2026-09-28) |

`ElementGroupPlayer` adds `EventPlayerIndex` (Int), which retargets the group to
player N (+154, P `005FCE30`).

### Text

| Key | Arguments | Effect |
|---|---|---|
| `Text` | localize key | A missing key shows the raw key and logs a warning |
| `TextFont` | font name | `systemfont` is special; `japanese` is forced when `sJapaneseOnly` is set |
| `TextBox` | w, h | Viewport fractions only; no mode argument |
| `TextScale` | exactly sx, sy | Glyph scale |
| `TextAlignment` | exactly h, v | `Left` / `Right` / `Center`; `Top` / `Bottom` / `Center` / `Baseline` (P `00A91A14`) |
| `TextBreak` | `None` / `Word` / `No Reformat` / `Word Reformat` | |
| `TextClip` | `Character` | The only accepted value; clipping cannot be turned off |
| `TextStyle` | `Normal` / `Shadow` / `Selected` | |
| `TextCharacterSpacing`, `TextTabSpacing` | int | |
| `IntegerFormat`, `FloatFormat` | printf format | Defaults `%d`, `%f`. An Int or Uint event uses the first, a Float the second |
| `NumberToTime` | template | Prints a number of seconds with `%d` days, `%h` hours, `%m` minutes, `%s` seconds (P `0060BE50`) |
| `Percent` | bool | × 100 and printed `%.0f%`; `FloatFormat` is ignored |
| `InfiniteDashes` | bool | Int `0x7FFFFFFF`, Uint `0xFFFFFFFF` or Float `FLT_MAX` prints `--` instead of nothing |
| `EventText` | String or Uint localize hash | P `0060C360`; the text is copied |
| `EventNumber` | Int, Uint or Float | Formatted as above |

### Bitmaps

`BitmapBase` (Bitmap, BitmapMasked, BarBitmap, ProceduralBarBitmap, Target templates):
`Bitmap("tex")`, `BitmapRect(w, h [, HAlign, VAlign [, mode]])` (see
[HUDSystem.md](HUDSystem.md#bitmap-sizing-bitmaprect-read-on-phantom-2026-09-26)),
`TexCoords(u0, v0, u1, v1)`, `BitmapStyle` `Normal`/`Shadow` (the shader shadow flag,
P `008C8CB0`), and `EventBitmap` taking a Uint hash or a Texture, which fires `Changed`
(P `005FA0B0`).

`BitmapMasked` adds `Mask("tex")` and `MaskTexCoords(u0, v0, u1, v1)`, reordered
correctly into `SetMaskTexCoords` (P `008DFD90`). No HUD uses the `BitmapMasked`
keyword; `Mask` only appears on the map's Backdrop sub-items.

### Model3D

| Key | Arguments | Effect |
|---|---|---|
| `Mesh` | model name | |
| `EventMesh` | Uint hash or Model | P `006057C0` |
| `Lighting` | bool, default 1 | 0 adds render flag 0x10 (P `008DEC80`) |
| `Depth` | float, default 0.01 | Scales the model's translation (P `008DECE0`) |
| `Hardpoint` | bone name | Draws through that bone's matrix (P `008DEDA0`) |
| `InheritMeshInfo` | a Model3D element's name | Shares an earlier element's MeshInfo table |
| `MeshInfo("mesh") { Position / Rotation / Scale / Depth }` | sub-scope | Per-mesh override, up to 256 (P `00605FC0`) |

### Bars

The bar base (no keyword): `Value` (initial and default), `MinValue`, `MaxValue` (see
[HUDEventBindings.md](HUDEventBindings.md)) and `EventValue` (Int, Uint or Float,
P `005F6650`).

`BarBitmap`:
- `ScaleTexture` (bool, default on) crops U with the fill.
- `ScaleSize` (`F5B1A000`, bool, default on) is the edge-movement bit HUDSystem.md
  knew only by hash.
- `FlashyScale`: the change strip's height eases from the bar's to this value during
  the first second of its fade (P `005F7C30`).
- `FlashyIncFadeOutTime` / `FlashyDecFadeOutTime`: the strip's fade time when the value
  grew or shrank (P `005F7650`).

`ProceduralBarBitmap`, every render (P `0061B790`): alpha = saved alpha ×
(`GlowAlphaBase` 0.5 + random × `GlowAlphaScale` 1.0), size = fill × (1.3 +
(0.5 to 1) × `GlowScaleSize` 0.05). The 1.3 has no key. `NoiseFreq` (60) and
`NoiseRoughness` (0.3) are read but never used by rendering.

`BarSegmented`: `NumSegments`; `Circular` (default 1: segments on an ellipse inside
`Rect` from `AngleStart` to `AngleEnd` in degrees; 0 lays them in a row, P `005F8FF0`);
`Segment("Segment") { ... }` is the segment template.

### Lists and boxes

- **MultilineText:** `NumLines`, `DisplayTime`, `ScrollSpeed`, `AlwaysScroll`,
  `AddToTop`, `DisableOnEmpty`; `EventText` adds a line; `EventColor` sets the colour
  and alpha of the next line (P `006078A0`), replacing the Element-level `EventColor`;
  `Format("Format") { Text keys }` is the line template.
- **BorderedBox:** `Background("tex")` and `Border(w, h)` in texture pixels build a
  nine-slice box sized by `Rect` (P `005EDCE0`).
- **ObjectiveList:** no keys. It must be named `playerNobjectives` (P `0061A700`);
  `SetupText` (P `0061AE20`) hard-codes the styling: yellow active, grey completed,
  left/top aligned, 8 px margin, 2 px spacing.
- **VehicleSeating:** no keys; sub-scopes Backdrop, Empty, EmptyText, Self, SelfText,
  Player, PlayerText, AI, AIText (names from the stock file, not the reader).

### Map and Target

- **Map:** `PositionSmall` / `PositionLarge` / `PositionSpawn`;
  `BackgroundMaskShapeSmall` / `Large` / `Spawn` (`Circle` or `Rectangle`);
  `AllowMiniMap`; `PostNumbers` (flag bit 4, effect not traced); `PostFlashColor`,
  `PostFlashRate`, `PostFlashRadius`, `TargetFireColor`; events `EventToggleMapMode`,
  `EventChangeMapMode`, `EventPostHide`, `EventRefreshTarget`, `EventRefreshPost`,
  `EventRefreshMarker`. Sub-scopes are {Backdrop, PlayerFOV, PlayerDirection, Target,
  Post, PostText, PostSelect, Marker, North} × {Small, Large, Spawn}.
- **Target:** `FourSegmentLockOn`, `EventResetTargetCommon`, `EventResetTargetPlayer`,
  and 24 template sub-scopes: {Objective, Flag, Attacker, Locked, Hint, HintFlag} ×
  {OnScreen, OffScreen} × [Behind]. Templates read `MinScale` / `MaxScale` (scale
  across the target's distance range), `ColorFriendly` and `ColorInterp`. How the
  element finds and places its targets is in
  [HUDSystem.md](HUDSystem.md#world-markers-and-distances-researched-2026-09-28).

### ViewPort and Sound

- **ViewPort:** `Viewport0..3Position(x, y, z)` places screen groups 1 to 4,
  `Viewport0..3UseSafezone` sets their safe-zone flag, and `EventNameFilter` re-reads
  the scope once per camera, so once on PC.
- **Sound:** `Sound("id")`, `EventTrigger`, `EventStop`.

## Behaviours worth exposing on more classes

Every class passes unknown keys down to `Element::ReadData` (P `005F3E10`, M
`00693790`), so one detour there reaches Text, Bitmap, the bars, Model3D, Target
templates and every group. It does not reach keys a subclass consumes first (group
`Event*`, MultilineText `EventColor`/`EventText`, Model3D `Depth`), nor ViewPort,
Sound or `MeshInfo`. The sidecar handler pattern already exists: the lerp binds extra
handlers through `Item::ReadEvent` and unlinks them before the element is freed.

1. **`EventAlpha` on every element, and persistent.** High value, easy. Today it is
   groups only and lasts one frame. A GameExt callback should write `mAlpha` (+AC),
   which `Element::Update` multiplies by the fader every frame, so a value holds until
   the next send. Caveats: ProceduralBarBitmap overwrites `mAlpha` every render (write
   its saved alpha), Target templates treat an alpha of 0 as a flag, and the BarBitmap
   flash strip copies `mAlpha` only when it starts (P `005F7650`). Changing the stock
   group callback the same way would alter the stock reticle whenever its event stops.
2. **`EventScale` and `EventRotation` on every element.** High value, easy. The stock
   callbacks (M `0069A4B0` / `0069A740`) touch only `mElement` (+B0), so they can be
   reused as they are, letting one icon, reticle or model pulse or spin without a
   wrapper group. `EventScale` discards the authored Scale, and elements a parent
   rewrites every frame (Target and Map copies, segments, seat markers) will not keep
   it.
3. **`EventPosition` on every element.** Medium. The stock callback (M `0069A350`)
   reads the group's Rect fields, so a small new one is needed, from
   `ConvertRelativeToPixels` (P `005F3090`) and `SetPosition`.
4. **Templating beyond ViewPort.** High value, bigger job. Only `ViewPort::Read`
   (P `0061EDF0`) re-reads a scope with `Item::SetEventFilter`. A group-level repeat
   could stamp out weapon 1 and 2 or 16 command post slots from one block; the filter
   substitutes one `%` number.
5. **Copying another element, generalising `InheritMeshInfo`.** Medium. The copy
   routines exist (`Element::operator=` P `005F28B0`, `ElementBitmapBase::operator=`
   P `005F9A20`, `ElementText::CopyToTextElement` P `0060BC70`) with lookup by
   `Element::FindByHashID` (P `005F34D0`). Event bindings are not copied.
6. **The procedural flicker on any element.** Low to medium; better as a random-number
   transform feeding items 1 and 2.
7. **A runtime texture for BorderedBox.** Low, easy: `BorderedBox::SetTexture`
   (P `005EDC80`) exists but has no event.
8. **ObjectiveList styling from a template,** as MultilineText's `Format`. Medium; needs
   a hook on `SetupText` (P `0061AE20`).

Not recommended: `Mask` on bars (a different render element type) and `Additive` on
Model3D (not a byte patch; see [HUDBlendMode.md](HUDBlendMode.md)).

## Surprises and traps

- **A group's colour always tints its children.** `RenderUsingContext` (P `008C38F0`)
  multiplies each child's colour by the parent's RGB with alpha forced to 255, so a
  group's `Color` or `EventColor` tints its whole subtree, while alpha needs
  `PropagateAlpha` at every level. Stock HUDs set `Color` on groups 130 times.
- **A group's `BlendMode` reaches only children read before it:** `SetBlendMode`
  (P `008C3B20`) walks the current child list. No HUD sets it on a group.
- **An element's `EventColor` alpha is discarded** by the per-frame alpha write;
  MultilineText's own `EventColor` does set per-line alpha.
- **Editor-only names are rejected by the reader:** `HAlign`, `VAlign`,
  `BitmapHAlign`, `BitmapVAlign`, `BitmapWidthHeight`, `RelativeWidthHeight`,
  `RelativeSize`, `TextHAlignment`, `TextVAlignment`, `PositionRelative`,
  `ScaleUniform`, `ScaleAll`, `North` and `PositionRelativeSmall/Large/Spawn` exist only
  in Get/SetProperty, so a `.hud` using them logs "Error reading parameter".
- **`Frame` is valid only in `Position`;** `Rect` and `BitmapRect` take the first three
  modes.
- **Target templates:** an `Alpha` that rounds to 0 switches the template to team
  colour (P `0060A360`); stock `HintOffScreen` and `HintOffScreenBehind` use it.
- **`Item::ReadEvent` (P `00617E30`) returns true for a missing event** and only logs.
- **An element binds one `EventEnable` and one `EventDisable`.** Each is a single
  handler, and `EventClass::RegisterEventHandler` (P `0060F760`) unlinks a handler from
  its old class before linking it to the new one, so a second `EventDisable` line
  silently replaces the first. Stock HUDs never write two; they pair `x` with
  `x.disable` instead.
- **Keys no stock HUD uses:** `EditOnly`, `EventChanged`, `EventFadeOut`, `Percent`,
  `TextClip`, `TextTabSpacing`, `IntegerFormat`, `MaskTexCoords`, `Depth`,
  `Hardpoint`, `InheritMeshInfo`, `MinValue`, `MaxValue`, `ScaleSize`, every Procedural
  Glow and Noise key, `DisableOnEmpty`, `ColorInterp`, `PostNumbers`, `PostFlash*`,
  `TargetFireColor`, `BackgroundMaskShapeLarge/Spawn`, the `UseSafezone` keys,
  `Viewport1..3Position` and every Sound item.

## Not determined

- What `PostNumbers` (map flag bit 4) does.
- Whether text honours `BlendMode`: `RedGlyphCacheElement::SetBlendMode` (P `005BF120`)
  stores a value; the render was not traced.
- Whether Model3D honours element alpha.
- VehicleSeating's modtools reader.
- Modtools Update and Render differences beyond the known ones; only key sets were
  compared. GOG was not checked.
