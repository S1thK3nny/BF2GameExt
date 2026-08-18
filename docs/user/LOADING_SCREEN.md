# Loading Screen System

The vanilla game reads its loading screen configuration from a munged `load.cfg`,
which cannot be overridden without replacing the base `data\_lvl_pc\load\load.lvl` file. BF2GameExt hooks
the loading screen config parser and renderer to add new parameters that work alongside the vanilla ones.

Modders can also redirect the entire loading screen to a custom `load.cfg` from
Lua with `SetLoadDisplayLevel(path)`, which resolves paths like `ReadDataFile`
does (including the `dc:` prefix for the active addon). See the
[Lua API](LUA_API.md).

## Capacity limits

The loading screen holds at most **10 models, 50 textures and 10 skeletons** from
its load lvl. Stock BF2 appends past those arrays without checking, which
corrupts the loading screen object and crashes later in the load. BF2GameExt
caps the appends instead and writes a severity-3 line to the bf2log naming what
it dropped, so an oversized `load.lvl` costs you the extra art rather than the
session.

To check for what builds these are available on, see the [compatibility table](../../README.md#compatibility).

## Custom Parameters

These work on any loading screen. They do not need `EnableBF1`.

| Parameter | Syntax | Description |
|-----------|--------|-------------|
| `RemoveToolTips` | `RemoveToolTips(1/0)` | Hides the tips box and text |
| `RemoveLoadingBar` | `RemoveLoadingBar(1/0)` | Hides the progress bar |
| `RemoveLoadingText` | `RemoveLoadingText(1/0)` | Hides the blinking "Loading" caption (bottom left) |
| `RemoveMissionName` | `RemoveMissionName(1/0)` | Hides the map name (top left) |
| `RemoveModeName` | `RemoveModeName(1/0)` | Hides the game mode name (top right) |
| `AnimatedTextures` | `AnimatedTextures(baseName, count, fps [, x, y, w, h])` | Frame-sequence animation overlay. `count` frames named `baseName0`..`baseName(count-1)`, max 64 (a larger `count` is clamped and logged), cycled at `fps` (default 10) on wall-clock time so it keeps moving while a chunk stalls, starting from frame 0 each time a loading screen appears. An `fps` of 0 or less holds on frame 0 instead of hiding the overlay. `x, y, w, h` place it in normalized screen space; both `w` and `h` must be greater than 0 or the overlay is drawn full screen. Repeat the parameter to run several overlays at once (max 8), each with its own frames, rate and rect - they draw in config order, so a later one layers over an earlier one. Also forces the screen to repaint at ~30 fps, which it otherwise only does on loader progress |
| `ScanLineTexture` | `ScanLineTexture(texName [, f1, f2, f3])` | Full-screen scanline overlay drawn last, on top of everything. `f1, f2, f3` are parsed and stored but currently unused - the overlay is always drawn at full screen. They come from the BF1 config syntax and their meaning there is not yet known |
| `LoadSoundLVL` | `LoadSoundLVL(lvlPath)` | Loads an extra .lvl for its sound definitions, so **custom** sounds are registered before the first frame renders. For BF1 loading screens, this is not needed as the sounds are still available in the base game. |

`LoadSoundLVL` resolves its path exactly the way `SetLoadDisplayLevel` does (see
the [Lua API](LUA_API.md)), so all three forms work and the trailing `.lvl` is
optional:

| Form | Resolves to |
|------|-------------|
| `LoadSoundLVL("sound\\bes")` | `data\_lvl_pc\sound\bes.lvl` |
| `LoadSoundLVL("dc:sound\\bes.lvl")` | `<addon dir>\Data\_lvl_pc\sound\bes.lvl` |
| `LoadSoundLVL("..\\..\\addon\\VTR\\data\\_LVL_PC\\sound\\bes")` | raw path relative to `data\_lvl_pc\` |

A `;group` sound-group selector may be appended, the same way script-side
`ReadDataFile("sound\\bes.lvl;bes1cw")` writes it. It is **optional**: a lvl whose
sound properties are reachable without one loads fine as a plain path.

```
LoadSoundLVL("dc:sound\\bes.lvl")          -- no group
LoadSoundLVL("dc:sound\\bes.lvl;bes1cw")   -- with a group
```

## TeamModel

A spinning 3D model on each side of the loading screen. BF2 builds the feature
and then switches it off; BF2GameExt turns it back on.

The two slots are `"1"` and `"2"` - just the two on-screen positions. All four
keys work in both `LoadDisplay()` and `Map()`, so models can differ per map.

```
LoadDisplay()
{
    TeamModel("1", "gal_prp_death_star")
    TeamModel("2", "gal_prp_death_star")
    TeamModelScale(100)
    TeamModelRotationSpeed(1.0)

    TeamModelOffset("1", 0.70, 0.75)
    TeamModelOffset("2", 0.88, 0.75)
}

Map("VTRg_con")
{
    TeamModel("1", "gal_prp_rebel_logo")
    TeamModel("2", "gal_prp_imperial_logo")
}
```

| Parameter | Syntax | Description |
|-----------|--------|-------------|
| `TeamModel` | `TeamModel("1"\|"2", "modelName")` | Binds a model to one of the two slots |
| `TeamModelOffset` | `TeamModelOffset("1"\|"2", x, y)` | Places that slot. `x`/`y` are screen fractions from the top left, `+x` right and `+y` down. `(0.5, 0.5)` is the centre |
| `TeamModelScale` | `TeamModelScale(f)` | Multiplier on the model size, shared by both |
| `TeamModelRotationSpeed` | `TeamModelRotationSpeed(f)` | Spin rate. The two models counter-rotate |

The slot must be **quoted**. A bare `1` is a number, not a string, and cannot be read as one.

It is not necessary to have two TeamModels. One is enough, and the other slot can be left empty. The two slots are independent: they can have different models and offsets. Scale and rotation speed are shared.

The model must be in `load.lvl`, via a `model` REQN in `load.req`:

```
REQN
{
    "model"
    "gal_prp_death_star"
}
```

> ### Do not reuse loading screen assets in the mission
>
> **Any model or texture you load for the loading screen will NOT appear in the
> mission if the mission uses it too.** The loading screen frees its own assets
> when it tears down, and that takes the shared copy with it - the mission is
> left referencing something that no longer exists.
>
> Give loading screen models and textures their own names, distinct from anything
> the map loads. This is stock engine behaviour, not something the extension
> introduces or can work around.

<img width="800" height="450" alt="TeamModelsPreview" src="https://github.com/user-attachments/assets/e90a478f-6863-4628-9c8c-b99ebf256936" />

<sub>*Team Model example from [`GameAssets/Examples/LoadingScreen-TeamModels`](../../GameAssets/Examples/LoadingScreen-TeamModels). Does not come with the background image*</sub>

## BF1 Sequence

Through a lot of trial and error, the BF1 loading screen was reverse engineered to a point where it can be reproduced in BF2. The sequence is a series of zooms into a planet, into the atmosphere, further into the surface and finally into the map itself. The sequence is controlled by a set of parameters in the `load.cfg` file, which define the planets, their textures, and the sounds that play during the zooms. Everything in this section requires `EnableBF1()`; without it, these parameters are parsed and discarded.

| Parameter | Syntax | Description |
|-----------|--------|-------------|
| `EnableBF1` | `EnableBF1(1/0)` | Master switch for the BF1-style zoom animation sequence |
| `PlanetLevel` | `PlanetLevel(index, texName, x, y, w, h)` | Per-level planet texture at a normalized screen rect. Place inside `PC()` or `Map()` |
| `ZoomSelectorTextures` | `ZoomSelectorTextures(horz, vert, corner)` | Texture strips for the 16-quad crosshair frame around the zoom target |
| `ZoomSelectorTileSize` | `ZoomSelectorTileSize(halfW [, halfH])` | Half-dimensions of each crosshair tile in normalized screen space |
| `XTrackingSound` | `XTrackingSound(soundName)` | Looping sound during horizontal band convergence |
| `YTrackingSound` | `YTrackingSound(soundName)` | Looping sound during vertical band convergence |
| `ZoomSound` | `ZoomSound(soundName)` | One-shot sound on zoom-in phase |
| `TransitionSound` | `TransitionSound(soundName)` | One-shot sound on planet transition |
| `BarSound` | `BarSound(soundName)` | Periodic sound when no planet animation is active |
| `BarSoundInterval` | `BarSoundInterval(seconds)` | Seconds between BarSound replays |

These take any registered sound name, but **the original BF1 loading screen
sounds are still in the retail game** - `load_zoom`, `load_transition`,
`load_xtracking`, `load_ytracking` and `load_bar` live in the `global.snd` bank
inside the stock `core.lvl`, which is resident for the whole session, so naming
them is all it takes.

https://github.com/user-attachments/assets/c37be45d-8b5d-4d10-90cf-85e479a6b585

<sub>*BF1 Loading Screen example from [`GameAssets/Examples/LoadingScreen-BF1`](../../GameAssets/Examples/LoadingScreen-BF1).*</sub>