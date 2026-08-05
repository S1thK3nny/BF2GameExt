# Features

See the [compatibility table](../../README.md#compatibility) for the current state of each build.

## Engine Limit Extensions

Automatic binary patches applied on load:

- **Heap Memory Extension** - Increases the game's main memory heap from 64 MB to 256 MB, drastically reducing out-of-memory crashes
- **DLC Mission Limit** - Increases from 500 to 4096, allowing more mods installed simultaneously
- **Sound Layer Limit** - Prevents crashes on maps with many flyers or entities using EngineSound
- **Sound Memory Limit** - Increases sound RAM from 32 MB to 256 MB
- **Audio Stream Limit** - Raises the number of audio streams that can be open at once from 6 to 12, removing the `Maximum number of open audio streams exceeded` failure when a mission script calls `OpenAudioStream` more than six times. Each stream carries its own ~3.4 MB of decode buffers, so the 12-slot array reserves about 40 MB of *virtual address space*. Turn this off if a heavy mod is running out of address space. INI: `[LimitIncreases] AudioStreamLimit=1`

  **Caveat:** the shared fire-and-forget request pool stays at 24 entries, so a script queueing sounds on all 12 streams at once can still hit that older limit. Exceeding it drops the queued request rather than failing the stream.
- **Particle Cache** - Increases the cached particle limit from 300 to 1200
- **Object Limit** - Doubles the active object table from 1024 to 2048 slots, raising how many objects a map can have alive at once
- **Combo Animation Limit** - Increases from 30 to 90 entries, with an expanded animation index range
- **High-Res Animation Limit** - Increases from 50 to 12,800 entries
- **Soldier Animation Bank Limit** - Raises the per-class loaded animation bank cap from 18 to 128, removing the `Out of space for soldier animation banks (max 18)` error. Note that a bank split into numbered sub-banks (`human_0` through `human_N`) uses one slot each, so this lets a class load far more split banks than before.

  **Caveat, a separate and deeper limit is not raised:** the number of *distinct bank names* is capped at **16** and *weapon types* at **20**. Those two cannot be raised safely. Keep modded soldier classes to 16 or fewer distinct bank names and 20 or fewer weapon types.
- **String Pool** - Increases the string pool from 32 KB to 128 KB, preventing crashes in debug builds with heavy string usage
- **Matrix/Item Pool** - Extends the matrix pool to 256 times its original capacity
- **Renderer Cache** - Increases the particle renderer cache from 15 to 120 entries
- **Input Update Rate** - The engine runs two fixed-rate update timers. The second one gates keyboard, joystick and voice chat updates, and was fixed at 30 Hz, so input was only sampled 30 times a second no matter how high the framerate ran. This raises it to 120 Hz. The simulation timer is untouched, so nothing about game speed or netcode changes, input just stops being the slowest thing in the loop. INI: `[LimitIncreases] NetworkTimerIncrease=1`
- **GC Visual Limits** - Raises Galactic Conquest per-frame rendering limits: pathway beams from 64 to 256 (255 on Steam), and planet icons from 128 to 512. Also spreads beams across spare cache slots when the shared batching cache fills up. Without that, every pathway beam competes for one cache and they silently stop drawing at roughly 50 beams no matter how large the buffer is. Fixes pathways and fleet/planet icons disappearing on modded GC maps with many planets. INI: `[LimitIncreases] GCVisualLimits=1`
- **Sky Object Limit** - Removes the cap on how many objects a sky dome or backdrop can contain. Port of PrismaticFlower's upstream fix. INI: `[Fixes] SkyObjectLimit=1`

## Engine & Rendering Fixes

General engine bug fixes, several of them ported from PrismaticFlower's upstream. Build restrictions and INI keys are noted per entry:

- **PropGenerator Loop Fix** - The procedural foliage system could read past the end of its object array at very high fields of view and crash. The patch restores the missing bounds check. INI: `[Fixes] PropGeneratorLoopFix=1`
- **Chunk Push Fix** - When an explosion sweeps up a soldier, the engine rolls the class's chunk frequency to decide whether the body breaks apart into chunks. If that roll passes it flags the body and returns immediately, before the explosion's push is ever applied, so a body that gibs simply drops where it stood while a body that does not gets thrown. The fix lets the push run either way, so chunked bodies are flung by the blast like everything else. INI: `[Fixes] ChunkPushFix=1`
- **Terrain Texture Fix** - The terrain shader caches two textures that only get assigned on maps that have a terrain detail map. Going from a map with one to a map without one in the same session left the shader pointing at freed memory, producing garbage terrain or a crash. The fix re-resolves both textures before every terrain load so they are always valid. Also fixes an upstream copy-paste bug that fed the wrong texture into one of the two slots. INI: `[Fixes] TerrainTextureFix=1`
- **Game Logging Enablement** - Retail builds ship the engine's `BFront2.log` file logging compiled in but switched off. This turns it back on without needing the `/log` command line flag, which is useful for diagnosing crashes and mod issues. No effect on Modtools, which always logs. INI: `[Features] GameLogging=0` *(off by default)*
- **Enable Sound Warnings** - When an ODF references a sound that is not loaded, the engine can warn about it, but the warning is switched off by default and cannot normally be enabled. This turns it on so missing sound names show up in the log. Modtools only: the retail builds compiled the warning code out entirely, so there is nothing to enable there. INI: `[Features] EnableSoundWarnings=0` *(off by default)*
- **Blur Downsize Clamp** - The blur effect renders into a buffer sized by a factor tuned for 2005 era resolutions. At modern resolutions that buffer ends up far larger than intended, which makes the blur both weaker looking and more expensive. This clamps it to 512 pixels on its long edge. INI: `[Fixes] BlurDownsizeClamp=1`
- **Screenshot Fix** - Print Screen crashes the retail builds. Replaces the broken screenshot routine with a clean capture written to `ScreenShots\screenshot_NNNN.tga`. No effect on Modtools. INI: `[Fixes] ScreenshotFix=1`
- **Error Dialog Fix** - The retail builds are missing the dialog resource the engine uses for fatal error messages, so those dialogs silently fail and the game just exits with no explanation. This supplies a replacement dialog from BF2GameExt so the error is actually shown. No effect on Modtools. INI: `[Fixes] ErrorDialogFix=1`
- **DLC Mission List Initialization Fix** *(experimental, off by default)* - Launching straight into a mod map from the command line fails, because the addon mission list is only built when the shell menu runs. This enters and immediately exits the shell first, giving addon scripts their normal context. Ported from upstream but not yet confirmed working on retail builds, where command line addon launches still fail. INI: `[Fixes] DLCMissionInitFix=0`
- **HUD Widescreen Reticle Correction** - On widescreen displays the game scales and offsets every HUD element, which pushes the aim reticle off the true aim point, with the error growing toward the screen edges. This pre-corrects the reticle so it lands in the right place, leaving all other HUD elements untouched. INI: `[Fixes] ReticleCorrection=-1` (auto; `0` disables, or set `0..1` manually)
- **Map Queue Next Mission Fix** - Finishing a match on Modtools always dropped you back to the main menu, even when the mission playlist still had maps queued. This was due to the branch simply not being present due to the modtools simply being older than the retail builds. The branch is restored and the queue now rolls straight into the next map the way it does on retail.
- **Custom In-Game Movies** - `ScriptCB_PlayInGameMovie("ingame.mvs", "segment")` looks like it takes a movie file, but every shipping build throws that first argument away and hardcodes the file, picking `ingame.mvs` (or `ingamefr.mvs` / `ingamegr.mvs` on French and German) from a language table. A custom in-game movie could therefore only ever be played by overwriting the stock `ingame.mvs` in the base game folder. The argument now works, and understands the `dc:` addon prefix, so a mod can ship its movie in its own addon folder. The three stock names still take the old path, so the localised campaign movies are unchanged. See **[Lua API](LUA_API.md)** for the usage.

## Loading Screen System

Adds new loading screen parameters that **allow modders to fully restore bf1 style loading screens.** 
These work alongside the vanilla ones by redirect the whole loading screen configuration to a custom `load.cfg` from Lua.

See **[Loading Screen](LOADING_SCREEN.md)** for the full parameter reference.

## Soldier Systems

- **Prone Stance** - Re-enables, fixes, and adapts the cut prone posture. Double-tap crouch to go prone, any crouch press to stand back up. Includes a terrain fix that stopped prone working on slopes. The prone animations live in their own `prone.lvl`, which is read automatically after every `ingame.lvl`. Drop `prone.lvl` into `data\_lvl_pc\`; if it is not there, prone stays off for that mission. INI: `[Features] Prone=1`
- **Multiple First-Person Animation Banks** - Lets each soldier class use its own first person animation bank instead of sharing one global set. Partial banks work too, with missing animations falling back to the defaults. ODF: `FirstPersonAnimationBank = bankname`
- **First-Person Sprint Animation** - The engine has no first person sprint state and just plays the run animation faster. This adds the possibility for modders to add a real sprint animation per weapon class. If `<bank>_rifle_sprint` (or `_bazooka_sprint`, `_tool_sprint`) exists in the bank it is used while sprinting. Works with custom banks, and is entirely optional: if the animation is absent, nothing changes.
- **Animation Bank Appending** - Lets an animation bank be extended with extra numbered sub-banks spread across multiple .lvl files. The engine only scans for sub-banks once, during the first .lvl load, so a sub-bank that arrives later (for example `human_5` from a modified `ingame.lvl` after a mod's own `ingame.lvl` already registered `human_0`) was silently ignored. This picks up the late arrivals. Works for any bank, not just `human`. *The animation bank needs to be split into sub-banks for this to work.*
- **Extra Override Textures** - The stock engine gives soldier classes two runtime texture override slots. This adds three more: `OverrideTexture3`, `OverrideTexture4` and `OverrideTexture5`. The extra slots ride on the stock render path, so `OverrideTexture` must also be set for them to apply. Set them on the concrete soldier class; they are not inherited through `ClassParent`. The underlying system is fully dynamic, so the count could be raised further, though more than five is untested.
- **Unit Class Removal** - Remove classes from a team's spawn menu at runtime. See [Lua API](LUA_API.md).
- **Award Removal** - ...Can I even call this a feature? The awards are enabled permanently, even if you turn off the visual effects via the 1.3 or 1.5 patch. Four of the nine grant a passive that never wears off, the other five grant an upgraded weapon. Two INI toggles remove either group without touching the buff system itself, so buff pickups and the officer class buff weapons keep working as before. Both are off by default. `[Features] DisableAwardBuffs=1` drops the permanent buffs; technician's award weapon shares the same unlock flag as its passive, with nothing in the engine separating the two, so that one weapon stays locked as well. `[Features] DisableAwardWeapons=1` drops the award weapons. Set both to disable all nine awards.

## Weapon Systems

- **Barrel Fire Origin Fix** - Fixes projectiles spawning from `bone_head` instead of `hp_fire` on `cannon` and `launcher` weapons, so shots come out of the actual barrel hardpoint. Aim stays true while zoomed: the shot is re-aimed at whatever it would have hit from the default origin, so moving the muzzle never costs accuracy. Turns itself off while a sniper scope is on screen, where the barrel is not visible anyway. INI: `[Fixes] BarrelFireOriginFix=1`
- **Shield Channel Fix** - Fixes shield weapons activating regardless of which secondary weapon is selected. The shield now only responds when it is the active weapon for its channel.
- **Disguise Model Override** - Lets WeaponDisguise swap the soldier's model to a specific model instead of cloning the first enemy soldier. ODF: `DisguiseModel = modelname`
- **Animated Lightsaber Textures** - Ports the Xbox version's animated blade textures, giving a lightsaber a four frame texture cycle where PC blades use a single static texture. ODF, under the blade's WeaponMelee section: `AnimTexture1 = tex_frame2`, `AnimTexture2 = tex_frame3`, `AnimTexture3 = tex_frame4`
- **Grappling Hook** *(experimental)* - Re-enables the cut grappling hook weapon, with custom pull physics, a slingshot mechanic (jump mid-pull to launch), and rope cable rendering. ODF: `PullSpeed`, `MaxRange`

## Vehicle Additions and Fixes

- **Carrier Class** - EntityCarrier was an unused class. Now, it's a completely usable class with proper landing oscillation, cargo attachment, level of detail rendering, turret activation and animation, making it usable as a VehiclePad.
- **Droideka Death Animation Fix** - Droidekas never played their death animation even though every stock droideka bank defines one. A bug cut the animation off after a single frame, so the droideka just exploded instantly, while walkers like the ATST, ATTE and ATAT played theirs correctly. The fix lets the death animation run to completion, and drops the personal shield as soon as the droideka starts dying instead of leaving it up through the collapse. Rolling droidekas still explode instantly by design, and banks with no death animation are unaffected. INI: `[Fixes] DroidekaDeathAnimation=1`
- **Flyer Boost Animation** - If a flyer's animation bank contains an animation named `boost`, it plays automatically when boosting, with a smooth blend in and out. Frame 0 should be the normal flying pose and the last frame the full boost pose.
- **Vehicle First/Third Person Toggle** - Fixes the change view button being silently ignored on hovers and walkers. Both classes shipped with the view change permanently reported as suppressed, so ground vehicles were stuck in third person unless `ForceMode` was set in the ODF. Also applies to their AI controlled variants.
- **CreateEntity Vehicle Weapons Fix** - Vehicles spawned from Lua with `CreateEntity` worked fine except that their weapons silently did nothing. Stock `CreateEntity` skips the team assignment and activation steps the normal vehicle spawner performs, and without a team the game refuses to spawn projectiles. This runs the missing steps automatically. Adds an optional fourth argument, `CreateEntity(class, matrix, name [, team])`, defaulting to 0 if omitted. Existing call sites keep working unchanged.
- **LandOnArrival Fix** - The path node property `LandOnArrival` is read for every node but never worked as authored, because of two engine bugs. Only the first node in a path was ever checked, and once a flyer landed the flag was never cleared, so it re-triggered the landing every frame and could never take off or be used again. The fix makes arrival at any flagged node land the flyer, and clears the flag afterwards so the vehicle can be entered by AI and players.
- **Flyer Engine Sound Fix** - AI flyers following waypoint paths had stuttering, warbling engine audio. The path follower's speed varies very slightly every frame, and the engine sound amplified that jitter into large pitch and volume swings. This smooths the speed inputs so the jitter is damped out, while player controlled flight is effectively unaffected.
- **Self-Piloted Hover Crash Fix** - A hover vehicle with `PilotType = "self"` crashed the game outright. The hover's obstacle avoidance fetches the vehicle's pilot without checking whether there is one, and a self piloted hover has no separate pilot. Every stock hover is soldier entered, so the case was never guarded. This handles the missing pilot safely, along with a second crash of the same kind when the vehicle acknowledged a unit order.
- **Droideka Ball Mode Toggle** - The droideka is the only playable walker and vehicle hybrid, so its roll is welded to the chassis. A mod that wants it as a plain walking unit can ask the player not to press the button, but the AI still rolls. `DisableBallMode = 1` on a walkerdroid class removes the roll outright, for the AI as much as the player. It is per class, so a rolling and a non-rolling droideka can coexist, and it inherits through `ClassParent` like a stock property. Off by default. ODF: `DisableBallMode = 1`

## AI Systems

- **Dead Body Shooting Control** - In the vanilla game, Alliance units break off to walk up to and fire on nearby soldier corpses. A fun feature implemented by Pandemic, but it does hinder the gameplay experience as they are dead focused on the corpses. Two INI toggles control this:
  - `[Features] DisableDeadBodyShooting=1` *(default on)* - Stops the behaviour entirely for every side, so no one ever shoots dead bodies. Overrides the all factions toggle.
  - `[Features] DeadBodyShootingAllFactions=1` *(default off)* - Extends the behaviour to all factions instead of just Alliance. Ignored while `DisableDeadBodyShooting=1`.

## Additional Console Commands

Extra commands added to the in-game console (`~`). Modtools only, because the
retail builds have no command console to add them to.

- `RenderHoverSprings` - Visualise hover vehicle spring compression with coloured wireframe spheres
- `ShowWeaponRanges` - Draw weapon AI range circles (MinRange, OptimalRange, MaxRange) around soldiers
- `memwatch` - Reverse-engineering aid. Arms a CPU hardware data breakpoint on an address and reports every distinct piece of code that reads or writes it, with a register snapshot and a best-effort call stack per accessor. Up to four addresses at once, since that is how many debug registers x86 has. `memwatch [u]<hexaddr> [len] [r|w|rw]` to arm, bare `memwatch` to report and disarm, `memwatch clear` to drop all watches. A plain address is a runtime one; the `u` prefix takes an unrelocated address straight out of Ghidra and rebases it for you. Reported accessor and caller addresses are unrelocated, so they paste back into Ghidra as is. See [MemWatchRE.md](../MemWatchRE.md)

## Controller Support

- **Gamepad Bindings** - Five control modes (Unit, Vehicle, Flyer, Hero, Turret) with configurable button layouts. Does not affect keyboard and mouse bindings. INI: `[Controller.*]` sections
- **Aim Assist** - Xbox style aim assist ported from the console version's dead code. Proximity friction, auto lock on hit, target tracking and directional friction. Controller only, singleplayer only. Off by default. INI: `[AimAssist] Enabled=1`
- **Rumble** - Controller vibration on weapon fire and damage. INI: `[Controller] Rumble=1`
