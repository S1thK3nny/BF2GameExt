# Roadmap

Planned and in-progress work. This is the backlog, not a feature list: nothing here
is shipped. For what the DLL actually does today, see
[docs/user/FEATURES.md](docs/user/FEATURES.md).

## Bugs

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

## Sound

**Sound region and stream manipulation** - Goal is runtime control over ambient sound
regions and streams. Current state of the problem:

- Lua `SetProperty` can never reach sound entities. `EntitySound` derives from `Entity`,
  not `EntityEx`, so it is absent from the id map `SetProperty` looks in. This is
  structural, not a missing case.
- `SetClassProperty` does work on `SoundAmbienceStatic` and `SoundAmbienceStreaming`, but
  only for four properties (`Sound`, `SoundStream`, `MinDistance`, `MaxDistance`) and only
  before the entity is created, since it edits the class and not the instance.
- Sound regions clobber those values anyway once they take over.
- So the open question is a respawn or re-apply path: change the class, then force the
  existing sound entities to rebuild from it, without regions immediately overwriting the
  result.

## Lua API

**`GetProperty` / `GetClassProperty`** - The read side of the existing `SetProperty` /
`SetClassProperty` pair. Same entity and class lookup, same property name resolution,
returning the current value instead of writing one. The sound entity limitation above
applies identically to `GetProperty`.

**Hero health drain switch** - A way to stop heroes from bleeding health over time. This
belongs in Lua rather than in an ODF property: the drain is a game rule, and an ODF entry
would force every hero class to be edited individually and would not let a script turn it off
for one mode and leave it on for another. Needs the code that applies the drain traced first,
then a toggle hung off that path.

## Documentation

**AI systems documentation** - Write up the goal layer (`AIGoalManager::AssignUnit`) and the
combat response layer (`SelectCombatResponse`), how a unit gets from a team level goal to an
individual combat action, and which ODF and Lua knobs actually feed into it. Should also
record the confirmed no-ops so others stop trying to use them.

**Improve RedConsoleCommands documentation** - The existing
[reference](docs/RE/RedConsoleCommands.md) was largely built from symbol names, so a number of
entries are inferred rather than verified. Needs a pass that actually exercises the
commands, corrects the inferred descriptions, and marks the ones confirmed to be dead
no-ops.

## Debugging

**Add warnings before crashing due to missing chunks** - The engine will crash if a chunk is missing from the map, but it does not log a warning at all. There's no real reason to do this, except it caught me off guard a few times and I want my revenge on the engine. The warning should include the chunk details.

## Will not do

**Splitscreen.** For anyone reading this: no, splitscreen will not be a thing.

It is not a matter of flipping the disabled flag. The PC build compiles the relevant arrays
down to a single element, so simply unclamping the camera count writes past the end of those
arrays and corrupts memory rather than producing a second viewport. Making it real means
rebuilding those structures and every consumer of them. Notes on the system are in
[SplitscreenSystem.md](docs/RE/SplitscreenSystem.md) for the curious, but it is documentation, not
a plan.
