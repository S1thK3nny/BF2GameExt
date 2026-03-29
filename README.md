# BF2GameExt

A DLL extension for Star Wars Battlefront II (2005) that exposes new modding capabilities by hooking into the game engine at runtime. Injects into BF2_modtools.exe and adds custom Lua functions, ODF properties, loading screen parameters, bug fixes, and engine limit extensions.

## Goal

Fully reverse-engineer Star Wars Battlefront II (2005) and produce a complete Ghidra decompilation of the game engine - then use that knowledge to push the boundaries of what's possible in SWBF2 modding. Re-enable cut features, fix long-standing engine bugs, and add entirely new capabilities through the game's existing Lua and ODF systems.

Aspyr's Classic Collection, outsourced to Dragons Lake Entertainment, failed to deliver even basic fixes the community had been requesting for years, while simultaneously taking from modders without credit. Every feature listed below was achieved by patching raw assembly in a 20-year-old binary without source code access. Consider what a studio with the actual source code could have accomplished in a single afternoon and then ask why they didn't.

## Table of Contents

- [Features](#features)
  - [Engine Limit Extensions](#engine-limit-extensions)
  - [Loading Screen System](#loading-screen-system)
  - [Soldier Systems](#soldier-systems)
  - [Weapon Systems](#weapon-systems)
  - [Vehicle Fixes](#vehicle-fixes)
  - [Event Callbacks](#event-callbacks)
  - [Web Requests](#web-requests)
  - [Additional Debug Commands](#additional-debug-commands)
- [Supported Executables](#supported-executables)
- [Installation](#installation)
- [Configuration](#configuration)
- [Building from Source](#building-from-source)
- [Project Structure](#project-structure)
- [Contributors](#contributors)

## Features

### Engine Limit Extensions
Automatic binary patches applied on load:

- **Heap Memory Extension** - Increases RedMemory heap from 64MB to 256MB, drastically reducing out-of-memory crashes
- **DLC Mission Limit** - Increases from 500 to 4096, allowing more mods installed simultaneously
- **Sound Layer Limit** - Prevents crashes on maps with many flyers/entities using EngineSound
- **GC Visual Limits** - Raises Galactic Conquest per-frame rendering limits: pathway beams from 64 to 256, particle icons from 128 to 512. Fixes pathways and fleet/planet icons silently disappearing on modded GC maps with many planets

### Loading Screen System
The vanilla game reads a loading screen configuration from a munged `load.cfg`, but it cannot be overridden without replacing the base game file. BF2GameExt hooks into the `LoadDisplay` config parser and renderer to add new parameters that work alongside vanilla ones. Modders can also redirect the entire loading screen to a custom `load.cfg` via `SetLoadDisplayLevel(path)` in Lua.

#### Custom Parameters

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
| `LoadSoundLVL` | `LoadSoundLVL(lvlPath)` | Path to an .lvl containing sound definitions for the above |
| `RemoveToolTips` | `RemoveToolTips(1/0)` | Hides the tips box and text. Works independently of EnableBF1 |
| `RemoveLoadingBar` | `RemoveLoadingBar(1/0)` | Hides the progress bar. Works independently of EnableBF1 |

#### Lua Functions

| Function | Description |
|----------|-------------|
| `SetLoadDisplayLevel(path)` | Redirects to a custom load.cfg (call from script root or ScriptPreInit) |

### Soldier Systems
- **Prone Stance** - Re-enables, fixes, and adapts the cut prone posture system. Double-tap crouch to go prone, any crouch press to stand back up. Includes a terrain rotation fix that prevented prone from working on slopes. Lua: `EnableProne(enable)`
- **Multiple First-Person Animation Banks** - Allows each soldier class to use its own first-person animation bank instead of sharing one global set. Supports partial banks where missing animations fall through to defaults. ODF: `FirstPersonAnimationBank = bankname`
- **Unit Class Removal** - Dynamically remove classes from a team's spawn menu at runtime. Lua: `RemoveUnitClass(team, class)`

### Weapon Systems
- **Barrel Fire Origin Fix** - Fixes ordnances spawning from `bone_head` instead of `hp_fire` on WeaponCannon. Forces projectiles to originate from the actual barrel hardpoint. Lua: `SetBarrelFireOrigin(enable)`
- **Disguise Model Override** - Allows WeaponDisguise to swap the soldier's visual model to a specific GameModel instead of cloning the first enemy soldier. ODF: `DisguiseModel = modelname`
- **Grappling Hook** *(experimental)* - Re-enables the cut grappling hook weapon with custom pull physics, slingshot mechanic (jump mid-pull to launch), and rope cable rendering. ODF properties: `PullSpeed`, `MaxRange`

### Vehicle Fixes
- **Carrier/Flyer Fixes** - Fixes landing state oscillation, cargo attachment, LOD rendering, and animation override for EntityCarrier
- **Flyer Boost Animation** - If a flyer's AnimationName bank contains an animation named `boost`, it will automatically play when boosting with a smooth blend transition. Frame 0 should be the normal flying pose and the final frame the full boost pose.

### Event Callbacks
- **OnCharacterExitVehicle** - Register Lua callbacks that fire when soldiers dismount vehicles, with filtering by name, team, or class. Lua: `OnCEV(fn)`, `OnCEVName(name, fn)`, `OnCEVTeam(team, fn)`, `OnCEVClass(class, fn)`, `ReleaseCEV(handle)`

### Web Requests
Make HTTP requests directly from Lua scripts - enables integration with external APIs, telemetry, live configuration, and more. All from within singleplayer or multiplayer missions.

- `HttpGet(url)` / `HttpPut(url, body)` / `HttpPost(url, body)` - Synchronous requests, return response body
- `HttpGetAsync(url)` / `HttpPutAsync(url, body)` / `HttpPostAsync(url, body)` - Fire-and-forget on background threads

### Additional Debug Commands
Extra commands for the in-game console in the ModTools (`~`):

- `RenderHoverSprings` - Visualize hover vehicle spring compression with colored wireframe spheres
- `ShowWeaponRanges` - Draw weapon AI range circles (MinRange, OptimalRange, MaxRange) around soldiers

## Supported Executables

- **BF2_modtools** - Full support (modding executable from the official mod tools)
- **[GoG](https://www.gog.com/en/game/star_wars_battlefront_ii)** - Binary patches only
- **[Steam](https://store.steampowered.com/app/6060)** - Binary patches only

Lua extensions and hooks currently target BF2_modtools only. GoG/Steam support for the full feature set is planned.

## Installation

1. Build `BF2GameExt.dll` (PatcherDLL project) and `BF2GameExt.exe` (BF2GameExt project)
2. Place both in your `Star Wars Battlefront II Classic` folder (outside of `GameData`)
3. Run `BF2GameExt.exe` and patch a **copy** of BF2_modtools.exe
4. The patcher places the DLL into `GameData` automatically
5. Done! Run the game through the patched executable and have fun :)

## Configuration

Features are configured through `load.cfg` parameters, ODF properties, and Lua functions. See the [Examples](Examples/) folder for ready-to-use configurations with inline documentation.

## Building from Source

Requirements:
- Visual Studio 2022 (v143 toolset)
- Windows 10 SDK
- C++20

```
git clone https://github.com/S1thK3nny/BF2GameExt.git
```

Open `BF2GameExt.sln` and build the `PatcherDLL` project. Output goes to `bin\Debug\` or `bin\Release\`.

## Project Structure

```
PatcherDLL/src/
  core/               Entry point, patching, address registry, resolve helpers
  entity/             EntitySoldier, EntityFlyer, cloth collision fixes
  weapon/             Grappling hook, disguise model override
  lua/                Lua API hooks and custom function registration
  loading_screen/     Loading screen system (config, renderer, lifecycle)
  shell/              Galactic Conquest visual limit extensions
  debug_commands/     Console debug visualization commands
  util/               File helpers, slim_vector, class limit patch
```

## Contributors

- **[PrismaticFlower](https://github.com/PrismaticFlower)** - Author of the original project this was forked from. Creator of numerous essential tools for the BF2 modding community.
- **[phantom567459](https://github.com/phantom567459)** - BF1 engine research and decompilation. His work on the BF1 binary made the loading screen system possible.
- **[Ryan Hank](https://github.com/RJP1992)** - Reverse engineering, decompilation, implementation.
- **[S1thK3nny](https://github.com/S1thK3nny)** - Reverse engineering, decompilation, implementation.
