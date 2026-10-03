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

**AI spinning at sniper nodes** - AI stationed at a sniper hint node can start spinning on the
spot extremely fast. The cause is unknown. A facing target that flips every update, or a heading
the turn code cannot resolve, would both look like this. Next step is to catch a spinning unit
with `memwatch` on its heading and see what keeps writing it.

## Vehicles

**FinAnimation for AI** - The 9-pose `FinAnimation` on flyers and hovers never moves when AI is
flying: the fins sit in the neutral pose. The pose reads the vehicle's turn input for the
sideways axis and its speed for the other, and AI apparently never fills in the turn input.
Needs the sideways axis fed from the vehicle's actual turn rate instead, so AI and players
drive the same pose.

**Walker stomp attack cleanup** - The stomp / attack system is fully wired at runtime but
rough around the edges:

- Add dedicated `AttackEffect` and `AttackSound` ODF properties instead of the current
  hardcoded FX and sound.
- Revisit `AttackControls`. Values 1 and 2 are documented as primary / secondary fire but
  do not actually restrict to those, so the real semantics need pinning down.
- Write proper user documentation for enabling it. It is currently gated on
  `HealthType = "animal"` plus an `AttackAnimation` ODF entry.

**Flyer strafe mode ODF property** - Flyer strafing is fully built into the flight model but
its input was cut: `StrafeSpeed` and `StrafeRollAngle` are both multiplied by a constant 0, so
today the only thing that moves a flyer sideways is the side-roll trick
(`TrickSideRollStrafeSpeed`). The strafe stick axis (left stick X, A/D) drives roll instead.
Goal is an opt-in `EntityFlyer` ODF property (`EnableStrafe`) that feeds the strafe axis into the dead term, so
that axis moves the flyer sideways (and leans it by `StrafeRollAngle`) instead of rolling it.
Roll stays reachable through the alternate control mode, which rolls on the turn axis.
AI already produces a strafe value from `StrafeSpeed` (stock gunships and the MAF set it), which
in vanilla ends up as roll; in strafe mode it would strafe as the AI code intended. Whether that
flies well needs a play test. Patch sites are mapped for modtools, Steam and GOG.

**Test the flyer roll throttle fix online** - `[Fixes] FlyerRollThrottleFix` is untested in
multiplayer. Fly a GameExt client on a vanilla host and on a GameExt host, and roll with the
throttle held: the flyer should keep its speed without rubber-banding. If the host caps a remote
pilot's input again, keep the fix to the host's own pilot (or off for clients).

**Per-walker stomp speed** - `[Fixes] WalkerStompFix` lands a `StompDetectionType = "1"`
walker's step once its foot comes down faster than 3 m/s, the same for every walker. A
slow-footed walker's steps can still go unlanded: the AT-TE's first steps from a standstill
come down at 2.6 to 2.8 m/s. Add an ODF property (for example `StompDetectionSpeed`, in m/s,
inheriting through `ClassParent`) that sets the speed per class, read where the fix sets the
test for each walker's update.

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

**Real riot shields** - A shield that actually stops shots by its shape rather than by a
deflect rule. The engine already has almost everything this needs: a unit's collision can hold
up to 64 shapes, each shape can block shots without being targetable, damage multipliers already
work per shape (a multiplier of 0 blocks without hurting), and there is already a spot that
copies extra shapes onto the soldier for each stance. It is unused only because soldiers never
read collision properties from their ODF. The deciding question is whether a collision shape
can follow an animated bone. If not, the shield would be a fixed box in front of the unit, fine
while holding a block stance and wrong as soon as the arm moves. Settle that first. Details in
[docs/RE/SoldierCollisionSystem.md](docs/RE/SoldierCollisionSystem.md).

**Roll and jump camera sweep** - In third person the camera follows a roll or a jump rigidly.
Let it trail behind and look toward the unit's feet through rolls, jump launches and the fall
from a jump's apex, as BFIII's delay camera does, replacing `RollShake`'s plain shake. It needs
the soldier's position and vertical speed on each build. Details in
[docs/RE/CameraShake.md](docs/RE/CameraShake.md).

**Frame-rate independent chase camera** - BF2 eases the chase camera toward the unit by a step
that depends on the frame time, so a fast unit's camera sits a little further back at higher
frame rates and lurches on a slow frame. Replace the easing with the exact step for a moving
target. It changes the stock camera for every unit. Details in
[docs/RE/CameraShake.md](docs/RE/CameraShake.md).

## Weapons

**Other players' bolts are white on a multiplayer client** - The host sees every bolt in its
colour, and so does a client for its own shots, but a client sees other players' bolts white.
A client builds those from the host's create ordnance event, which picks the ordnance class by
an 8 bit net index, so the first thing to check is whether that index resolves to a different
class on the client than the firing weapon's own ordnance class. Not yet known whether stock
weapons do it too or only `dualcannon` ([docs](docs/user/classlabels/dualcannon.md)).

**Force pushable grenades** - Force push moves units but ignores thrown grenades. Goal is to let
a push catch a live grenade and send it back. Needs tracing first: what the push picks up as
targets, and how to redirect ordnance that is already in flight. Grenades should opt in through
a property on the ordnance ODF, not through a ClassLabel, so rockets and mines are not swept up
with them.

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
- **B. Terrain decals.** Rebuild `ComputeTerrainDecal` and drive it from A's spawn path.
  Retail dead-stripped it, but the original body is known, so this is transcription, not
  invention. Gives a validated reference decal to check step D against.
- **C. Find a `RayTest` that returns hit position plus normal.** Untraced. Blaster bolts must
  already compute one to place impact effects. Unblocks D and may shrink it considerably.
- **D. Write `ComputeObjectDecal`.** The real work, and the only piece with no existing body
  to copy - it is empty in every build. Clip the projection quad
  against the collision object's triangles. Q3-derived implementations are GPLv2 and this
  repo is MIT, so read for design only.
- **E. Wire it up.** Hook `WeaponMelee::UpdateFire` after the ray test returns true; the
  object, ray index and segment are all in hand there.

A and B are confidently estimable. C and D are where the schedule can move. Once the
pipeline exists, blaster impacts and explosion scorch marks are nearly free and will be far
more visible in normal play than saber marks.

**Restore `FlatInfo()`** - BF1 skies could have a flat, scrolling texture layer at a fixed
height, separate from the dome, for a sheet of cloud or haze. BF2 cut it completely, both the
`.sky` parsing and the rendering, so this means building it again, not switching it back on. The
properties are known from a leftover block in Endor's `.sky` file. What is still missing is how
BF1 actually drew it. Details in [docs/RE/SkySystem.md](docs/RE/SkySystem.md).

**Unlock framerate above 80 FPS** - The game caps out around 80 and anything above this will
cause issues with vehicles, especially hovers, due to being tied to the framerate. The fix is to detach the physics and animation updates from the framerate, and instead run them on a fixed timestep. This will allow the game to run at higher framerates without affecting gameplay mechanics.

## HUD

Research and addresses for these are in
[docs/RE/HUDSystem.md](docs/RE/HUDSystem.md#candidate-events-researched-2026-09-25-not-built),
[world markers and distances](docs/RE/HUDSystem.md#world-markers-and-distances-researched-2026-09-28),
[Lua and the HUD](docs/RE/HUDSystem.md#lua-and-the-hud-researched-2026-09-28) and
[docs/RE/HUDElementProperties.md](docs/RE/HUDElementProperties.md).

**HUD shake** - The HUD stays still while the camera shakes, so a strong shake leaves the
reticle off the true aim point. Publish the camera shake's screen offset as a HUD event so a
reticle can follow it. Details in [docs/RE/CameraShake.md](docs/RE/CameraShake.md).

**Spread-scaled reticle** - The reticle stays one size whatever the weapon's current spread.
Publish each weapon's live spread as an angle and as an on-screen scale for `EventScale`, so
the reticle blooms with recoil and tightens with stance.

**Compass, pitch and zoom** - A HUD cannot show a compass, an artificial horizon or a zoom
readout. Publish camera heading, pitch and zoom, pure camera maths like horizon levelling.

**Speed and altitude** - Nothing tells the HUD a vehicle's speed or height. Publish both
for speedometers and altimeters.

**Damage direction** - Players cannot tell where damage came from. Publish a rotation that
points at the last attacker and keeps tracking them, from the damage hook aim assist uses.

**Kill confirm** - There is no kill cue beyond the feed. Check whether the stock kills
statistic already fires on each kill; otherwise add a pulse from the kill feed.

**Missile and grenade warnings** - The missile warning gives no direction and nothing warns
about grenades. Publish a bearing and distance for the incoming missile and live grenades.

**Objective waypoints** - The stock `Target` element already draws script markers on
objects in 3D with edge arrows, but every objective shares one icon and region markers
get nothing. Publish each marker's screen position so a HUD can style them itself.

**Friendly name tags** - Teammates are hard to pick out online. Publish a fixed set of slots
with name, health and position for teammates in view.

**Off-screen arrows** - The floating target bar vanishes when its target leaves the
screen. Pin it to the edge with a rotation, as the command post markers already are.

**Script-assigned markers** - `MapAddEntityMarker(..., true)` already puts a stock 3D
marker on an entity and replicates it online, but with the shared objective icon and no
distance. Add `player1.trackN.*` slots with the command post markers' events, filled by
a Lua call that names an entity and a height; needs a design for slot ownership between
scripts, and a timer or callback only fills them on the host.

**Script HUD events** - A mission script cannot show, hide or feed a HUD element. Add
`HudEventCreate`/`HudEventSend` for a reserved `lua.` namespace, created at the top of
`ScriptInit`, before the `.hud` files load.

**Alpha, scale and rotation on any element** - Only groups take `EventAlpha` (for one
frame), `EventScale` and `EventRotation`. Extend all three to every element through the
reader every class shares, with a persistent alpha.

**Sub-pixel per element** - `[Features] HudSubPixel` is all or nothing and each player's
own choice, so a mod cannot ship gliding markers without softening still text as well.
Add a `SubPixel(1)` property covering an element and everything inside it; the reader,
a spare flag bit and the draw are in
[pixel snapping](docs/RE/HUDSystem.md#pixel-snapping-built-2026-09-29).

**Colour gradients** - An element is drawn in one tint, so an icon or bar can only show a
gradient baked into its texture. Add a `Gradient` property for bitmaps and bars that draws
them through the engine's masked-bitmap mode with a gradient texture GameExt builds from
the colours, kept on a bar's full length as it fills. Details in
[colour gradients](docs/RE/HUDSystem.md#colour-gradients-researched-2026-09-30).

**Repeated HUD blocks** - A `.hud` must write out a command post slot or marker, and its
transforms, once per slot: `EventNameFilter` only renumbers by camera, once on PC. Add a
`Repeat` block that re-reads its contents N times with the filter set to each index, as
`ViewPort` does per camera.

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
  Not yet located on modtools, Steam or GOG, and where the instance name lives on
  `EntitySound` is not yet known. That
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

**Flyer shakes on retail** - On Steam and GOG a flyer's `TurnShake` and `CollisionShake` do
nothing, and speed names in a flyer's `Threshold` never resolve, so its default boost and brake
shakes stay quiet there and speeding up is judged by speed change instead of the throttle. The
flyer fields the rework reads (`mGetSpeedSpeed`, the forward axis, the class's speeds and turn
rates, `mControlMove`, `mControlStrafe`, `mInLandingRegionFactor`) need reading on retail, and
retail `ApplyShake`, which takes its amount and duration in XMM1 and XMM2, needs a stand-in at
the two collision calls. Details in [docs/RE/CameraShake.md](docs/RE/CameraShake.md).

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

**New AI behaviours** - AI never sprint (nothing in the AI controllers or agents touches the
sprint control), melee units trade blows with little blocking or spacing, and general behaviour
has few tactical touches. The behaviour layer is built for swapping: `UnitAgent` and its 14
behaviours each have their own `EnterState` / `UpdateState` / `ExitState`, so new behaviour can
be added there, starting with sprinting to distant goals out of combat and blocking and backing
off in melee duels. Needs the goal and combat layers mapped first (AI systems documentation,
below). Details in [docs/RE/AISystem.md](docs/RE/AISystem.md).

**Flyers on maps with no flyer paths** - A flyer on a map with no flyer paths just circles. The
engine already has a movement mode that needs no path, used for strafing runs. The idea is to
fall back to it when the flyer cannot find a path, so flyers work on maps that never had flyer
paths made. Details in [docs/RE/FlyerAI.md](docs/RE/FlyerAI.md).

## Controller

**Shell and menu navigation** - The pad does nothing in the menus: main menu, spawn screen, map
and unit selection and the pause menu all still need mouse and keyboard. The menus themselves
were built for a controller, but PC ships the table that turns pad buttons into menu inputs
almost empty, with the first four buttons all set to Accept. A few PC screens also only work
with the mouse: the profile screen only accepts a mouse click, and the top tab row cannot be
reached from the buttons at all. A fix for both is written and stashed
(`Controller Menu Navigation`): it fills in the table and adds a small Lua patch to the shell
that fixes the profile screen and lets the pad triggers switch tabs. It has not been tested yet.
Details in [docs/RE/GuiInputSystem.md](docs/RE/GuiInputSystem.md).

## Lua API

**`GetProperty` / `GetClassProperty`** - The read side of the existing `SetProperty` /
`SetClassProperty` pair. Same entity and class lookup, same property name resolution,
returning the current value instead of writing one. The sound entity limitation above
applies identically to `GetProperty`, and a `GetInstanceProperty` would need the same
per-family handling as `SetInstanceProperty`.

**`SetClassProperty` for GameExt's ODF properties** - The camera shake properties (`FireShake`,
`RollShake` and the rest) are read as the ODF loads, so a script's `SetClassProperty` hands them
to the class's stock `SetProperty`, which ignores them. Stock `SetClassProperty` also only finds
entity classes, so weapon classes are out of reach altogether. Route those names through the
same listeners from the class `SetProperty` (the soldier one is already hooked on every build;
`EntityFlyerClass::SetProperty`, Phantom `0x00529ED0`, and the walker's and hover's need
porting), and let
`SetClassProperty` fall back to weapon classes by name. Details in
[docs/RE/CameraShake.md](docs/RE/CameraShake.md).

**Hero health drain switch** - A way to stop heroes from bleeding health over time. This
belongs in Lua rather than in an ODF property: the drain is a game rule, and an ODF entry
would force every hero class to be edited individually and would not let a script turn it off
for one mode and leave it on for another. Needs the code that applies the drain traced first,
then a toggle hung off that path.

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
