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
| `LoadSoundLVL` | `LoadSoundLVL(lvlPath)` | Loads an extra .lvl for its sound definitions, so custom sounds are registered before the first frame renders |

Each `Remove*` parameter hides exactly one element and is independent of the
others. Combine them for a fully blank screen:

```
RemoveToolTips(1)
RemoveLoadingBar(1)
RemoveLoadingText(1)
RemoveMissionName(1)
RemoveModeName(1)
```

> **Changed behaviour.** `RemoveLoadingBar` used to hide the game's four corner
> screen groups, which between them also hold the map name, the mode name and the
> "Loading" caption. It now hides only the progress bar, as its name and this
> table always said. If you were relying on the old behaviour, add the three new
> parameters above.

`LoadSoundLVL` is listed here because it is a general-purpose hook, but in
practice the only consumers today are the BF1 sound parameters below.

### BF1 Sequence

The BF1-style zoom animation: a planet is framed by a crosshair selector, the
view zooms into it, and the next planet takes over. Everything in this section
requires `EnableBF1(1)`; without it these parameters are parsed and discarded.

| Parameter | Syntax | Description |
|-----------|--------|-------------|
| `EnableBF1` | `EnableBF1(1/0)` | Master switch for the BF1-style zoom animation sequence |
| `PlanetLevel` | `PlanetLevel(index, texName, x, y, w, h)` | Per-level planet texture at a normalized screen rect. Place inside `PC()` or `Map()` |
| `AnimatedTextures` | `AnimatedTextures(baseName, count, fps [, x, y, w, h])` | Frame-sequence animation overlay. Frames named `baseName0`..`baseName(count-1)` |
| `ScanLineTexture` | `ScanLineTexture(texName [, f1, f2, f3])` | Full-screen scanline overlay drawn on top of everything |
| `ZoomSelectorTextures` | `ZoomSelectorTextures(horz, vert, corner)` | Texture strips for the 16-quad crosshair frame around the zoom target |
| `ZoomSelectorTileSize` | `ZoomSelectorTileSize(halfW [, halfH])` | Half-dimensions of each crosshair tile in normalized screen space |
| `XTrackingSound` | `XTrackingSound(soundName)` | Looping sound during horizontal band convergence |
| `YTrackingSound` | `YTrackingSound(soundName)` | Looping sound during vertical band convergence |
| `ZoomSound` | `ZoomSound(soundName)` | One-shot sound on zoom-in phase |
| `TransitionSound` | `TransitionSound(soundName)` | One-shot sound on planet transition |
| `BarSound` | `BarSound(soundName)` | Periodic sound when no planet animation is active |
| `BarSoundInterval` | `BarSoundInterval(seconds)` | Seconds between BarSound replays |
