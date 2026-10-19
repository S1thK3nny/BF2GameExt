# HUD sizes fixed to pixels (audit, 2026-10-20)

Every size, offset or spacing the stock HUD element code fixes in pixels, with no way for a
`.hud` file to give it in another unit. This is the groundwork for the ROADMAP entry "Relative
units for every HUD size": each value would take the optional unit `Position` and `BitmapRect`
already take (`"Pixels"` by default, `"Viewport"`, `"Screen"`, `"Frame"`), Pixels staying the
default.

**How it was checked.** Every function in Phantom's HUD range (P `005ED000`–`00620000`) was
listed with its namespace. ReadData, PostReadSetup, the Setup/Refresh functions, Update and
OnRender were read for every class `HUD::Manager::Open` (P `00619970`) registers: Element,
GroupBase/Group/GroupPlayer, BorderedBox, Bar, BarBitmap, ProceduralBarBitmap, BarSegmented
(+Segment), BitmapBase/Bitmap/BitmapMasked, Map (+BitmapGroup), Model3D (+MeshInfo),
MultilineText (+Line), Target (+Target), Text, VehicleSeating (+SeatingMarker, SeatingText),
ObjectiveList (+Objective) and ViewPort. No other `HUD::Element*` class exists. The Red* layer
the HUD relies on (text layout, the bitmap shader, the interface camera, the model draw) was read
too, and the stock `.hud` files under `data\Common\hud` were checked for which keys they use.
Phantom (P) is a source rebuild: it gives the names, and modtools (M) what players run. Steam and
GOG were not checked.

## Fixed in pixels

| # | Element / piece | What | Value | Where | Key | Where a unit would go |
|---|---|---|---|---|---|---|
| 1 | VehicleSeating, its own `Position` | Where the seat stack sits | Raw numbers as **pixels** whatever mode is written; its `Scale` and `Rect` alignment are never applied | `PostReadSetup` P `0060E1D0`, M `006AC030` (no base call: no `ApplyRelativeMode`, `ApplyScale`, `SetAlignment`); `Read` P `0060E300` only runs x through `ConvertWideScreenX` | `Position` (mode read, unused) | Honour the existing 4th argument. Stock gives VehicleSeating no Position (it sits in a Group); no file in the user's mods gives it one either. |
| 2 | VehicleSeating, seat stacking (no backdrop model) | Seat n's height | **−Σ font height** of the names before it, the font's `m_uiHeight` (`RedFont`+0x0E), not times TextScale | `UpdateSeatingState` P `0060E8C0`, M `006ABC70` | none | New key, e.g. `SeatSpacing(v[, unit])` |
| 3 | VehicleSeating, names | Name's offset from its seat | **−½ font height**, unscaled | P `0060E8C0`, M `006ABC70` | none | New key, or derived from #2 |
| 4 | BorderedBox | Drawn border width and height | `Border(w, h)` drawn **w × h px**; the same numbers are texels for the UVs | ReadData P `005ED9C0`, `SetBorder` P `005EDBA0`, `SetupElements` P `005EDCE0` (nine bitmaps); M `0068D900` (one nine-slice bitmap, `SetRect9` M `008398A0`, `SetTexCoords9` M `00839A10`) | `Border` | `Border(w, h, unit)`, converting the drawn size only. Stock: (2, 2) ×18, (1, 1) ×3, (38, 38) ×2. |
| 5 | ObjectiveList | Left inset and first line's top | **8 px**, **8 px** | `SetupText` P `0061AE20`, M `006B9790` | none (no ReadData of its own) | New key, e.g. `Margin(x, y[, unit])` |
| 6 | ObjectiveList | Wrap width and height budget | Rect **− 16 px** each way | P `0061AE20`; M `006B9790` (16.0 at M `00A35A1C`) | none | Twice `Margin` |
| 7 | ObjectiveList | Gap between objectives | **2 px** | P `0061AE20`; M `006B9790` (2.0 at M `00A2A4B0`) | none | New key, e.g. `Spacing(v[, unit])` |
| 8 | ObjectiveList | Text box height | 1000 px, "unbounded" | P `0061AE20`, M `006B9790` | none | none needed |
| 9 | ObjectiveList | Text style | **gamefont_tiny**, TextScale 1, spacing 1, Left/Top; yellow active, grey completed | `Objective::Objective` P `0061A6A0`; M `006B90E0` (font handle M `00AE359C`, its neighbour 0x46F060D5 = PblHash("gamefont_tiny"); inferred) | none | `TextFont`, `TextScale`, or a `Format{}` template |
| 10 | Map, post numbers | Number's offset from its post | **5 px** up in the map's turned frame (x + sin·5, y − cos·5); the PostText template's `Scale` is dropped (1, 1) | `UpdatePostIcons` P `00603470`; M `0069F8F0` (5.0 at M `00A2AA00`) | none | New key, e.g. `PostTextOffset(d[, unit])` |
| 11 | Map, default icon size | Target\*, Post\*, PostSelect\*, Marker\*, North\* without a `BitmapRect` | **6 px square, 4 px with more than one camera** (`sSymbolScale`), stored as `BitmapRect(6/vw, 6/vh)`; North centre/bottom. The stock templates have no BitmapRect, so this is the stock size; then the class `MapScale` or the script's scale | constructor P `005FCFD0`, `sSymbolScale` P `009FDF44` = {6, 4}; M `006A0CC0`, M `00A5D07C` = {6, 4} | default only | Already overridable by the template's `BitmapRect` in any unit; an `IconSize` default is optional |
| 12 | Map, spawn screen click spots | Mouse pick radius (not drawn) | (vw/640 − 1) × 3 × sSymbolScale | P `005FCFD0`, M `006A0CC0` | none | probably none |
| 13 | ProceduralBarBitmap | Glow quad | `BitmapRect` w, h **as read, used as pixels** each frame whatever the mode; the stock hero pulse `BitmapRect(0.3, 0.000083, …, "Viewport")` is a sub-pixel quad | aligned `SetRect` P `0061BAD0` keeps the raw w/h, `OnRender` P `0061B790`, `SetRect_` P `0061BB30`; M OnRender bytes at M `006BA210` (thunk M `0040926E`) | `BitmapRect`'s mode, ignored | A new opt-in key, e.g. `GlowRect(w, h[, unit])`: honouring BitmapRect's mode would change every stock HUD's hero bar |
| 14 | BarBitmap, change strip | Height the shrinking strip ends at | `FlashyScale` **px** (default 0.2), also its final V span | ReadData P `005F7520`, `Update` P `005F7C30`; M ctor `00695DB0` (+0x470 = 0.2), WriteData M `00695B10`; M Update not found | `FlashyScale` | `FlashyScale(v, unit)`, the rectangle part only. Stock: (1) ×63, (0) ×44. |
| 15 | Text (every ElementText, templates included) | Space after each glyph | `TextCharacterSpacing(n)`, whole **px**, not times TextScale, default 1 | ReadData P `0060C850` → `SetIntercharacterSpacing`; used in `ClipString` P `008C55B0`, `WriteCache` P `008C6A20`, `GetTabSpacing` P `008C6210` | `TextCharacterSpacing` | `TextCharacterSpacing(n, unit)`, rounded (short ints). Stock: (−1) ×48. |
| 16 | Text | Tab stop pitch | `TextTabSpacing(n)` **px**; 0 = 8 × (space + spacing) | P `0060C850` → `SetTabSpacing` P `008C65F0` | `TextTabSpacing` | second argument; unused in stock |
| 17 | Text, glyphs and lines | Glyph and line size | font cell **px** × TextScale; line = round(h × TextScale.y) + leading (0; only Lua `ScriptCB_IFText_SetLeading` sets it). Drives MultilineText's line count and #2 | `GetTextHeight` P `008C62F0`, `WriteCache` P `008C6A20` | `TextFont`, `TextScale` | already handled by GameExt's `AuthoredHeight` (and stock `Scalable` + `ScriptCB_SetHUDScale`) |
| 18 | ViewPort | Offset of screen groups 1–4 | `Viewport0..3Position(x, y, z)` raw **px** | ReadData P `0061F170` | `ViewportNPosition` | 4th argument; unused in stock |

For a one-number length (#2, #3, #7, #10, #14, #15, #16) a unit needs a basis: width for a
horizontal length and height for a vertical one, as `ConvertRelativeToPixels` does per axis, or
height for text spacing to match `AuthoredHeight`. To be decided before building.

## Already relative, or not a pixel size

- **Element `Position`**: Pixels/Screen/Viewport/Frame, Pixels by default (P `005F4540`,
  `005F2AC0`, `005F3090`); z is in interface units (640 across), so resolution-free.
- **`Rotation`, `Scale`**: degrees and a plain factor (P `005F51E0`, `005F2C40`).
- **Group `Rect`**: Pixels/Screen/Viewport (no Frame); alignment (P `005FC7E0`) and
  `EventPosition` (P `005FB930`) follow its mode.
- **`BitmapRect`**: Pixels/Screen/Viewport, Viewport by default (P `005FA670`, `005FA3B0`);
  TexCoords, Mask and MaskTexCoords are UVs.
- **Bar fill**: a share of the converted width (P `005F7920`). **BarSegmented**: Rect-relative,
  the row step `(int)width / N` only truncates; angles in degrees (P `005F8FF0`).
- **ProceduralBarBitmap glow factors** 1.3, 0.05, 0.5, 1: plain; `NoiseFreq`, `NoiseRoughness`
  unused.
- **`TextBox`**: always viewport shares (× `sViewportWidth/Height`, truncated); no other mode, so
  any unit added must keep Viewport as its default.
- **Text layout literals** (the glyph clamp, the missing-space half-line advance): relative to
  the font. **Shadow style**: a clip-space (2, −2), W/554 by H/554 px, follows the screen.
- **MultilineText**: box from the Rect, line height from the Format font, `ScrollSpeed` in lines
  a second; no literals (P `006083B0`, `00608090`, `006081D0`, `006084D0`, `00607780`, `00607180`).
- **Model3D is not pixels.** One mesh unit is `W / (640 × Depth)` px, a fixed share of the
  screen's width (`RedInterfaceScreen::Render` M `00817870`, `GetDepthMultiplier` M `00839D60`
  returning 640/W, `LocalToWorld` M `00839D80`; Phantom P `008930E0`, `008DEC60`, `008DECE0`).
  Scale and Depth are plain; Position and MeshInfo Position convert by mode (P `00605BA0`,
  `00605720`). It follows the width, so it changes with the aspect ratio, and y follows the HUD's
  vertical mapping. `InheritMeshInfo` converts a non-Pixels position twice.
- **The map's view cones** (PlayerFOV\*, PlayerDirection\*): Model3D elements rebuilt each frame
  at the map point (P `00603060`), width-relative the same way. (The ROADMAP entry's "cones drawn
  at a pixel size" and "Model3D Scale in pixels" do not match the code.)
- **Map**: the backdrop's default square is 0.22/0.75/0.65 of the view height (P `00A91810`, M
  `00AD81D4`); posts, units and markers sit within ±0.5 of the backdrop and are pinned to its rim
  (P `005FF380`); icon scales are `MapScale` or the script's; `PostFlashRadius` (3) and
  `PostFlashRate` (10) are factors (the radius branch is unreachable on both builds, M
  `0069F8F0`); North sits at −½ backdrop height; PositionSmall/Large/Spawn take a mode, and
  **without the line their defaults are taken as pixels** (HUD::Matrix starts in Pixels, P
  `005F25E0`, M `00691330`; `PostReadSetup` P `00600840`); minimap radius 125 m, bounds margin
  100 m; PostText defaults gamefont_small, Center/Bottom.
- **Target**: markers at ((x+1)/2 × Rect w, (y+1)/2 × Rect h); its `Rect` is always viewport
  shares, the mode word ignored (P `0060A030`, M `006A9170`); MinScale/MaxScale plain, distances in
  metres; the lock-on 1.2, the flyer clamp 0.9–1.4 and the ×1.6 are factors; the off-screen 0.9 is
  a share of the screen.
- **Templates** (Segment, Target, VehicleSeating, Map): their own BitmapRect and Position honour
  units; VehicleSeating's hardpoint offsets are `W / (640 × depth)` px.
- **GameEvents positions**: 0–1 shares (e.g. lockOnPosition, P `00615BC0`).
- **Screen-level**: `Scalable`, the letterbox and `ConvertXForWideScreen` are relative.
- **Rounding only**: `SetRect`'s floor(v + 0.5), modtools' draw rounding, the `(int)` conversions.
- **Not reachable from a .hud**: `HUD::Editor`; AmmoGroup, DirectionsGroup, HealthGroup (not
  registered in `Manager::Open`); `ProjectedPositionInterpolator` (no callers); the text cursor.

## Differences between the builds, and what is open

- Modtools scales each seat icon by 0.4 on every state change (M `006ABC70`); Phantom does not.
- BorderedBox: Phantom nine bitmaps, modtools one rounded nine-slice (#4).
- Modtools' aspect fixes (bitmap y stretch M `00698790`, map group M `0069B920`, BarSegmented
  ellipse M `00696AB0`) and draw rounding are not pixel sizes.
- Not found or not re-read on modtools: `BarBitmap::Update` (FlashyScale's use),
  ProceduralBarBitmap's aligned SetRect, `VehicleSeating::Read`; the Text, ViewPort and
  BorderedBox ReadData addresses are from HUDElementProperties.md. The ObjectiveList font there is
  inferred from the neighbouring hash. Whether Model3D sizing (the real width) clashes with
  `AuthoredRatio`'s layout width was not checked.
