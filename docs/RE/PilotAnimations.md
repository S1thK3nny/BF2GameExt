# Pilot and rider animations

How a vehicle poses the soldier riding it, and how a walker's rider could follow the walker's
movement: turning, starting, walking and stopping. Researched 2026-10-11 by a read-only pass in
Ghidra; nothing is built, and each address is to be re-read before it is used. P = Phantom,
M = modtools, S = Steam, G = GOG; S and G addresses move with the exe.

## The ODF side

`ControllableClass::SetProperty` (P `0x004EF090`) reads every seat's pilot keys:

- `PilotAnimation`, registered through `SoldierAnimationBank::AddAnimation` (P `0x0074D7B0`) in a global
  table of 10 names. The 11th name gets nothing back and that seat's pilot is silently hidden.
  ComboAnimIncrease keeps the limit at 10 per map.
- `PilotPosition` (a hardpoint) and `AnimatedPilotPosition` (one of the vehicle's animated bones).
- `Pilot9Pose` (hash `0xA976D065`): a pilot animation plus the flag that forces its frames from vehicle
  state.
- `IsPilotExposed` (`0x2B60E217`, default 1; 0 hides the pilot), `PilotType`, `ControlsUnit`
  (`0x44C77C95`).

The animation is looked up as `<bank>_<name>` (for example `human_drive_1manatst`) in the soldier's
bank and its parents.

## Who shows a rider

Any seat whose class has a `PilotAnimation`. Among the stock walkers:

- the AT-RT: `drive_1manatst`, a single-frame pose on the walker's `bone_root`;
- the tauntaun and kaadu: `ride`, a 61-frame loop, while the tauntaun's stride is two 11-frame clips,
  so the rider is visibly out of step;
- the AT-TE's top gunner turret: `drive`;
- the AT-ST, AT-AT and spider walkers have no pilot animation, so their pilots are hidden.

## How a rider is posed

**Every update, from the vehicle.** The vehicle calls `ControllableClass::UpdatePilotAnimation`
(P `0x004EFF40`, M `0x005E0150`, S and G `0x00484ED0`; plain thiscall on all three, eight stack
arguments, RET 0x20) with the pilot, the vehicle's pose, matrix and velocity, the frame time, and
`frame1`, `frame2` and `blend`. It calls the pilot's `SetPilotInfo` (P `0x0057A5F0`, M `0x0054B2F0`,
S `0x004F2EC0`), which stores those three values (soldier +0xA0C/+0xA10/+0xA14 on M, 0x10 lower on
retail) and the animation's hash, and sets the soldier's state to 17 (18 with `ControlsUnit`).

- The walker always passes three literal -1s, from `EntityWalker::PostCollisionUpdate` (P `0x005970D0`;
  M `0x00559140`, call at `0x005599AF`, pushes at `0x00559977`, `0x0055997C` and `0x00559981`, walker
  in EBX; S and G `0x00505C80`, call at `0x005065D7`, stores at `0x0050657F`, `0x00506587` and
  `0x005065A1`, walker in ESI).
- The flyer passes -1s too. The hover passes its 9-pose (`EntityHover::Move`, P `0x005423B0`): frame
  roll + 4, second frame roll + 1 or + 7, blend the absolute pitch.
- Turrets, passenger slots, droidekas and remote terminals call it as well: hovers, flyers and
  walkers share this one path.

**When drawn.** `EntitySoldier::Render` (M `0x00535D90`) passes the hash through `SetAction` and the
forced frames into the animator, calls `SetupPose`, then resets the frames to -1. The pilot branch of
`SetupPose` (M `0x0057CA90` to `0x0057CC08`; S from `0x0063FC1A`) loops the animation at 30 frames a
second. With `frame1` below 0 it copies the whole player into the pose, so a change of pilot animation
is a hard cut: the blend time the player records is never used there. Otherwise it scrubs to `frame1`
and blends toward `frame2` by `blend`. It ends with `ZephyrSkeleton<32>::BuildWorldMatrices`
(call at M `0x0057CBF0`, S `0x0063FD84`, G `0x00640E24`; thiscall, RET 4). It never reaches
`ApplyProceduralAnimationAndBuildWorldMatrices`, so riders get no aim turn and never meet the
directional jets' stand-in.

## What a walker knows

`mState` (M +0x20B8, retail +0x2078) indexes the walker's clip list (`EntityWalkerClass::PostReadSetup`,
P `0x00598350`):

| State | Clip |
|---|---|
| 0 | idle |
| 1, 2 | turn left, turn right on the spot |
| 3, 4 | dying, dead |
| 5, 6 | start: idle to the left or right foot |
| 7, 8 | the two walking half-strides |
| 9, 10 | stop: the left or right foot to idle |
| 11 and up | attacks |

`SetState` (P `0x0059C4E0`, table at P `0x00A8F0B0`) changes state only when a clip ends; walking
backward plays the same clips in reverse. `UpdateState` scales the clip rate by speed, which is why a
rider looping on its own clock drifts. The gait phase is the clip player's time, from the blend
(M +0x1C44, retail +0x1C04), then unit 0 (+0x2C), then its time from 0 to 1 (+0x131C), as
`WalkerBlend::GetTurnState` reads it (M `0x005897A0`, S `0x00675CE0`). The walker cross-fades its own
clips over 0.2 s. Steering while walking only turns the body; there is no state for it. On clients
`EntityWalker::Render` (P `0x005988C0`) swaps in the network-interpolated frame only while drawing the
walker, then puts the local one back.

## Designs

**A. The walker as a 9-pose.** Retarget the walker's `UpdatePilotAnimation` call (the three -1s are
literals) to a stand-in that fills `frame1`, `frame2` and `blend` from the state and phase; the engine
does the posing. The variants would be frame ranges in one long pilot animation, for example
`PilotStateFrames = "<state> <first> <last>"`. Small, and no extra names, but one strip of an
animation per rider, a two-way blend only, at the walker's simulation rate.

**B. Named variants blended when drawn (recommended).** Retarget the pilot branch's
`BuildWorldMatrices` call to a register-keeping stand-in that finds the rider's walker (soldier, then
`Character::mVehicle`, then `GetGameObject`: a turret seat answers with its vehicle, a passenger slot
probably too, not read), reads its state and phase, and blends `<bank>_<PilotAnimation>_turnleft`,
`_turnright`, `_start`, `_stop` and `_walk` onto the pose in time with the walker's stride, easing the
weights over the walker's own 0.2 s. The variants are found through `soldier_anim_tables` and never
registered, so they cost none of the 10 names. A walker ODF switch such as `UsePilotStateAnims`,
inherited through `ClassParent`, turns it on; a turn-rate threshold could lean the rider while the
walker steers. About the size of directional jets.

## Risks

- The ODF listener's four property handlers are all taken, so a new property needs a fifth slot or a
  shared handler.
- Purely local and visual: nothing goes online. On clients the rider may drift from the drawn legs,
  since it would read the walker's local phase rather than the interpolated one (inferred).
- ComboAnimIncrease moves the per-map pilot table, which design B does not read; the `<bank>_<name>`
  finder belongs in `soldier_anim_tables`.

## Open

- Does a client's walker state match the host's? A play test with a remote AT-RT.
- Which phase is current when the rider is drawn, the local or the interpolated one? Design B could
  keep the value the walker's own `Render` uses.
- Does a passenger slot's `GetGameObject` return its vehicle, as a turret's does?
- Should steering while walking play the turn variants, and from what turn rate?
- Should variants blend the whole body or each half separately? A single-frame variant must not loop.
- The stock AT-RT pose is a single frame, so nothing changes until the variants are animated.
