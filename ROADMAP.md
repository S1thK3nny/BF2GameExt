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

## Rendering

**Lightsaber illumination** - Lightsaber blades are drawn as glowing geometry but emit no
actual light, so a saber lights nothing around it. A working idea would be to have each
blade register a real omni light with the engine's lighting system, colour taken from the
blade's own ODF colours.
A problem however is that the engine allows only four omni lights per object, so sabers compete with every other
light in the area, and it is the same four-light wall documented in the per-object light
limit notes.

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
