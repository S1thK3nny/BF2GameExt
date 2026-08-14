<!-- GENERATED FILE. Do not edit by hand.
     Produced by generate_ini.py from ini_registry.hpp,
     controller_support.cpp and version.h. Re-run:  python generate_ini.py -->

# Configuration

Every option BF2GameExt v1.0.0 reads from `BF2GameExt.ini`, which sits next to the DLL in `GameData`. The INI is only consulted by the DInput8 proxy install, and it is optional: with no file present every key falls back to the default listed below.

Values are `1` for on and `0` for off unless a range is given. Lines starting with `;` are comments.

## General

Read by `dinput8.dll` before the extension is loaded, so these two cannot be changed at runtime.

| Key | Default | Description |
|-----|---------|-------------|
| `Enabled` | `1` | Master enable/disable switch for BF2GameExt |
| `DLLPath` | `BF2GameExt.dll` | Path to the main extension DLL (relative to proxy) |

## LimitIncreases

Engine limit patches. All are on by default and all are safe to leave on; they only raise a ceiling, they do not change behaviour below it.

| Key | Default | Description |
|-----|---------|-------------|
| `HeapExtension` | `1` | Extend RedMemory heap size |
| `SoundLayerLimit` | `1` | Raise SoundParameterized layer limit |
| `DLCMissionLimit` | `1` | Raise DLC / addon mission limit |
| `SoundLimit` | `1` | Raise global sound limit |
| `ParticleCacheIncrease` | `1` | Increase particle effect cache |
| `ObjectLimitIncrease` | `1` | Raise entity / object pool limit |
| `ComboAnimIncrease` | `1` | Raise combo animation limit |
| `HighResAnimLimit` | `1` | Raise high-resolution animation limit |
| `NetworkTimerIncrease` | `1` | Raise the input/voice-chat update tick from 30 Hz to 120 Hz (the simulation tick is untouched) |
| `MatrixPoolIncrease` | `1` | Extend matrix / item pool size |
| `StringPoolIncrease` | `1` | Increase string pool size |
| `AudioStreamLimit` | `1` | Raise the concurrent OpenAudioStream limit from 6 to 12. Each stream needs 3.4 MB of contiguous buffers, so this reserves 40 MB of the 32-bit process's 2 GB of virtual address space (RAM use is lower - pages are only committed as streams are actually used) |
| `LODLimitExtension` | `1` | Troops and props snap to their blurry low-detail models as soon as a fight gets crowded. Keeps roughly twenty times as many of them at full detail |
| `ExplosionVisibleRadius` | `1` | Explosions more than a short way off were not drawn at all, so distant fighting looked empty. Makes them visible across the map |
| `GCVisualLimits` | `1` | Raise Galactic Conquest galaxy-map pathway/particle draw limits (fixes missing pathways and icons with >13 planets) |

## Fixes

Bug fixes for engine defects. On by default. Each one is guarded by a byte check against the stock instruction bytes, so a patch that does not recognise your executable declines to apply rather than corrupting it.

| Key | Default | Description |
|-----|---------|-------------|
| `ChunkPushFix` | `1` | Let explosions push bodies that break into chunks, instead of dropping them where they stood |
| `PropGeneratorLoopFix` | `1` | Fix foliage-update crash at very high FOVs (PrismaticFlower's fix) |
| `SkyObjectLimit` | `1` | Raise the SkyObjectClass instance limit (PrismaticFlower's fix) |
| `SaberBlockFix` | `1` | Let lightsabers block other lightsabers from any direction. In stock BF2 a saber block only registers while you happen to be aiming at the centre of the map. Set 0 for stock |
| `TerrainTextureFix` | `1` | Re-resolve terrain detail/white textures each map (fixes playlist crash; PrismaticFlower's fix) |
| `BarrelFireOriginFix` | `1` | Fire projectiles from barrel hardpoint instead of bone_head. HINT: firing from the barrel adds barrel-to-crosshair parallax, so shots may not land exactly on the reticle once ReticleCorrection re-aligns it to the 3D aim point (worst at close range and with large weapon offsets). Set ReticleCorrection=0 if barrel-origin shots feel off-point |
| `BlurDownsizeClamp` | `1` | Clamp blur effect downsize resolution to 512px at high resolutions (PrismaticFlower's fix) |
| `ScreenshotFix` | `1` | Replace the broken Print Screen handler on retail builds (PrismaticFlower's fix) |
| `ErrorDialogFix` | `1` | Restore fatal-error dialogs on retail builds via a template in BF2GameExt.dll (PrismaticFlower's fix) |
| `DLCMissionInitFix` | `0` | EXPERIMENTAL: initialize the DLC mission list when launching a mission from the commandline (PrismaticFlower's fix; not yet working on retail, keep off) |
| `DroidekaDeathAnimation` | `1` | Let droidekas play their death animation (death01) instead of exploding instantly; banks without one are unaffected |
| `ReticleCorrection` | `-1` | HUD widescreen reticle vertical alignment: -1 auto (scales with aspect ratio), 0 to disable, or a manual strength 0..1 (full letterbox undo at 1) |

## Features

Optional behaviour that changes the game rather than fixing it. Some need assets that ship alongside the DLL.

| Key | Default | Description |
|-----|---------|-------------|
| `Prone` | `1` | Enable prone stance. Requires data\_lvl_pc\prone.lvl (the human_5 animation sub-bank), which is loaded automatically alongside every ingame.lvl read; prone stays off for any mission where that file is missing |
| `GameLogging` | `0` | Enable the engine's BFront2.log file logging on retail builds (modtools always logs) |
| `EnableSoundWarnings` | `0` | Log 'Unable to find sound property' warnings for missing sounds (modtools only; retail stripped the warning code) |
| `DisableAwardBuffs` | `0` | Remove the permanent combat-award buffs. Buffs from officer buff weapons and buff pickups are untouched. Technician's award weapon shares the same unlock bit as its passive, so it stays locked too |
| `DisableAwardWeapons` | `0` | Remove the combat-award weapons. Set alongside DisableAwardBuffs to disable all nine awards |
| `DisableDeadBodyShooting` | `1` | Stop AI from shooting dead bodies entirely (overrides DeadBodyShootingAllFactions) |
| `DeadBodyShootingAllFactions` | `0` | Let all factions shoot dead bodies, not just Alliance (ignored if DisableDeadBodyShooting=1) |

## Lightsaber

Lighting for lightsaber blades. Off by default. Radius and intensity are independent: radius changes how far the light reaches, intensity changes how bright it is, and changing one does not affect the other.

| Key | Default | Description |
|-----|---------|-------------|
| `LightsaberIllumination` | `0` | EXPERIMENTAL: ignited lightsaber blades give off real light in their own blade colour. Objects can only take 4 dynamic lights at once, so a nearby saber can replace one of a room's own lights. 0 by default |
| `LightsaberLightRadius` | `4.0` | How far the lightsaber light reaches, in metres at full blade extension (it grows as the blade ignites). Brightness is unaffected by this, so it only changes reach - but a larger radius evicts more of the map's own lights |
| `LightsaberLightIntensity` | `1.0` | Multiplier on the lightsaber light colour. 1.0 uses the blade colour as authored |

## AI

Removes hardcoded biases that make BF2's AI single out the human player. SWBF1 has no player term anywhere in its target selection, which is why its AI is remembered as fairer. Three of the four are on by default; set any of them to 0 for stock behaviour. `PlayerThreatFairness` is the exception and is **off** by default, because it is the one that stops the AI reacting to being aimed at, which makes them feel unresponsive rather than fair. AI will still turn on you the moment you damage them, because that path force-sets the attacker as the target and re-broadcasts to nearby squadmates; it is deliberate and is left alone.

| Key | Default | Description |
|-----|---------|-------------|
| `PlayerVisionFairness` | `1` | AI spot you at the same range they spot a bot. Stock BF2 doubles its view range for human players. Set 0 for stock |
| `PlayerPriorityFairness` | `1` | AI rank you the same as a bot at equal distance. Stock BF2 ranks you as if you were half as far away. Set 0 for stock |
| `PlayerThreatFairness` | `0` | AI stop treating you as extra dangerous while you are aiming at them. OFF by default: it is the one fairness option that makes AI feel unresponsive, since they no longer react to being aimed at. Set 1 to enable |
| `PlayerAwarenessFairness` | `1` | AI keep looking for other enemies while fighting you. In stock BF2 an AI tracking you cannot notice anyone else at all. Set 0 for stock |

## Controller

Gamepad support. The button and axis bindings live in the `[Controller.<Mode>]` sections and are documented separately in [CONTROLLER.md](CONTROLLER.md).

| Key | Default | Description |
|-----|---------|-------------|
| `Enabled` | `1` | Enable gamepad / controller support |
| `Rumble` | `1` | Enable controller rumble / vibration |

## AimAssist

Xbox-style aim assist for gamepad players, singleplayer only. **Off by default** - set `Enabled=1` to turn it on, since it changes how aiming feels and mouse players have no use for it. The tuning values are set to match the console release, so enabling it alone gives you the Xbox feel; they are exposed for anyone who wants it stronger or weaker.

| Key | Default | Description |
|-----|---------|-------------|
| `Enabled` | `0` | Enable controller aim assist |
| `ConeAngle` | `30` | Fallback cone angle in degrees when weapon has no AutoAimSize |
| `TrackingDeadZone` | `0.5` | Dead zone multiplier for weapon AutoAimSize |
| `FrictionStrength` | `3.0` | Directional friction scale when aiming away from lock |
| `PullStrength` | `5.0` | Auto-tracking ramp rate per second toward locked target |
| `LockBreakTime` | `0.1` | Seconds of pushing away to break target lock |
| `AutoLockOnHit` | `1` | Automatically lock onto first enemy you damage |
| `SnapStrength` | `1.0` | Instant correction on first lock frame (0 = ramp only) |
| `ProximityFriction` | `1` | Slow stick when crosshair is near any enemy |
| `ProximityFrictionRadius` | `0.5` | Screen-space radius for proximity slowdown |
| `ProximityFrictionScale` | `0.4` | Min friction at dead center (0 = full stop, 1 = none) |

## Controller bindings

The `[Controller.Unit]`, `[Controller.Vehicle]`, `[Controller.Flyer]`, `[Controller.Hero]` and `[Controller.Turret]` sections map physical buttons and axes to in-game actions. Every default is written into the shipped INI as a commented-out line. See [CONTROLLER.md](CONTROLLER.md) for the input and action names and the full default tables.

Not everything is configured here. Gameplay features also read `load.cfg` parameters, ODF properties and Lua functions; see [FEATURES.md](FEATURES.md).
