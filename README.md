# BF2GameExt

A DLL extension for Star Wars Battlefront II (2005) that exposes new modding capabilities by hooking into the game engine at runtime. Injects into BF2_modtools.exe and adds custom Lua functions, ODF properties, loading screen parameters, bug fixes, and engine limit extensions.

## Goal

Fully reverse-engineer Star Wars Battlefront II (2005) and produce a complete Ghidra decompilation of the game engine - then use that knowledge to push the boundaries of what's possible in SWBF2 modding. Re-enable cut features, fix long-standing engine bugs, and add entirely new capabilities through the game's existing Lua and ODF systems.

Aspyr's Classic Collection, outsourced to Dragons Lake Entertainment, failed to deliver even basic fixes the community had been requesting for years, while simultaneously taking from modders without credit. Every feature listed below was achieved by patching raw assembly in a 20-year-old binary without source code access. Consider what a studio with the actual source code could have accomplished in a single afternoon and then ask why they didn't.

## Table of Contents

- [Features](#features)
  - [Engine Limit Extensions](#engine-limit-extensions)
  - [Engine & Rendering Fixes](#engine--rendering-fixes)
  - [Loading Screen System](#loading-screen-system)
  - [Soldier Systems](#soldier-systems)
  - [Weapon Systems](#weapon-systems)
  - [Vehicle Additions and Fixes](#vehicle-additions-and-fixes)
  - [AI Systems](#ai-systems)
  - [Additional Debug Commands](#additional-debug-commands)
  - [Controller Support](#controller-support)
- [Lua API](#lua-api)
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
- **Sound Memory Limit** - Increases sound RAM from 32MB to 256MB
- **Particle Cache** - Increases cached particle limit from 300 to 1200
- **Object Limit** - Doubles EntityEx hash table from 1024 to 2048 buckets, raising the active object cap
- **Combo Animation Limit** - Increases from 30 to 90 entries, with expanded animation index range
- **High-Res Animation Limit** - Increases from 50 to 12,800 entries
- **Soldier Animation Bank Limit** - Raises the per-class loaded animation-bank cap from 18 to 128 (the `Out of space for soldier animation banks (max 18)` error). The 18-slot inline array is relocated to a per-class heap buffer and every consumer redirected to it. Note: a bank name split into numbered sub-banks (`human_0`..`human_N`) counts as one slot each, so this lets a class load far more split banks than before. **Caveat — a separate, deeper limit is NOT raised:** the number of *distinct bank names* is hard-capped at **16** and *weapon types* at **20** (fused into fixed-size stack arrays in `SetupBodyMasks`; raising them is impractical and the bank-name cap cannot be raised without corrupting the stack). Keep modded soldier classes to **≤16 distinct bank names and ≤20 weapon types**.
- **String Pool** - Increases string pool from 32KB to 128KB, preventing crashes in debug builds with heavy string usage
- **Matrix/Item Pool** - Extends matrix pool to 256x original capacity
- **Renderer Cache** - Increases particle renderer cache from 15 to 120 entries (the array relocation lives in the Particle Cache patch set; the 15-slot allocation clamp is raised by GC Visual Limits when both are enabled)
- **GC Visual Limits** - Raises Galactic Conquest per-frame rendering limits: pathway beams from 64 to 256 (255 on Steam), particle icons from 128 to 512. Also spills the shared `RedParticleRenderer` batching cache into spare slots when full — without this, all pathway beams share one 200-entry texture cache and silently stop rendering at ~50 beams regardless of the buffer size. Fixes pathways and fleet/planet icons silently disappearing on modded GC maps with many planets. INI: `[LimitIncreases] GCVisualLimits=1`
- **SkyObjectClass Limit** - Removes the hard cap on `SkyObjectClass` instances by neutralizing the global instance counter, allowing sky domes/backdrops with many objects. Port of PrismaticFlower's upstream fix. INI: `[Fixes] SkyObjectLimit=1`

### Engine & Rendering Fixes
General engine bug fixes ported from PrismaticFlower's upstream, applied on all supported builds (modtools/Steam/GOG):

- **PropGenerator Loop Fix** - `PropGenerator::Update` (procedural foliage) could branch past its cluster-object array bounds check at very high FOVs and read past the array end, crashing. The patch redirects that branch back to the bounds check. INI: `[Fixes] PropGeneratorLoopFix=1`
- **Terrain Texture Fix** - The terrain shader caches two `RedTexture*` globals (the null detail map and white fallback) that are only assigned on maps that have a terrain detail map. Switching in a playlist from a detail-map map to one without leaves a stale pointer → garbage/crash. The fix hooks `ReadTerrain` to re-resolve both textures from the hash table before every terrain load, so they're always valid. (Also fixes an upstream copy-paste bug that fed the detail-map hash into the white slot.) INI: `[Fixes] TerrainTextureFix=1`
- **Game Logging Enablement** - Retail builds (Steam/GOG) ship the engine's `BFront2.log` file logging compiled in but disabled. This re-enables it without needing the `/log` command-line flag, useful for diagnosing crashes and mod issues. No-op on modtools (which always logs). INI: `[Features] GameLogging=0` *(off by default)*
- **Enable Sound Warnings** - `GameSound::SetID` guards its "Unable to find sound property `0x%08x`" warnings (fired when an ODF references a sound name that isn't loaded) behind the read-only `GameSoundEngine::gEnableSoundWarnings` flag, which defaults to off. Setting this writes the flag so those missing-sound warnings surface in the log. Modtools only: retail builds compiled the warning code out entirely, so there is nothing to enable there. INI: `[Features] EnableSoundWarnings=0` *(off by default)*
- **Blur Downsize Clamp** - The blur effect renders into a target downsized by a factor tuned for 2005-era resolutions; at modern resolutions the downsized target grows far larger than intended, making the blur weaker-looking and more expensive. Clamps the downsized resolution to 512px on its long edge before each render. INI: `[Fixes] BlurDownsizeClamp=1`
- **Screenshot Fix** - Print Screen crashes the retail (Steam/GOG) builds. Replaces the broken `Screenshot::RequestScreenshot` with a clean backbuffer capture written to `ScreenShots\screenshot_NNNN.tga`. No-op on modtools. INI: `[Fixes] ScreenshotFix=1`
- **Error Dialog Fix** - The retail builds are missing the dialog template resource that `RedWarning::DialogBoxMessage` uses, so fatal-error dialogs silently fail and the game just exits. Redirects the `DialogBoxParamA` call to an equivalent template embedded in BF2GameExt.dll (the game's own dialog proc is intact). No-op on modtools. INI: `[Fixes] ErrorDialogFix=1`
- **DLC Mission List Initialization Fix** *(experimental, off by default)* - Launching straight into a mod map from the commandline fails because the addon/DLC mission list is only populated by the shell state's Enter. When a mission is entered without the shell ever having run, the shell state is entered and immediately exited first, giving addon scripts their full, normal Lua context. Ported from upstream but not yet confirmed working on retail builds — commandline addon-mission launches still fail there. INI: `[Fixes] DLCMissionInitFix=0`
- **HUD Widescreen Reticle Correction** - The vanilla letterbox transform in `HUD::Manager::Update` scales and offsets *every* HUD element on widescreen displays, which pushes the aim reticle off the true 3D aim point (the error grows toward the screen edges). This pre-distorts only the reticle Y in `ReticuleDisplay::Update` so it lands correctly after the letterbox transform; all other HUD elements are untouched. Handled per build (x87 constant redirect on modtools, SSE `MULSS`/`ADDSS` rewrite on retail). INI: `[Fixes] ReticleCorrection=-1` (auto; `0` disables, or a manual `0..1` strength).

### Loading Screen System
The vanilla game reads a loading screen configuration from a munged `load.cfg`, but it cannot be overridden without replacing the base game file. BF2GameExt hooks into the `LoadDisplay` config parser and renderer to add new parameters that work alongside vanilla ones. Modders can also redirect the entire loading screen to a custom `load.cfg` from Lua. See [Lua API](#lua-api).

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

### Soldier Systems
- **Prone Stance** - Re-enables, fixes, and adapts the cut prone posture system. Double-tap crouch to go prone, any crouch press to stand back up. Includes a terrain rotation fix that prevented prone from working on slopes. This requires a modified ingame.lvl to load the prone animations, human_5. Please disable otherwise. INI: `[Features] Prone=1`
- **Multiple First-Person Animation Banks** - Allows each soldier class to use its own first-person animation bank instead of sharing one global set. Supports partial banks where missing animations fall through to defaults. ODF: `FirstPersonAnimationBank = bankname`
- **First-Person Sprint Animation** - The engine's FP state machine has no sprint state — it plays the run animation at higher speed. This adds support for a distinct sprint animation per weapon class. If `<bank>_rifle_sprint` (or `_bazooka_sprint`, `_tool_sprint`) exists in the animation bank, it will be used instead of the run animation while sprinting. Works with custom FP banks. Sprint animations are optional — if absent, the default run behavior is unchanged
- **Animation Bank Appending** - Allows animation banks to be extended with additional numbered sub-banks across multiple .lvl files. The engine's `AnimationFinder::_AddBank` only scans for sub-banks once during the first .lvl load, so late-loaded sub-banks (e.g. `human_5` from a modified `ingame.lvl` after a mod's `dc:ingame.lvl` already ran `_AddBank("human")`) are silently ignored. The fix hooks `_AddBank` to retroactively append any sub-banks that exist in the hash table but weren't picked up during the initial scan. Works for any bank, not just `human`
- **Extra Override Textures** - The stock engine gives soldier classes two runtime texture-override slots (`OverrideTexture`, `OverrideTexture2`), each swapping the texture on a specially-named model material (`override_texture`, `override_texture2`). This adds three more; `OverrideTexture3`, `OverrideTexture4`, `OverrideTexture5`. The extra slots piggyback on the stock render path, so `OverrideTexture` (slot 1) must also be set for them to apply. Per-class; not inherited through `ClassParent` (set them on the concrete soldier class). This number can be increased further to any count. It has not been tested with more than 5, but the underlying system is fully dynamic and should work for any number of override textures.
- **Unit Class Removal** - Dynamically remove classes from a team's spawn menu at runtime. See [Lua API](#lua-api).

### Weapon Systems
- **Barrel Fire Origin Fix** - Fixes ordnances spawning from `bone_head` instead of `hp_fire` on WeaponCannon. Forces projectiles to originate from the actual barrel hardpoint. INI: `[Fixes] BarrelFireOriginFix=1`
- **Disguise Model Override** - Allows WeaponDisguise to swap the soldier's visual model to a specific GameModel instead of cloning the first enemy soldier. ODF: `DisguiseModel = modelname`
- **Grappling Hook** *(experimental)* - Re-enables the cut grappling hook weapon with custom pull physics, slingshot mechanic (jump mid-pull to launch), and rope cable rendering. ODF properties: `PullSpeed`, `MaxRange`
- **Shield Channel Fix** - Fixes WeaponShield activating on any fire button press regardless of which weapon is selected. The shield's Update override reads the fire trigger directly without checking if it's the active weapon for its channel.
- **Animated Lightsaber Textures** - Ports the Xbox version's `AnimTexture1`/`AnimTexture2`/`AnimTexture3` WeaponMelee blade properties, which give a lightsaber blade a 4-frame animated texture cycle (PC blades use a single static texture). Hooks `WeaponMeleeClass::SetProperty` to capture the anim texture hashes per blade and `_RenderLightsabre` to substitute the current frame at render time — the blade struct is never modified. ODF (under the blade's WeaponMelee section): `AnimTexture1 = tex_frame2`, `AnimTexture2 = tex_frame3`, `AnimTexture3 = tex_frame4`.

### Vehicle Additions and Fixes
- **Droideka Ball Mode Toggle** - The droideka is the only playable walker/vehicle hybrid, so its roll is welded to the chassis: a mod that wants it as a plain walking unit can ask the player not to press the button, but the AI still rolls. `DisableBallMode = 1` on a walkerdroid (EntityDroideka) class takes the roll away outright, for the AI as much as the player. Per-class, so a rolling and a non-rolling droideka can coexist, and it inherits through `ClassParent` like a stock property. Off by default. ODF: `DisableBallMode = 1`
- **Droideka Death Animation Fix** - Droidekas never play their death animation, even though every stock droideka bank defines one and the engine loads it. A bug in the death handling cut the animation off after a single frame, so the droideka just exploded instantly — while walkers like the ATST/ATTE/ATAT played theirs correctly. The fix lets `death01` run to completion before the droideka dies. Rolling droidekas still explode instantly (by design), and banks with no death animation are unaffected. INI: `[Fixes] DroidekaDeathAnimation=1`
- **Flyer Boost Animation** - If a flyer's AnimationName bank contains an animation named `boost`, it will automatically play when boosting with a smooth blend transition. Frame 0 should be the normal flying pose and the final frame the full boost pose.
- **Carrier Fixes** - Originally an unused class, the Carrier Fixes address landing state oscillation, cargo attachment, LOD rendering, and animation override for EntityCarrier, making it viable for modders to use as a VehiclePad.
- **Vehicle First/Third Person Toggle** - Fixes change-view being silently dropped on hovers and walkers (EntityHover, EntityWalker, and their CommandHover / CommandWalker AI wrappers). Each class's Controllable-aimer subobject shipped with a const-true stub at vtable+0x3C, which the toggle gate read as "view change suppressed", so ground vehicles were stuck in third person unless `ForceMode` was set in the ODF. The fix repoints that slot at the const-false thunk already present at +0x40 of the same vtables.
- **CreateEntity Vehicle Weapons Fix** - Vehicles spawned via Lua `CreateEntity` would work fine except for their weapons silently no-oping. Stock `CreateEntity` only calls `EntityClass::Create`; it skips the team-set and activate steps that `VehicleSpawn::UpdateSpawn` performs after creation. Without a team set, `OrdnanceFactory` has no `DamageOwner` and refuses to spawn projectiles. The fix detours `CreateEntity` and runs the post-create sequence (`SetTeam`, spawn-team / group bits at `+0x234`, controllable activate) automatically. Adds an optional 4th argument: `CreateEntity(class, matrix, name [, team])`, with it defaulting to 0 if omitted. Back-compatible with existing call sites.
- **LandOnArrival Fix** - The `.pth` path node property `LandOnArrival` is parsed for every node but never worked as authored, due to two engine bugs: `EntityPathFollower::Update` only checks the flag on the first path node (an index gate skips it for every later node), and `mbLandNow` is never cleared once set, so a landed flyer re-triggers `Land()` every frame and can never take off or be used again. The fix NOPs the index gate so arrival at **any** flagged node lands the flyer, and hooks `EntityFlyer::Land` to clear the flag and release the path follower, thus making the vehicle enterable by AI and players after a path-scripted landing.
- **Flyer Engine Sound Fix** - AI flyers following waypoint paths have stuttering/warbling engine audio. The path follower moves along a Catmull-Rom spline whose parametric speed varies slightly every frame, and the speed derivative fed to `VehicleEngine::Update` amplifies that noise into large pitch/volume swings. The fix smooths the `speedRatio` and `acceleration` inputs with a fast exponential moving average (τ = 0.05s) so the path-following jitter is damped out while player-controlled flight remains effectively unaffected.
- **Self-Piloted Hover Crash Fix** - A hover vehicle with `PilotType = "self"` in its ODF crashes the game outright. `EntityHover::UpdateIndirect` (the hover's AI obstacle-avoidance) fetches the vehicle's pilot and calls `pilot->GetGameObject()` with no null check, to exclude it from a collision raycast. Every stock hover is soldier-entered (`PilotType = vehicle`) so the pilot is always valid and Pandemic never guarded the self-piloted case - a self-piloted hover has no separate pilot, so the getter returns null and the game dereferences it. The fix routes that getter call through a shim that substitutes the hover itself when there is no pilot, so the exclusion resolves harmlessly. A second null-pilot crash on the same vehicles - dereferencing the missing pilot while acknowledging a unit order - is guarded the same way, skipping the order-acknowledge when there is no pilot.

### AI Systems
- **Dead Body Shooting Control** - Vanilla `CombatHelper::DeadBodyCheck` makes **Alliance** units (the team whose `Team::mSide == 1`) break off to walk up to and fire on nearby soldier corpses. Two INI toggles control this behavior:
  - `[Features] DisableDeadBodyShooting=1` *(default on)* - Stops the behavior entirely, for every side, so no one ever shoots dead bodies. Overrides the all-factions toggle.
  - `[Features] DeadBodyShootingAllFactions=1` *(default off)* - Extends the behavior to all factions instead of just Alliance. Ignored while `DisableDeadBodyShooting=1`.

### Additional Debug Commands
Extra commands for the in-game console in the ModTools (`~`):

- `RenderHoverSprings` - Visualize hover vehicle spring compression with colored wireframe spheres
- `ShowWeaponRanges` - Draw weapon AI range circles (MinRange, OptimalRange, MaxRange) around soldiers

### Controller Support
- **Gamepad Bindings** - Five control modes (Unit, Vehicle, Flyer, Hero, Turret) with configurable button layouts. Does not affect keyboard/mouse bindings. INI: `[Controller.*]` sections
- **Aim Assist** - Xbox-style aim assist ported from the console version's dead code. Proximity friction, auto-lock-on-hit, target tracking, and directional friction. Controller-only, singleplayer-only. INI: `[AimAssist]`
- **Rumble** - Controller vibration on weapon fire and damage. INI: `[Controller] Rumble=1`

## Lua API

All functions below are registered globally and callable from any mission, ScriptInit, or shell script. BF2_modtools only.

### Character & Weapon Queries

| Function | Description |
|----------|-------------|
| `GetCharacterWeapon(charIndex, channel)` | Returns the ODF name of the weapon currently held in the given channel (0 = primary, 1 = secondary, ...). Returns nil if the slot is empty. |
| `SetCharacterWeapon(charIndex, odfName [, channel])` | Replaces the active weapon in a channel (0 = primary, 1 = secondary) with another already-loaded weapon ODF. Builds a real Weapon through the engine's own factory and destroys the old one — ammo, animation stance, and aimer all come out correct. Singleplayer only; slots using `WeaponShareAmmo`/`WeaponShareEnergy` are refused, as are weapons the unit's animation bank has no animmap for (e.g. giving a jedi-bank unit a rifle). The old weapon is kept in that case. Works in first person. Melee-family weapons (sabers, saber throw) are untested and unsupported. Returns 1 on success, nil on failure. |
| `GetWeaponAmmo(charIndex [, channel])` | Returns four numbers: `curClip, numClips, maxClips, roundsPerClip` for the active weapon in the channel (default 0). Ammo is tracked in **clips**, with `curClip` being a fractional 0.0–1.0 of one loaded clip. |
| `SetWeaponAmmo(charIndex, curClip [, numClips [, channel]])` | Writes `curClip` (fractional 0.0–1.0) and optionally `numClips` (spare clips) on the active weapon. Pass nil for `numClips` to leave it untouched. |

### Spawn Menu

| Function | Description |
|----------|-------------|
| `RemoveUnitClass(team, className)` | Removes a unit class from a team's spawn menu at runtime. Compact-shifts the team's class arrays to preserve order. |

### Animation

| Function | Description |
|----------|-------------|
| `ReapplyAnimations()` | Re-runs the full animation bank assignment for every soldier class. **Warning: leaks ~250 `SoldierAnimation` pool entries and ~100 MB of Heap 5 per call — never call it repeatedly, and never after `SetCharacterWeapon` (not needed since v6).** Only use once after a deliberate hotload that changes `FirstPersonAnimationBank` ODF values. |

### Event Callbacks

Register Lua callbacks that fire when soldiers dismount vehicles. All registration functions return a handle that can be passed to `ReleaseCharacterExitVehicle` to unsubscribe.

| Function | Description |
|----------|-------------|
| `OnCharacterExitVehicle(fn)` | Fires on every character exiting any vehicle. |
| `OnCharacterExitVehicleName(name, fn)` | Filtered to vehicles with the given entity name. |
| `OnCharacterExitVehicleTeam(team, fn)` | Filtered to a specific team index. |
| `OnCharacterExitVehicleClass(className, fn)` | Filtered to a specific vehicle ODF class. |
| `ReleaseCharacterExitVehicle(handle)` | Unregister a previously-registered callback. |

### Loading Screen

| Function | Description |
|----------|-------------|
| `SetLoadDisplayLevel(path)` | Redirects the loading screen to a custom `load.cfg`. Call from script root or ScriptPreInit. |

### Rendering

| Function | Description |
|----------|-------------|
| `SetFogEnable(0/1)` | Toggles the D3D fog render state. |
| `SetFogRange(start, end)` | Sets near/far fog distances. |

### HTTP

Make HTTP requests directly from Lua. Useful for telemetry, live configuration, or external API integration in either singleplayer or multiplayer missions.

| Function | Description |
|----------|-------------|
| `HttpGet(url)` | Synchronous GET. Returns response body as a string, or nil on failure. |
| `HttpPut(url, body)` | Synchronous PUT. Returns response body. |
| `HttpPost(url, body)` | Synchronous POST. Returns response body. |
| `HttpGetAsync(url)` | Fire-and-forget GET on a background thread. |
| `HttpPutAsync(url, body)` | Fire-and-forget PUT. |
| `HttpPostAsync(url, body)` | Fire-and-forget POST. |

### Debug

| Function | Description |
|----------|-------------|
| `DumpAimerInfo(charIndex)` | Prints the soldier's Aimer state (fire point, yaw/pitch, target) to the game debug log. |

## Supported Executables

- **BF2_modtools** - Full support (modding executable from the official mod tools)
- **[GoG](https://www.gog.com/en/game/star_wars_battlefront_ii)** - Binary patches + ported hook features
- **[Steam](https://store.steampowered.com/app/6060)** - Binary patches + ported hook features

A runtime build-dispatch layer resolves per-build addresses, so an increasing set of hook-based features now runs on Steam and GOG in addition to the binary patches — including prone, aim assist, animation bank appending, the terrain/prop/sky fixes, the vehicle first/third-person toggle, and the cloth (cape) collision fixes. The full Lua API and the remaining hooks currently target BF2_modtools only; broader GoG/Steam coverage is ongoing.

## Installation

### Method 1 — DInput8 Proxy (recommended)
No exe patching required. Drop these files next to your game executable (inside `GameData`):

- `dinput8.dll` (DInput8Proxy project)
- `BF2GameExt.dll` (PatcherDLL project)
- `BF2GameExt.ini` (from `dist/`, optional - all features enabled by default)

Works with Steam, GOG, and modtools builds. Compatible with other dinput8 proxy DLLs (e.g. ReShade) via automatic chain-loading.

### Method 2 — Exe Patcher
1. Build `BF2GameExt.dll` (PatcherDLL project) and `BF2GameExt.exe` (BF2GameExt project)
2. Place both in your `Star Wars Battlefront II Classic` folder (outside of `GameData`)
3. Run `BF2GameExt.exe` and patch a **copy** of BF2_modtools.exe
4. The patcher places the DLL into `GameData` automatically

## Configuration

All runtime options are controlled via `BF2GameExt.ini` (only used with the DInput8 Proxy method). If the INI file is absent, all features are enabled by default except those that require additional assets (e.g. Prone).

| Section | Purpose |
|---------|---------|
| `[General]` | Master enable switch, DLL path |
| `[LimitIncreases]` | Engine limit patches (heap, sound, objects, etc.) |
| `[Fixes]` | Bug-fix patches |
| `[Features]` | Optional gameplay features (e.g. Prone), diagnostics (game logging), and AI behavior toggles (dead-body shooting) |
| `[Controller]` | Gamepad enable and rumble toggles |
| `[Controller.*]` | Per-mode button/axis bindings (Unit, Vehicle, Flyer, Hero, Turret) |

The INI file is generated from the C++ source of truth. To regenerate after adding new features:

```
python generate_ini.py
```

Gameplay features are also configured through `load.cfg` parameters, ODF properties, and Lua functions. See the [Examples](Examples/) folder for ready-to-use configurations with inline documentation.

## Building from Source

Requirements:
- Visual Studio 2022 (v143 toolset)
- Windows 10 SDK
- C++20

```
git clone https://github.com/S1thK3nny/BF2GameExt.git
```

Open `BF2GameExt.sln` and build the solution. Output goes to `bin\Debug\` or `bin\Release\`.

## Project Structure

```
DInput8Proxy/src/    DInput8 proxy loader (dinput8.dll)
PatcherDLL/src/
  core/               Entry point, patching, address registry, resolve helpers
  entity/             EntitySoldier, EntityFlyer, cloth collision fixes
  weapon/             Grappling hook, disguise model override
  lua/                Lua API hooks and custom function registration
  loading_screen/     Loading screen system (config, renderer, lifecycle)
  shell/              Galactic Conquest visual limit extensions
  debug_commands/     Console debug visualization commands
  controller/          Controller support, aim assist, rumble
  util/               File helpers, slim_vector, class limit patch, INI config/registry
dist/                 Default BF2GameExt.ini (generated by generate_ini.py)
```

## Contributors

- **[PrismaticFlower](https://github.com/PrismaticFlower)** - Author of the original project this was forked from. Creator of numerous essential tools for the BF2 modding community.
- **[phantom567459](https://github.com/phantom567459)** - BF1 engine research and decompilation. His work on the BF1 binary made the loading screen system possible.
- **[Ryan Hank](https://github.com/RJP1992)** - Reverse engineering, decompilation, implementation.
- **[S1thK3nny](https://github.com/S1thK3nny)** - Reverse engineering, decompilation, implementation.
