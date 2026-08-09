# Roadmap

Planned and in-progress work. This is the backlog, not a feature list: nothing here
is shipped. For what the DLL actually does today, see
[docs/user/FEATURES.md](docs/user/FEATURES.md).

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

**Lightsaber illumination (optional feature, 1.1.0)** - Lightsaber blades are drawn as
glowing geometry but emit no actual light, so a saber lights nothing around it. A working
idea would be to have each blade register a real omni light with the engine's lighting
system, colour taken from the blade's own ODF colours.
A problem however is that the engine allows only four omni lights per object, so sabers compete with every other
light in the area, and it is the same four-light wall documented in the per-object light
limit notes. That contention is exactly why this should ship opt-in rather than on by
default: a saber silently stealing a light slot from a map's own lighting is a worse
default than no saber glow at all. Targeted at 1.1.0 alongside the decal work, as a
combined "saber and impact visuals" release.

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
