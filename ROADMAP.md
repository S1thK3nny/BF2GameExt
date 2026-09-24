# Roadmap

Planned and in-progress work. This is the backlog, not a feature list: nothing here
is shipped. For what the DLL actually does today, see
[docs/user/FEATURES.md](docs/user/FEATURES.md).

## Bugs
**Tentacle fields are unclamped in stock code** - `NumTentacles` and `BonesPerTentacle` on
`EntitySoldierClass` accept bigger values than the arrays behind them can hold, and no build clamps
them. The bones overflow lands on the stack, so a typo in an ODF can corrupt the return address.
Retail even removed the warning, so it fails with no message at all. `TentacleLimit=1` already
clamps both, so this only matters with that feature off. The fix is a small edit to the bitfield
mask at each read site, listed in [docs/RE/TentacleSystem.md](docs/RE/TentacleSystem.md).

**GUI freeze with too many effect classes** - The engine's hash table lookup has no loop limit.
Once a table is completely full, looking up a name that is not in it loops forever: the game
freezes with audio still playing. The effect class table holds 256, and a missing or misspelled
`AttachEffect` name is exactly that kind of lookup. It fits the reported freeze, but nothing
proves yet that this is what people hit. Next step is to watch the effect class count with
`ContentCensus` on a map that freezes, before patching anything. Details in
[docs/RE/ContentCensus.md](docs/RE/ContentCensus.md).

**LODs break under freecam** - Models pop to the wrong detail level, or drop out entirely,
while the camera is detached. The LOD selection almost certainly scores against the player
entity or the game camera rather than the active render camera, so once freecam moves away
from the player everything is graded at the wrong distance. Needs the actual LOD distance
source traced before a fix is designed.

## Vehicles

**9-pose vehicle aiming for AI** - The 9 aim poses are driven directly by the player's
mouse / joystick deflection, so an AI driven vehicle never advances past the initial frame
and sits locked in the neutral pose. Needs the pose selection to be fed from the vehicle's
actual aim delta (turret / aimer angle) rather than raw player input, so AI and players
drive the same path.

**Walker stomp attack cleanup** - The stomp / attack system is fully wired at runtime but
rough around the edges:

- Add dedicated `AttackEffect` and `AttackSound` ODF properties instead of the current
  hardcoded FX and sound.
- Revisit `AttackControls`. Values 1 and 2 are documented as primary / secondary fire but
  do not actually restrict to those, so the real semantics need pinning down.
- Write proper user documentation for enabling it. It is currently gated on
  `HealthType = "animal"` plus an `AttackAnimation` ODF entry.

**Flyer strafe mode ODF property** - The engine has an abandoned strafing path for flyers.
Goal is an opt-in `EntityFlyer` ODF property that turns the turn axis into lateral movement,
which means giving up rolling on any class that enables it, since the same input drives both.
A prototype exists on an older commit and was
player-only for exactly this reason: AI keeps rolling so navigation is not broken. The open
question before this can ship is what AI actually does when a class it flies is in strafe
mode, and whether the AI flight path can be fed strafing at all rather than simply left on
the old behaviour. Needs a play test either way, since the visual lean scaling and sign were
never confirmed in game.

**AI spawning whilst the CommandFlyer is flying** - Ever noticed when flying a gunship that you suddenly have AI "falling out?".
This happens when the CommandFlyer is flying and the AI spawns in. The AI spawns in at the CommandFlyers position, despite the fact that the CommandFlyer is flying. 
The fix is to add an additional check to the AI spawning code to check if the CommandFlyer is flying, and if so, 
just don't allow them to spawn there.

**ControlsUnit passenger weapons** - Lets a passenger use their own weapon from inside a
vehicle. Very unlikely to happen: the engine stub is half baked, and it needs a lot more
than patches. The PassengerSlot entity builds no weapon and no aimer (so there is no
reticle), `UpdatePilotAnimation` passes hardcoded -1 aim arguments (so the body never
aligns with where the passenger is looking), and `GetMatrix` recomputes the raw mount
matrix against the vehicle every frame with no smoothing (so the camera and aim are welded
to the vehicle's collision jitter). Fixing it properly means building the missing weapon
and aimer path, not patching the existing one.

## Soldiers

**Charged jump for soldiers** - Speeder bikes get a hold-to-charge jump driven by five
`EntityHoverClass` ODF properties; soldiers only get a single fixed `JumpHeight` impulse.
Goal is to give soldiers the same model:

```
JumpTimeMin      = 0.1
JumpTimeMax      = 0.35
JumpForce        = 50.0
JumpMinSpeedMult = 0.4
JumpEnergyPerSec = 100.0
```

All five are parsed today in `EntityHoverClass::SetProperty` only, so this means adding the
properties to `EntitySoldierClass` and porting the charge logic (hold window, force scaling
between min and max hold, forward speed multiplier while charging, energy drain per second)
onto the soldier jump path rather than replacing `JumpHeight` outright. `JumpHeight` should
keep working for ODFs that do not opt in.

**Melee-only blocking** - Right now a blocking melee weapon deflects everything it is set up
to deflect, which makes any shield or blocking stance behave like a lightsaber. Goal is an
ODF or combo property that restricts a weapon's block to incoming melee attacks only, so
blaster fire passes through. The deflect call site is already known from the saber block
work, so this is a matter of finding what identifies the incoming attack at that point and
adding a per-weapon gate, not new plumbing.

**Jetpack Particle Effect Origin** - The engine hardcodes the jetpack effect to be attached to `bone_ribcage`, 
which may work fine for a regular jetpack, but not if you want to do something a bit more fancy,
like Cad Banes jetpack boots, which would require the effect to be attached to bone_l_foot and bone_r_foot. 
The fix is to add a new ODF property to the jetpack class that allows you to specify the bone name for the effect origin.

**Jetpack Directional Animations** - The jetpack only has one animation: jetpack_hover. 
The goal is to add directional animations based on the player's movement direction, similarly to how the flying
or land animations for units have it.

**Real riot shields** - A shield that actually stops shots by geometry rather than by a
deflect rule. Needs per-unit collision on the shield part, which the soldier collision model
does not currently provide: soldiers use a single capsule, and the only existing example of
custom soldier collision is the acklay style units, whose collision also does a ground check
that a shield must not inherit. So the real work is a soldier collision path that supports
extra attached collision volumes without dragging the ground handling along with it. Large,
and gated on that collision work rather than on anything shield specific.

**Improved dual pistols** - Two visible pistols that alternate fire, instead of the current
one model and one muzzle. The only dual wield support the engine offers today is
`OffhandGeometryName`, and every stock use of it is on a lightsaber, so a `cannon` class
weapon that wants a second pistol has nothing to hang it on. Two routes:

- Make `OffhandGeometryName` work outside melee weapons. The smaller change, but it only
  ever attached a second *model*, so it buys the look and none of the behaviour.
- Preferred: a new `dualcannon` ClassLabel deriving from `cannon`, owning both the second
  model and the fire alternation.

Either way the offhand attachment point should be named from the ODF rather than hardcoded,
something like `OffhandHardPoint = "hp_weapons2"`, with the matching hardpoint added to the
skeleton and model. The stock soldier skeleton carries only `hp_weapons` (CRC `0x2b960099`).

The `dualcannon` route is cheaper than it sounds: registering a new `ClassLabel` is one
allocation plus one constructor call, and the whole extension surface is two pure virtuals
(`Derive` and `Build`) plus `SetProperty`. Extra per-instance state is free because the new
class owns every allocation of its own type. Full write-up, including what still has to be
checked before it could ship, in
[docs/RE/WeaponClassFactory.md](docs/RE/WeaponClassFactory.md). That does not settle the
open question below, which is the part that actually needs deciding.

Firing model: when weapon 1 finishes its salvo, switch to weapon 2; when weapon 2 finishes,
switch back. An ODF option should pick the timing:

- continuous - one trigger pull alternates 1, 2, 1, 2 for as long as it is held
- per shot - one salvo per trigger pull, so fire 1, release, fire 2

What is already mapped, so this does not start from nothing:

- `Weapon` carries an `mIsOffhand` bit (`+0x2B0` bit 7), so the engine already distinguishes
  an offhand weapon instance.
- `EntitySoldier` carries a dual wield flag byte (modtools `+0x24A`, release `+0x232`,
  bit 0), already in `entity_layout.hpp`.
- That flag already reroutes input: the character weapon path treats channel 1 of a dual
  wield pair as firing off the *reload* trigger, with no reload of its own. So the engine's
  existing notion of dual wield is "two channels, the second one on the reload trigger",
  not "one channel that alternates". Deciding whether to extend that or bypass it comes
  first, because it settles whether this is a `Weapon` level change or an `EntitySoldier`
  input change.

Open question: whether the offhand should be a real second `Weapon` instance (two ammo
pools, two reloads, two muzzle effects) or one weapon that alternates its fire origin
between two hardpoints. The first is what "dual pistols" implies and is what the alternating
salvo logic naturally wants; the second is far cheaper and may be enough if the ask turns
out to be visual plus muzzle alternation.

## Weapons

**Force pushable grenades** - Force push moves units and ignores thrown ordnance, so a
grenade sails straight through a push that would have thrown a soldier across the room. Goal
is to let a push pick up a live grenade and send it back.

Needs tracing first: what the push collects as targets and whether an in flight ordnance can
join that set at all, and how to move one once found, since the ordnance is already running
its own trajectory.

Identifying a grenade at the push site is the part that needs care. `ClassLabel = "grenade"`
sits on the *weapon* ODF; what flies is an ordnance instance from a separate ordnance ODF,
so nothing at the push site sees that label. Walking back to the spawning weapon to test it
would work but is blanket, catching any weapon that borrowed the label. Preferred is an opt
in property on the ordnance class: `OrdnanceGrapplingHookClass::SetProperty` already reads
`SoldierAnimation` that way, and an `Ordnance` carries its `OrdnanceClass*` at `+0x30`, so
the lookup is solved. What it must not be is the ordnance ClassLabel, which
would sweep up rockets, mines and anything else on the same label.

**`ScopeTextureFull` - a whole texture as the scope, not a mirrored quarter** - The zoomed
scope overlay is one texture quadrant mirrored into four screen quadrants, so art must be
symmetric about both axes and an off-centre reticle is not authorable. Each quad is also
stretched per axis independently, which is where BF2's oval-at-16:9 scope comes from.
`ScopeTextureFull` (hash `0xB7D234B3`, on `WeaponClass`) makes element 0 full-screen centred
and disables elements 1-3. Opt-in by data, no INI toggle. Addresses, the spec and a companion
`ScopeModel` are in [docs/RE/ScopeDisplaySystem.md](docs/RE/ScopeDisplaySystem.md); two things
worth fixing while in there are that `ScopeTexture` silently falls back to `weapon_scope` on a
typo with no warning, and that bitmap sizes are read once in the constructor so stock scopes
are wrong after a resolution change.

## Rendering

**Restore the decal system** - BF2 ships a complete decal pipeline with only the
`Add*Decal` entry points gutted, so surfaces never take burn or impact marks. Target is
marks on walls and props; character marks are out of scope, since the engine has no
per-bone decal skinning and no Ghoul2 equivalent. Ordered plan:

- **A. Proof of life.** Build a `DecalClass`, allocate one `Decal` from `Decal::sMemoryPool`,
  fill a hardcoded 4-vertex quad, link it into `m_decals` and see if it draws. This answers
  what disassembly cannot: whether a `DecalClass` instantiates end to end, whether the shader
  resolves, and whether the empty `PlatformInit` is fatal. If a quad will not render, stop.
- **B. Terrain decals.** Transcribe Phantom's `ComputeTerrainDecal` and drive it from A's
  spawn path. Retail dead-stripped it, so Phantom's body is the original shipping
  implementation - transcription, not invention. Gives a validated reference decal to check
  step D against.
- **C. Find a `RayTest` that returns hit position plus normal.** Untraced. Blaster bolts must
  already compute one to place impact effects. Unblocks D and may shrink it considerably.
- **D. Write `ComputeObjectDecal`.** The real work, and the only piece with no existing body
  to copy - it is empty in all three builds, Phantom included. Clip the projection quad
  against the collision object's triangles. Q3-derived implementations are GPLv2 and this
  repo is MIT, so read for design only.
- **E. Wire it up.** Hook `WeaponMelee::UpdateFire` after the ray test returns true; the
  object, ray index and segment are all in hand there.

A and B are confidently estimable. C and D are where the schedule can move. Once the
pipeline exists, blaster impacts and explosion scorch marks are nearly free and will be far
more visible in normal play than saber marks.

**Unlock framerate above 80 FPS** - The game caps out around 80 and anything above this will
cause issues with vehicles, especially hovers, due to being tied to the framerate. The fix is to detach the physics and animation updates from the framerate, and instead run them on a fixed timestep. This will allow the game to run at higher framerates without affecting gameplay mechanics.

## Sound

**Sound region and stream manipulation** - Goal is runtime control over ambient sound
regions and streams. Current state of the problem:

- Lua `SetProperty` can never reach sound entities. `EntitySound` derives from `Entity`,
  not `EntityEx`, so it is absent from the id map `SetProperty` looks in. This is
  structural, not a missing case.
- **`SetInstanceProperty` should handle sound entities.** It exists for exactly this
  wall: vehicle spawners are the same `Entity`-not-`EntityEx` case and are handled by
  walking their global list and calling their own `SetProperty`. Sound entities fit
  the same shape: add a family in `entity/instance_props.cpp` that walks
  `sEntitySoundList`, matches the instance name, and calls `EntitySound::SetProperty`.
  Mapped on the Phantom build only so far (ctors link into `sEntitySoundList` through
  `0x0043E1B0`, `SetProperty` at `0x0058C620`); needs porting to modtools, Steam and
  GOG, and where the instance name lives on `EntitySound` is not yet known. That
  handler does not chain to `Entity::SetProperty` on an unmatched key, so only its own
  keys will work.
- `SetClassProperty` does work on `SoundAmbienceStatic` and `SoundAmbienceStreaming`, but
  only for four properties (`Sound`, `SoundStream`, `MinDistance`, `MaxDistance`) and only
  before the entity is created, since it edits the class and not the instance.
- Sound regions clobber those values anyway once they take over.
- So the open question is a respawn or re-apply path: change the class, then force the
  existing sound entities to rebuild from it, without regions immediately overwriting the
  result.

**Audio stream queue pool** - Raising the audio stream limit from 6 to 12 left the shared
queue pool at 24 entries, while 12 streams can queue up to 48 requests. Running the pool dry
crashes instead of dropping the request. The pool cannot grow where it is, so it needs moving
like the stream arrays were, and running out should drop the request instead of crashing.
Details in [docs/RE/SoundSystem.md](docs/RE/SoundSystem.md).

**Only the first hero per team gets its SndHero VO** - The `SndHeroSelectable`, `SndHeroSpawned`,
`SndHeroDefeated` and `SndHeroKiller` sounds are stored once per team, not per hero, so whichever
hero loads first owns them for the whole round. The same root causes hero 2 to show up under
hero 1's name in the HUD and kill messages: the engine's "which hero is this team's" lookup
simply returns the first hero class registered. The likely fix is to hook that lookup and return
the hero that is actually in play.

## Retail builds

**Branch region fix untested on retail** - It is set up for all three builds but has only been
played on modtools. Retail does not print the "Unable to find branch region" warning, so testing
it there means turning on `[Diagnostic] BranchRegionDebug=1` and watching units actually take the
branch.

## Limits

**AI reservation pool past 127** - `ReservationPoolSize` stops at 127 because of how the value is
encoded in the exe. Going higher means rewriting that instruction. Only worth it if 127 turns out
to still run out in play. Details in [docs/RE/EngineLimits.md](docs/RE/EngineLimits.md).

**More than 16 command posts, single player only** - Multiplayer is off the table: the network
messages have room for exactly 16 posts. Single player is doable by growing only the game-side
arrays and leaving the HUD at 16, so posts past 16 work but do not show on the map or radar. It
has to check at runtime that the game is not online, so it is a hook rather than a patch. Site
list in [docs/RE/EngineLimits.md](docs/RE/EngineLimits.md).

## AI

**Flyers on maps with no flyer paths** - A flyer on a map with no flyer paths just circles. The
engine already has a movement mode that needs no path, used for strafing runs. The idea is to
fall back to it when the flyer cannot find a path, so flyers work on maps that never had flyer
paths made. Details in [docs/RE/FlyerAI.md](docs/RE/FlyerAI.md).

## Controller

**Shell and menu navigation** - Gamepad support is gameplay only today. Every binding mode
(`Unit`, `Vehicle`, `Flyer`, `Hero`, `Turret`) maps to an in game control path, so the pad
does nothing in the front end: the main menu, the spawn screen, the map and unit selection,
and the pause menu all still need mouse and keyboard. That makes the controller support a
half answer for anyone actually playing from the couch.

**Traced 2026-09-07, and most of it already exists.** The shell is a controller UI with a
mouse bolted on, not the reverse: all 120 `ifs_*` screens implement `Input_Accept`,
`Input_Back`, `Input_GeneralUp/Down/Left/Right`, `Input_Start`, `Input_L/RTrigger`,
`Input_Misc`/`Misc2`, and the handlers already take a joystick index
(`metagame_ai.lua:46`, `ifs_meta_main:Input_Accept(iJoystick,1)`). Only 2 files use
`fnTestHotSpot` for mouse hit-testing, against 57 with explicit directional handlers and the
rest inheriting `gShellScreen_fnDefaultInputUp/Down`. So the per-screen navigation logic is
written and shipping - it is what the arrow keys drive today - and none of this needs shell
Lua changes, which matters because GameExt ships no shell.

Dispatch is generic and source-agnostic. `GuiManager::HandleEvents` (Steam `0x00528FB0`)
drains an event queue and builds the Lua method name at runtime,
`_snprintf(buf, 0x7F, "%s%s", "Input_", name_table[id*2])` then `CallLuaFunctionOfScope`. The
bare name table (Steam `0x007E66C4`) carries the full console set - `Accept`, `Back`,
`GeneralUp/Down/Left/Right`, `Start`, `LTrigger`, `RTrigger`, `Misc`, `Misc2`, `KeyDown`,
`Char` - so nothing is a reduced PC subset. The console controller-management API is intact
and used by the scripts too (`ScriptCB_ReadAllControllers` and friends, plus
`ScriptCB_GetVKeyboardCharacter`, an on-screen keyboard that only exists for pad text entry).

So the work is one thing: raise those events from the pad. **The open question is the enqueue
side** - `HandleEvents` is the consumer; who produces into that queue, and whether there is a
callable post/push entry point, has not been found yet. Find that before scoping anything.
Remaining risk after that is blast radius rather than difficulty: menu code runs before
everything, so a bug locks people out of the game entirely, and verifying it means walking a
lot of screens rather than one repro.

## Lua API

**`GetProperty` / `GetClassProperty`** - The read side of the existing `SetProperty` /
`SetClassProperty` pair. Same entity and class lookup, same property name resolution,
returning the current value instead of writing one. The sound entity limitation above
applies identically to `GetProperty`, and a `GetInstanceProperty` would need the same
per-family handling as `SetInstanceProperty`.

**Hero health drain switch** - A way to stop heroes from bleeding health over time. This
belongs in Lua rather than in an ODF property: the drain is a game rule, and an ODF entry
would force every hero class to be edited individually and would not let a script turn it off
for one mode and leave it on for another. Needs the code that applies the drain traced first,
then a toggle hung off that path.

**Restore `FlatInfo()`** - a `.sky` block BF1 rendered and BF2 does not parse at all. A
scrolling flat texture layer: a horizontal plane, separate from the dome, for a cloud or haze
sheet at a fixed world height. The schema survives in a stock BF2 asset,
`assets/worlds/END/world1/end1.sky`, where a porter left the block in and nothing has read it
since - `Height(0,0)`, `Texture`, `Color`, `Modulate`, `TextureSpeed` (the UV scroll that makes
it drift), `TileSize`. Endor's copy is inert, so the file proves the schema, not the visual.

**This is a rebuild, not a revival.** Sky blocks dispatch by PblHash, and
`PblHash("FlatInfo") = 0x4B936222` occurs nowhere in the modtools exe, as neither constant nor
string - while every supported block occurs exactly once (`SkyInfo` `0x06C0D3E6`, `DomeInfo`
`0x185AB6C2`, `DomeModel` `0x82681057`, `SunInfo` `0x3780C8F9`, `LowResTerrain` `0xDDF0C29E`).
Parser case and renderer were both compiled out, so it needs a new sky config case plus a
renderable drawing one textured, scrolling, world-height quad.

BF1's reader is `SkyDome::Read(PblConfig&)` at `0x001D10A0`, with `SkyDome::Render` the draw
side. **Neither read yet** - still open are the two `Height` values, how `Modulate` selects the
blend, and whether the plane draws before or after the dome. (Ghidra has not applied BF1's
symbols; resolve names through their Mach-O nlist entries, not by name lookup.)

**AI systems documentation** - Write up the goal layer (`AIGoalManager::AssignUnit`) and the
combat response layer (`SelectCombatResponse`), how a unit gets from a team level goal to an
individual combat action, and which ODF and Lua knobs actually feed into it. Should also
record the confirmed no-ops so others stop trying to use them.

**Improve RedConsoleCommands documentation** - The existing
[reference](docs/RE/RedConsoleCommands.md) was largely built from symbol names, so a number of
entries are inferred rather than verified. Needs a pass that actually exercises the
commands, corrects the inferred descriptions, and marks the ones confirmed to be dead
no-ops.

## Will not do

**Splitscreen.** For anyone reading this: no, splitscreen will not be a thing.

It is not a matter of flipping the disabled flag. The PC build compiles the relevant arrays
down to a single element, so simply unclamping the camera count writes past the end of those
arrays and corrupts memory rather than producing a second viewport. Making it real means
rebuilding those structures and every consumer of them. Notes on the system are in
[SplitscreenSystem.md](docs/RE/SplitscreenSystem.md) for the curious, but it is documentation, not
a plan.

**Saber heroes in first person.** Tried both ways and removed. The saber can be made to show up
in first person, but BF2's first person is floating arms with a fixed camera and no head
movement, and it looks janky for a saber hero no matter what. Doing it right means rewriting the
first person system. Everything learned is in
[FirstPersonAnimationSystem.md](docs/RE/FirstPersonAnimationSystem.md) in case someone tries again.

**More than 64 attached effects per object.** `AttachedEffectsOverflowFix` refuses past 64, and
that is where it stays. Going higher means moving the table and rewriting about twenty addresses
per build, and 255 is a hard ceiling anyway. Normal content never gets close.
