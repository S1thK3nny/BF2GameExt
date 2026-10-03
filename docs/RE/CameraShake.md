# Camera shake - RE notes

How the stock camera shake works, why it feels harsh, why it never moves the aim, and
how GameExt redraws it as a blast and adds shake for firing, hits, landings, rolls and
sprinting (`render/camera_shake.cpp`, built 2026-09-27 for modtools, Steam and GOG; the
shapes follow a BFIII-derived camera spec), and for flyers: boosting, hard turns,
braking, bumps, tricks, take-off and landing (reworked 2026-09-30 on modtools, from the
user's own flight model and BF2's flight code; see [Flyers](#flyers)), and melee: swings,
strikes, blocks and deflections (2026-10-02; see [Melee](#melee)). Read on the Phantom build, which has symbols,
then ported. Addresses are Phantom and unrelocated (imagebase `0x400000`) unless
stated. User-facing details are in [ODF_PROPERTIES.md](../user/ODF_PROPERTIES.md#camera-shake).

## The stock shake

Everything goes through one small queue on the local player's chase camera.

`CameraManager::ApplyShake(amount, duration)` (`0x00499B50`, thunk `0x00407EF0`)
appends to `mChaseCamera[0]`: `mShakeAmount[n] = amount` and
`mShakeDecay[n] = amount / duration`. There are four slots; a fifth shake while four
are running is dropped. `ClearShake` (`0x00499C20`) empties the queue.

Each frame `ChaseCamera::Update` (`0x004A2080`, vtable `+0x04`) adds the four amounts
into `mShake`, then lowers each by `decay * dt` and removes it at zero. So each shake
fades out linearly over its duration, and overlapping shakes add up.

`ChaseCamera::SetupCamera` (`0x004A1450`, vtable `+0x08`) builds the view:

1. The owner's `SetupCameraTrackMatrix` (Trackable vtable `+0x08`) writes the camera
   matrix into `mMatrix`, easing it from the one left there last frame (see
   [The camera eases from last frame's view](#the-camera-eases-from-last-frames-view)).
2. Interpolation and camera collision adjust it.
3. If `mShake > 0` and the mission time is past `mShakeSuppressUntil`, `mMatrix` is
   copied to `mPreShakeMatrix` and multiplied by rotations about X, Y and Z, each of
   `BiasedRandom() * mShake * 0.1` radians.
4. `RedCamera::SetMatrix` receives `mMatrix`.

`BiasedRandom` (`0x004A12B0`) returns `r * r * r` for `r` uniform in -1..1, drawn from
`PblRandom::_Global`: six draws per frame while a shake runs.

| `ChaseCamera` field | Offset in the object |
|---|---|
| `mOwner` (Trackable) | `+0x0C` |
| `mMatrix` | `+0x10` |
| `mPreShakeMatrix` | `+0x50` |
| `mTimer` | `+0x90` |
| `mShake` | `+0x94` |
| `mShakeSuppressUntil` | `+0x98` |
| `mShakeCount` | `+0x9C` |
| `mShakeAmount[4]` | `+0xA0` |
| `mShakeDecay[4]` | `+0xB0` |

The object starts with the vtable (`0x009E3990`) and `GameCamera_data` (`mManager +4`,
`mView +8`); `ChaseCamera_data` begins at `+0x0C`.

### The camera eases from last frame's view

`mMatrix` is both what `SetupCamera` hands the renderer and where it starts the next
frame. `CameraTrackSetting::SetupCameraTrackMatrix` (`0x0077C5E0`; `0x0077D070` blends
two settings; modtools `FUN_004BBBA0`, the same logic), which the soldier's and the
flyer's `SetupCameraTrackMatrix` call with `mMatrix`, reads the old position and
orientation from it before writing the new ones:

- **Position:** the step to where the camera should be is split along the new view's
  right, up and back axes, and each part is cut to `1 - e^(-T * dt)` of itself. `T` is
  the class's `MoveTensionX` (moving right, then left), `MoveTensionY` (up, down) or
  `MoveTensionZ` (back, in): one value sets both directions, and `MoveTension` sets all
  six (`CameraTrackSetting::SetProperty`, `0x0077C130`). The default is 30. Stock
  starfighters use `MoveTensionZ` 4.25; a soldier camera with `"30 8"` comes back in
  at 8.
- **Orientation:** a blend toward the new one that keeps `e^(-AimTension * dt)` of the
  old. A soldier passes its `AimTension` (default 1000, so next to nothing is kept). A
  flyer passes `FLT_MAX` outside the trick camera, so none, and 20 to 50 while it lasts.
- **ChaseCamera's own blend:** while `2 * mTimer` is under 1 and the view is not
  zoomed, `SetupCamera` moves only `2 * mTimer` of the way from the old position and
  `(2 * mTimer)^2` of the way from the old orientation, each frame. `SetOwner` zeroes
  `mTimer` and `Update` adds `dt`, so this settles a new owner over half a second; a
  flyer's trick, side roll or crash sets it to 0.2, 0.48 or 0.25 to detach the camera.

Nothing hands `mPreShakeMatrix` back (`ChaseCamera::Update` only advances `mTimer` and
the queue), so whatever is in `mMatrix` at the end of a frame is where the next one
starts. The stock shake only turns the view, so only the orientation blends carry it
on. A camera moved back by `p` and left there piles up instead: each frame eases off a
share and the next adds `p` again, so the view settles about `p / (1 - e^(-T * dt))`
back. That is several times `p`, and more the higher the frame rate: 6.5 times at 60
frames a second with `T` 10, 15 times at 144. Uneven frames move it in and out: with a
boost's 2 m push easing through 1 m at `T` 10, one 45 ms frame among 60 fps ones
throws the view 1.7 m forward at once, and it creeps back. That was the choppy ease into `BoostShakeSteady`
(2026-10-01), and GameExt now hands the unshaken matrix back (see As built).

BF2's own follow does a smaller version of the same. A camera following a unit at
speed `v` trails it by about `v / T - v * dt / 2`, so it sits a little further back at
higher frame rates and lurches on a slow frame: 0.6 m for that same 45 ms frame at
137.5 m/s with `T` 10.

### Why it feels harsh

- **Every frame is a new random angle.** Nothing links one frame to the next, so the
  view buzzes rather than sways, and the buzz gets finer as the frame rate rises.
- **It is large.** Explosion `Shake` values in the stock ODFs run from 0.1 to 4.0. At
  4.0 a single frame can turn the view by up to 0.4 radians, 23 degrees, on each axis.
  The cube keeps most frames small, so the result is fine jitter with occasional big
  jolts.
- **It rolls the view as much as it pitches and yaws it.**
- **It fades linearly and then stops dead.**

### It never moves the aim

The shake changes only the matrix handed to the renderer, after everything that aims
has taken its own:

- **Third person:** `EntitySoldier::UpdateWeaponAndAimer` (`0x0058AD80`) builds
  `mTargetInfo.mAimStart` and `mAimPoint` from the unit's eye point and its
  `CameraTrackSetting` maths (`CalcEyePoint`, `CalcCameraPoint`,
  `GetCameraTiltValue`), not from any camera object. `PlayerController::Update`
  (`0x0071CAA0`) picks targets from that `mTargetInfo`.
- **First person and cockpits:** `FirstPerson::UpdateAimer` (`0x005B80C0`) is called
  from each entity's `SetupInterpolatedCameraTrackMatrix` with the matrix it has just
  built. That is step 1 above, before the shake is added.
- **HUD targeting:** `Controllable::CalculateScreenCoords` (`0x004EDC50`) uses the
  entity's own `SetupCameraTrackMatrixHUDTargeting`.
So shots go exactly where they would without a shake. What does move is the picture,
and with it the reticule: `ReticuleDisplay::Update` (`0x0073BA60`) places the reticule
by projecting the aim point, `mAimStart + mEyeDir * 1024`, through the render camera's
`_MatrixInverse` (`+0x70`) and `TransformCameraPointToProjectionSpace`, which uses only
near, far and field of view. It keeps only the height: x is fixed at 0.5. So the
reticule follows the aim point across the shaken picture, down the screen as the view
kicks up, and its interpolator adds a bounce of its own. The stock shake moves it the
same way; GameExt's shake is kept off it (see the reticule under As built).
`LockOnManager::UpdateTargetVisibility` (`0x0063D4D0`) takes the render camera, so
lock-on visibility tests see the shaken view.

Drawn from the shaken `mMatrix`: the first-person arms and cockpit
(`FirstPersonRenderable::Activate` `0x005B58D0`, `UpdateCockpit` `0x005B8220`) and
`SpaceDustEffect`.

### What shakes today

| Source | Amount and duration | Tuned by |
|---|---|---|
| Explosions (`ExplosionClass::CreateExplosion`, `0x005B2D30`) | `Shake` and `ShakeLength`, both scaled down with distance across `ShakeRadius` | the explosion ODF |
| A walker's death (`EntityWalker::Render`, `0x005988C0`) | `DeathShakeForce` and `DeathShakeDuration`, scaled down across `DeathShakeRadius`, after `DeathShakeDelay` | the walker ODF |
| Flyer collisions (`EntityFlyer::CollisionCallback` `0x00524340`, `PostCollisionUpdate` `0x005274B0`) | impact × 0.8 and impact × 0.7; above 0.5 also sets `mTimer = 0.1` | not tunable |

`EntityFlyer::SetupCameraTrackMatrix` (`0x0052CD70`) clears the queue for a flyer
class with bit `0x04` of `+0x9F0` set while it has no pilot, no energy or is crashing.
Nothing shakes the camera when a soldier is hit, fires, rolls or sprints.

## As built

**Drawing.** A detour on `ChaseCamera::SetupCamera` holds `mShakeSuppressUntil` at
`FLT_MAX` for the call, so the stock turn is skipped,
then moves and turns `mMatrix` itself and calls `RedCamera::SetMatrix` again. The offset
is in the camera's own frame: pitch, yaw and roll, and a move along its right, up and
back axes, with no cap: an ODF's values are used as given (the user's call, 2026-10-01), and
only an axis that is not a number is dropped. There are no INI settings either (removed
2026-10-01, the user's call: the ODFs decide); the stock shake is always drawn as the
blast, and the ODF shakes are always on unless the ODF reader has no listener slot left
for them. Everything is sampled by time, in double
precision so a long session does not coarsen the phase, so the frame rate does not
change the result. `mPreShakeMatrix` gets the unshaken matrix, as stock leaves it. Like
stock, the moved `mMatrix` stays in place for the rest of the frame, so the first-person
arms and cockpit follow the view.

**Handing the camera back.** Before calling `SetupCamera`, the detour puts back the
matrix `SetupCamera` made last frame in place of the shaken one, if the shaken one is
still there (`LeftShake` in `camera_shake_core.hpp`), so BF2's easing starts from its
own camera ([The camera eases from last frame's view](#the-camera-eases-from-last-frames-view)).
The shake is drawn on top and never fed back, so a push is its own size at any frame
rate (2026-10-01). Until then a push piled up. The push defaults were raised to what
they used to show at 60 frames a second under the soldier camera they were tuned with
(`MoveTensionZ "30 8"`): fire 0.015 to 0.06, the roll 0.1 to 0.7, the blast 0.03 to
0.08 per unit.

**The blast.** The stock queue is drawn as the spec's blast:
- **Motion:** roll and a small camera move on three sines at the spec's ratios (3.43,
  2.67 and 2.87 Hz at the default rate of 3), with no pitch or yaw.
- **Size:** the strongest queue entry (`mShakeCount` at `+0x9C`, `mShakeAmount[4]` at
  `+0xA0`), not `mShake`'s sum, so overlapping explosions never add up. The queue's own
  linear decay fades it.
- **Ceiling:** it levels off above about 2.5 units (`2.5 × tanh(x / 2.5)`), so a
  detpack's 2.0 is not four times a grenade's 0.5.
- **Zoom:** it is divided by `RedCamera::_fZoom` (`+0x140`), so it shrinks while zoomed
  in.
- **Defaults and override:** 6 degrees of roll and 0.08 m of movement per unit. The
  unit being viewed can reshape it with `BlastShake`, or turn it off.

**Shakes and triggers,** for the object the chase camera follows (Trackable
`GetGameObject`):

| Shake | Trigger | Kind |
|---|---|---|
| `FireShake` | `Weapon::SignalFire` detour, when the weapon's owner (`Weapon +0x6C`, its Trackable at `+0x18`) is that object; class from `Weapon +0x64`, else `+0x60`; once per weapon per frame (`ShotGate`), see below | one-off; adds up, 8 at most |
| `HitShake` | its health (`GameObject +0x144`) dropping, below | one-off; restarts |
| `LandShake` | soldier `mState` going from airborne (4 `JUMP`, 6 `JET_JUMP`, 7 `JET_HOVER`, 8 `FALL`) to grounded (0 `STAND`, 1 `CROUCH`, 2 `PRONE`, 3 `SPRINT`, 5 `ROLL`, 19 `SLIDE`) after at least 0.25 s; a walker's `m_fGroundedTimer` dropping back after at least 0.5 s, sized by its fastest fall in its `Threshold` (see [Walkers](#walkers)) | one-off; restarts |
| `StepShake` | a bit coming on in the walker's `mFootState`, one kick for all the feet that landed in an update, rolled toward their side | one-off; restarts |
| `JumpShake` | the walker's jumping flag (`0x80`) coming on | one-off; restarts |
| `RollShake` | soldier `mState` turning to 5 (`ROLL`), third person only | one-off; restarts |
| `SprintShake` | soldier `mState` 3 (`SPRINT`), third person only | held |
| `BoostShake` | the flyer's `mGetSpeedSpeed` in its `Threshold`, full while it speeds up toward a throttle target the throttle or a boost has raised and `Steady` of that once there, while `mState` is 2 (`FLYING`); a walker's ground speed in its `Threshold` while its `mBoost` bit is set | held |
| `TurnShake` | seconds of hard turning, measured from the flyer's forward axis, in its `Threshold`, times speed over `MaxSpeed`, while `FLYING`; a walker's `mState` 1 or 2 (turning on the spot) | held |
| `BrakeShake` | the same speed in its `Threshold` (one value: at or below it; default `MinSpeed` to `MaxSpeed`), full while it brakes: slows toward its throttle target with `mControlMove` at -0.1 or below, while `FLYING` | held |
| `CollisionShake` | the flyer's two stock collision shakes, retargeted; sized by the change in `mVelocity` | one-off; restarts |
| `TrickRollShake`, `TrickFlipShake` | `EntityFlyer::DoTrick` detour when it starts a trick: a flip if it set flag bit `0x02` | one-off; restarts |
| `TakeoffShake` | flyer `mState` going from 0 (`LANDED`) to 1 (`TAKEOFF`) | one-off; restarts |
| `LandingShake` | flyer `mState` going to 0 (`LANDED`) from 3 (`LANDING`) or 2 (`FLYING`) | one-off; restarts |
| `SwingShake` | the `SignalFire` detour, for a weapon whose `IsMelee` is true, in place of `FireShake` | one-off; restarts |
| `StrikeShake` | `WeaponMelee::UpdateFire` listing an object that does not block (see [Melee](#melee)) | one-off; restarts |
| `SwingBlockedShake` | a `Deflect` with no ordnance returning true during the viewed unit's `UpdateFire` | one-off; restarts |
| `BlockShake`, `DeflectShake` | the viewed unit's `WeaponMelee::Deflect` returning true, for a strike or a bolt | one-off; restarts |
| `BlastShake` | the stock queue | held; on by default |

BF2 signals fire once per round, not once per trigger pull. `Ordnance::Ordnance` (Phantom
`0x006EA370`) ends by calling the firing weapon's `SignalFire` (`OrdnanceDesc::mFireWeapon`,
call at `0x006EA645`), and `OrdnanceEmitterClass::Build` does the same (`0x006F11F3`).
`WeaponCannon::UpdateFire` (`0x007B79C0`) calls `Fire`, which makes one round, `ShotsPerSalvo`
times for each shot of a salvo, and starts the next shot when `SalvoDelay` has run out. So
a shotgun with eight pellets, or a salvo with no delay, signals several times in the same
frame. Once kicks added up (2026-10-01), each signal stacked another kick, eight at most:
a shotgun kicked eight times over. `FireShake` now plays once per weapon per frame
(`ShotGate`, 2026-10-02); a salvo with a delay still kicks for each shot.

Third person is `Tracker::IsFirstPersonView` on the Trackable's `mTracker` being false:
the same test `SetupCamera` makes, which honours a class's `ForceMode`. Any other soldier
state (a knockdown, a death) ends a jump without a landing. In first person, cockpits
included, every shake keeps its turn and drops its move (`turn_only`; the user's call,
2026-10-01): the camera is at the eye there, and the arms and cockpit drawn from
`mMatrix` would move with it. A scoped zoom also puts the soldier's camera at the eye
(`EntitySoldier::SetupCameraTrackMatrix`: aiming with a `Weapon::ZoomFirstPerson`
weapon) without `IsFirstPersonView`, so a push still plays there.

**One-off shakes.**
- **Envelope:** a one-off shake rises to its peak over `Rise` of its `Length` and eases
  back, a smoothstep each way.
- **Rate 0:** it is one push, drawn afresh each time from the class's ranges.
- **Rate above 0:** each angle swings on its own noise under the same envelope. The push
  does not (`PushOnce`, on by default): swinging, it pumped the camera in and out, which
  the user saw as bouncing (2026-10-01). `PushOnce = 0` brings the swing back.
- **Random numbers:** the DLL's own xorshift generator, since drawing from `PblRandom`
  would change the simulation's sequence.
- **Limit, adding:** each repeat is a kick of its own and the ones still running add
  together, up to `Limit` of them (a whole number, at most 8), as the user's reference
  model sums its fire kicks (2026-10-01, the user's call: overlapping shots had been
  losing their punch). Its envelope, defaults, per-shot draw and advance-then-sample
  order were already the same, and the tests check its sheet's per-frame values at 30 and
  60 frames a second. `FireShake` defaults to 8, as the model does; every other one-off
  defaults to 1.
- **Making room without a jump:** when `Limit` kicks are running, a new one drops the
  oldest but takes over its current value and fades it out over its own rise, so the
  view never jumps. The model drops the oldest outright; an earlier pool here did too,
  and with long kicks that showed as a hitch. At `Limit` 1 this is a restart: a repeat
  begins from wherever the view stands. A carry rule (a repeat taking `1 − 1/Limit` of
  where the view stood into its peak) stood in between, and is gone.

**Held shakes.**
- **Soldier sprint:** the spec's railed judder. Each signal is driven six times past its
  limits and clipped. Pitch follows noise at `Rate`; yaw follows the stride,
  `sin(4π·phase)·|sin(4π·phase)|`, with a stride of 2.8 / `Rate` seconds. It ramps in
  and out in straight lines, a third of a second each way by default.
- **Flyer boost and turn:** the reference model's turbulence, one sine per axis: pitch at
  `Rate`, yaw at 1.3 times it and roll at 0.7 times (11, 14.3 and 7.7 Hz at the default
  11), phases 0, 2.1 and 4.2, and push on a fourth sine at 1.1 times. By default they
  ease toward their target at 6 per second each way, as the model does.
- **Flyer brake:** a smooth sway on noise.
- **Fades:** every held shake but the blast follows its target with a fade from its own
  `Length` and `Rise` (`held_fade`): `Rise x Length` seconds to fade in, the rest to fade
  out, as a one-off splits its length. Boost, turn and brake ease exponentially, reaching
  95% in that time (three time constants); the sprint ramps linearly, all the way in that
  time. The defaults (1 and 0.5; the brake 1 and 0.25; the sprint 0.667 and 0.5) are the
  fixed fades these shakes had before (2026-10-01). The `Threshold` sets the target; the
  fade only sets how quickly the shake follows it.
- **Push, while held:** with `PushOnce` (every default but the blast's) the push is a
  steady move back that comes and goes with the shake's level; the blast's position keeps
  swaying.

**The reticule** stays on the unshaken view. Right after `SetupCamera` sets the camera
from the unshaken matrix, the detour keeps a copy of the camera's `_MatrixInverse`, and
another once it has set the shaken one. A detour on `ReticuleDisplay::Update` puts the
unshaken inverse back for that call only, then restores the shaken one. It only does this
when the camera still holds this frame's shaken inverse, so any other writer is left
alone. The reticule then holds still while the picture shakes under it, and its aim is
untouched. The widescreen fix detours the same function first, so the guard accepts its
`JMP` and Detours chains onto it. On modtools the aim point is projected through a helper
(`0x00678520`, through the thunk `0x00413B24`); Steam and GOG inline it (`LEA EAX,[ESI+0x70]`
at `0x00630883` and `0x00631923`).

**Hits** come from health rather than the damage code, so they work on a multiplayer
client, where the host does the damage.
- **Threshold:** a drop of at least 1 point or 0.5% of max health, whichever is more,
  shakes at once.
- **Cooldown:** drops in the 0.15 s after gather and shake when it ends.
- **Leak:** what has gathered leaks away at the threshold every 0.5 s, so a hero's drain
  never adds up to a shake.
- **Size:** full at a tenth of max health (`GameObject +0x148`), down to a quarter for
  the smallest hit that counts.

The offsets are the ones `controller_rumble.cpp` and `aim_assist.cpp` already read on
every build: a `GameObject` has its `Damageable` part at `+0x140` (Phantom PDB), with
`mCurHealth` and `mMaxHealth` after the vptr.

**ODF properties.**
- **Names:** twenty-one shakes, each with a scale (`FireShake`) and twelve details
  (`FireShakePitch`, `Yaw`, `Roll`, `Push`, `Length`, `Rise`, `Rate`, `Limit`,
  `Threshold`, `Steady`, `PushOnce`, `Teammates`). The 273 names are hashed at compile
  time and checked there for collisions. No stock property shares one; the nearest, the flyer class's unused
  `CrashShakeStart`, `CrashShakeEnd` and `CrashShakeLength` fields, are left alone by
  not naming a shake `Crash`.
- **Reading:** through `entity/odf_gameext_props.cpp`, whose reader hooks already see
  every property of every class.
- **Inheritance:** a second patch there, on the eight-byte `child = parent->Derive(hash)`
  call in `EntityClass::Read` and `WeaponClass::Read`, reports each new class with the one
  it was copied from. The values live in a table keyed by class and are copied from parent
  to child at that moment, so `ClassParent` works exactly as it does for stock fields.
- **Defaults:** a class's values override the defaults one field at a time, and setting
  any of them turns a shake on.

| | modtools | Steam | GOG |
|---|---|---|---|
| `ChaseCamera::SetupCamera` (detoured) | `0x004A2440` | `0x00453D00` | `0x00453CE0` |
| `ChaseCamera::Update` (queue fields read) | `0x004A2D70` | `0x00453820` | `0x00453800` |
| `ReticuleDisplay::Update` (detoured; `RET 4`) | `0x00683270` | `0x00630650` | `0x006316F0` |
| `RedCamera::SetMatrix` | `0x007FEED0` | `0x006CBEF0` | `0x006CCF90` |
| RedCamera projection (`_fZoom` read) | `0x007FEE50` | `0x006CBCD0` | `0x006CCD70` |
| `Tracker::IsFirstPersonView` | `0x0049FDD0` | `0x0044E3C0` | `0x0044E3A0` |
| `EntityFlyer::DoTrick` (detoured) | `0x004F3D10` | `0x004B18F0` | `0x004B18F0` |
| `EntityClass::Read` Derive site | `0x004D0992` | `0x00491DE0` | `0x00491DE0` |
| `WeaponClass::Read` Derive site | `0x0061E55C` | `0x0067A37D` | `0x0067B41D` |
| `rttiHashEntityFlyer` initialiser | `0x00A168C0` | `0x00402AD0` | `0x00402AD0` |
| `CameraManager::ApplyShake` (called; `RET 8`) | `0x004A0690` | not read | not read |
| its CALL in `EntityFlyer::PostCollisionUpdate` (retargeted) | `0x004F7F3A` | not read | not read |
| its CALL in `EntityFlyer::CollisionCallback` (retargeted) | `0x00503230` | not read | not read |
| `mControlMove` load in `EntityFlyer::Update` | `0x004FD376` | not read | not read |

Where the fields live:
- **Same on every build:** the chase camera's fields and its queue
  (`core/layout/chase_camera.hpp`), and the render camera's zoom
  (`core/layout/red_camera.hpp`).
- **Per build:** the flyer's fields (`core/layout/flyer.hpp`), pinned at `DoTrick` and
  `RecalculateSpeed` (modtools `0x004F2F80`, Steam and GOG `0x004ABC70`). The ones added
  for the flyer rework (`mGetSpeedSpeed`, the forward axis, the class's speeds and turn
  rates, `mControlMove`, `mInLandingRegionFactor`) are read
  on modtools only so far, and are 0 on Steam and GOG.
- **`ChaseCamera::Update`:** the vtable slot before `SetupCamera` on each build; modtools
  reaches both through thunks.
- **RTTI hashes:** `PblHash` of the class name, which is how both initialisers make them.

`tests/camera_shake_abi_tests.py` checks every guard, field and site, and the vtable
link, on all three executables; `tests/camera_shake_tests.cpp` checks the maths.

### Flyers

Read on Phantom, then modtools. The user's reference model (their Unreal flight project)
supplied the feel: a turbulence of three sines near 11 Hz, a boost that punches while it
spools up and then holds a quarter, a turn shake after 2 to 3 s of turning, and a bump
that jolts by its impact. Its inputs were Unreal's flight model, so they were replaced
with BF2's own:

**Speeds.** Every flyer class has its own speeds, and a fixed reference (the model's
80 m/s) fits none of them. `EntityFlyerClass_data` (class `+0x728`) has `mMinFlyerSpeed`,
`mMidFlyerSpeed`, `mMaxFlyerSpeed` and `mBoostFlyerSpeed` together at data `+0x164`, so
class `+0x88C` to `+0x898` on Phantom and modtools, from the ODF's `MinSpeed` to
`BoostSpeed`. The stock ODFs:

| Flyers | MinSpeed | MidSpeed | MaxSpeed | BoostSpeed |
|---|---|---|---|---|
| starfighters | 35 (Jedi fighter 70) | 60 to 90 | 85 to 110 | 130 to 200 |
| snowspeeder | 15 to 20 | 35 to 40 | 55 to 72 | 100 |
| LAAT (`rep_fly_gunship`) | 30 | 45 | 60 | none |
| ride-along gunships | -15 | 0 | 45 | none |

- `GetFlyerMinSpeed`, `GetFlyerMidSpeed` and `GetFlyerMaxSpeed` (Phantom `0x005266E0`,
  `0x00526690`, `0x00526640`; modtools `0x004F0A10`, `0x004F09B0`, `0x004F0950`) return
  the class values, capped by the `land_speed_*` globals while `mInLandingRegionFactor`
  (modtools `+0x5FC`) is set. GameExt reads the class values as the ODF gave them.
- `RecalculateSpeed` keeps `mGetSpeedSpeed` (data `+0x80`, modtools `+0x5F8`): the
  velocity along the flyer's nose, or on a path, its speed target. The stock motion
  blur (`SetupCameraTrackMatrix`, Phantom `0x0052CD70`, single player only) scales from
  the class's `BlurStartV` to max(`MaxSpeed`, `BoostSpeed`) on it, times `BlurEffect`;
  `SetupCameraFOV` (`0x0052CA00`) reads the same top speed. `BoostShake` measures the same
  speed, so its `Threshold` can name the class's speeds.

**Turning.** BF2 has its own hard-turn count, `Controllable::mTurnBuildup` and
`mPitchBuildup` (Phantom `Controllable_data +0xB8`/`+0xBC`; modtools Controllable
`+0xF4`/`+0xF8`, and the flyer's Controllable part is at `+0x240`, so flyer
`+0x334`/`+0x338`).
- `PlayerController::Update` (Phantom `0x0071CAA0`; modtools `0x0059B88D`) adds the frame
  time to both while the turn and pitch inputs' squared length is at least 0.9, caps them
  at 1, and zeroes both when it is not.
- `EntityFlyer::Update` (Phantom `0x0052EB00`; modtools from `0x004FD86D`) turns each
  past 0.5 into a rate multiplier, `1 + 2 (TurnBuildupMultiplier - 1)(buildup - 0.5)`,
  full at 1. Most stock flyers leave the multipliers at 1; the Jedi fighter (1.5/1.25)
  and the snowspeeder (1.25) tighten.
- For a class with a `BoostSpeed`, above `MaxSpeed` it zeroes both buildups every frame
  and scales turning down toward 0.7 (0.5 for a `CommandFlyer`) as the speed reaches
  `BoostSpeed`. So in BF2 a boost and a built-up turn never happen together, which is why
  `TurnShake` is its own shake rather than, as in the reference, part of the boost.
- **The mouse never builds it**, confirmed in play (2026-09-30): turning with the mouse
  gave no `TurnShake` while it read this count. `PlayerController::Update` also keeps a
  separate `mMouseTurnBuildup`, which is the mouse's own path.
- So GameExt measures the turn on the flyer instead (`nose_turn_rate`): the angle between
  its forward axis (the matrix row at flyer `+0x110` on modtools, the one
  `RecalculateSpeed` takes `mVelocity` along, `0x004F3050` on) a frame apart, over the
  frame time, smoothed over about a twentieth of a second. It turns hard at 60% or more of
  the faster of the class's `PitchRate` and `TurnRate` (class `+0x8A0`/`+0x8A4`, read by
  `Update` at `0x004FD752`), which `Update` blends between by how far each way the stick
  is pushed, so about what full stick gives; a class with neither counts from 45°/s.
  Tricks (flag bits `0x03`) do not count. `TurnCount` adds up the seconds, holding
  through a lull of up to 0.25 s, such as a mouse between two pushes, so a `Threshold` is
  still in seconds (default 2 to 3 s, the reference's).

**Speeding up and slowing down.** `EntityFlyer::Update` steers the speed toward a
target and never faster than a fixed rate, so measuring the speed's change says little:
- **Landing regions:** while `mInLandingRegionFactor` (data `+0x84`, modtools flyer
  `+0x5FC`) is non-zero, `GetFlyerMaxSpeed`, `GetFlyerMidSpeed` and `GetFlyerMinSpeed`
  cap the class's speeds with four fixed globals, `land_speed_max` 60, `land_speed_mid`
  20, `land_speed_min` 10 and `land_speed_min_mult` 0.2 (Phantom `0x00A8E8E0` on,
  modtools `0x00ACDC60` on; the getters are their only readers): max at most 60, mid at
  most 20, min a fifth of itself, at most 10. `BoostSpeed` is read raw. The throttle
  target below goes through the getters, so it is capped too, and GameExt applies the
  same caps (`landing_speeds`); a `Threshold`'s speed names stay the ODF's numbers.
- **Target:** `BoostSpeed` while boosting with one (modtools `0x004FECF9`, into `mSetSpeed`
  at data `+0x20`, flyer `+0x598`); otherwise from `mControlMove`, the forward and back
  input at Controllable_data `+0x44` (modtools Controllable `+0x80`, flyer `+0x2C0`,
  loaded at `0x004FD376`): `MidSpeed + (MaxSpeed - MidSpeed) x move` forward,
  `MidSpeed + (MidSpeed - MinSpeed) x move` back (`0x004FED19` on).
- **Rate:** `mSetSpeed` moves from `mGetSpeedSpeed` toward the target by at most
  `Acceleration x dt` (Phantom `0x00530E38`; the ODF takes both `Acceleration` and the
  stock misspelling `Acceleraton`, `EntityFlyerClass::SetProperty` `0x0052A3A7` and
  `0x0052B441`, default 5), or `BoostAcceleration x dt` while boosting or above
  `MaxSpeed` (default 0, so a class without it cannot change speed while it boosts).
  A flyer therefore speeds up and slows down at one fixed rate, and a threshold in
  m/s² could only say whether it was braking, not how hard: the first `BrakeShake`
  design, measured that way, was dropped for a speed band.
- **Steering:** `mGetSpeedSpeed` is the velocity along the nose, which lags the nose
  through `MomentumFilter`, so every turn dips it a metre or two a second and BF2 speeds
  it back up. Measured, that is speeding up. GameExt compares the speed with the target
  instead: speeding up while more than a twentieth of the class's speed range below
  it, slowing down while that far above (`heading_margin`), and uses the measured
  change only where `mControlMove` is not read yet.
- **Rolling:** BF2 reads the throttle and the roll as one stick. `PlayerController::Update`
  (Phantom `0x0071CAA0`) scales `mControlMove` and `mControlStrafe` down together when
  their length passes 1, so full throttle held through a roll (not a trick) reaches the
  flyer as 0.71. The flyer steers toward that lower speed until the roll ends, then
  speeds back up, in stock too; that replayed the boost shake in play (2026-10-02).
  `mControlStrafe` is the input after `mControlMove` (Controllable_data `+0x48`; modtools
  Controllable `+0x84`, flyer `+0x2C4`), read next in `Update` with a 0.3 dead zone
  (`0x004FD39D`). GameExt takes the throttle as held instead (`throttle_intent`: a pair
  on the rim goes back out to the edge of the square), and a speed-up starts only when
  the speed that asks for rises by more than the margin since the last one arrived
  (`SpeedUp`); arriving is still judged against BF2's own target. Regaining speed after
  a roll or turn does not count, and neither does the climb to cruise after take-off.
  It needs `mControlMove` and `mControlStrafe`, which retail does not read yet; there
  the shake still measures the speed change. `[Fixes] FlyerRollThrottleFix` (on by
  default, all three builds; `entity/flyer_roll_throttle_fix.hpp`) now skips the cap
  for a flyer, so a roll no longer costs speed at all and retail loses the replay too;
  `throttle_intent` still covers the fix being off on modtools.
- **Braking** is slowing down with `mControlMove` at -0.1 or below: the brake or reverse
  input. Letting go of the throttle at `MaxSpeed` or after a boost also slows the flyer,
  back to `MidSpeed`, but with the input at 0, so it is not braking.
- `EntityFlyer_data` also has `mTurbulance` (data `+0x1750` on Phantom), which
  `Update` reads during tricks; not followed up.

**Tricks.** `DoTrick(Trick)` (Phantom `0x00525990`): `FLIP_UP`, `FLIP_DOWN` and the four
diagonal flips call `FlipAdd` (`0x00525D90`), which sets flag bit `0x02` (modtools
`OR byte [ECX+0x5F4],2` at `0x004F3AE2`); the diagonals also `RollAdd`. `SIDE_ROLL` calls
`SideRoll` (`0x0052DF30`), which only `RollAdd`s. A class with no
`EnergyTrickDrainDoubleTap` turns every trick into `SIDE_ROLL`. The detour reads the flag
byte after the call: `0x02` is `TrickFlipShake`, anything else `TrickRollShake`.

**Bumps.** `PostCollisionUpdate` and `CollisionCallback` (Phantom `0x005274B0`,
`0x00524340`; modtools `0x004F79B0`, `0x005025B0`) work out an impact from the flyer's
speed just after the bump, `|mVelocity| / 40 + 0.01` capped at 1, and when the flyer is
the one `GetChaseCameraTarget(0)` follows, call `ApplyShake(impact x 0.8, impact x 0.7)`;
above 0.5 they also set the chase camera's `mTimer` to 0.1. The two CALLs go through the
thunk `0x004162D4` to `0x004A0690` on modtools, which is `thiscall(amount, duration)`,
`RET 8`, and appends to the four-slot queue. GameExt retargets both CALLs to
`bump_apply_shake`: when the viewed flyer's class sets `CollisionShake` it keeps the bump
for the next camera frame and never queues it; otherwise it calls `ApplyShake` as
before. The impact is judged by the change in `mVelocity` since the last camera frame,
since BF2's own figure rates a head-on stop, which leaves little speed, as almost
nothing, and stops at 40 m/s. The bump's own damage is kept from shaking again as a hit
for 0.25 s.

**Dead ends.**
- `EntityFlyerClass_data` has `mCrashShakeStart`, `mCrashShakeEnd` and
  `mCrashShakeLength` (data `+0x1EC` to `+0x1F4`). The constructor sets 1, 0.5 and 2 and
  nothing ever reads them: a cut shake, probably for the `CRASHING` death spiral (`mState`
  4, spun by `mMinCrashRollRate`/`mMaxCrashRollRate`). No ODF under the mod tools sets
  them.
- `mNoiseFactor` feeds only `EntityFlyer::GetSoundRadius`; BF2 flight has no turbulence
  of its own.

### Melee

Built 2026-10-02 on all three builds, not yet played. The user's names: `SwingShake`,
`StrikeShake`, `SwingBlockedShake`, `BlockShake`, `DeflectShake`, all on the melee
weapon's ODF, plus `StrikeShakeTeammates` (on unless `0`).

**Swing.** Every combo state with an attack enters FIRE and calls the base
`Weapon::SignalFire` (`WeaponMelee::EnterState` Phantom `0x007C3A10`, `EnterFire`
`0x007C3940`), which camera shake already detours. A weapon whose `IsMelee` (Weapon
vtable `+0x54`, `thiscall()`, bool in AL) answers true plays `SwingShake` there instead of
`FireShake`. Only `WeaponMelee` answers true (modtools `0x00633B70`, Steam `0x00687C40`,
GOG `0x00688CB0`, all `MOV AL,1 / RET`), which is how its vtable was found on Steam
(`0x007B1578`) and GOG (`0x007B24F0`).

**Strike.** `WeaponMelee::UpdateFire` (Phantom `0x007C9F60`; modtools `0x00639020`, Steam
`0x0068C230`, GOG `0x0068D2C0`; vtable `+0xA4`; `thiscall(float dt)`, `RET 4`) runs the
attacks of the current combo state. Each attack has a `DamageData` on `m_pDamageData`
(modtools `+0x1D8`, retail `+0x1A8`; `iNumObjects +0x08`, `apObject[8] +0x0C`, `pNext
+0x2C`; a new attack goes on the front). For each object the blade's rays reach, not yet on
the attack's list, it:
1. lists the object (eight at most);
2. calls its `Deflect` (vtable `+0xD4`) with no ordnance; true means blocked, and it moves
   on;
3. otherwise applies the damage (`Damageable +0x140`, vtable `+0x14`), any push and a hit
   effect, and sets a "struck" flag. At the end, if the flag is set, it plays the class's
   `m_HitSound` (`+0x328`) once.

GameExt detours `UpdateFire` for the viewed unit's weapon. Before the call it tallies each
attack's count; after it, the objects listed during the call, less the ones whose block
was seen during it, landed. `StrikeShake` plays once if any did. With
`StrikeShakeTeammates = 0`, objects on the viewed unit's own team (`GameObject +0x234`,
4-bit signed, above 0) are left out. A blocked object is matched by pointer: the struck
object and the blocking weapon's owner (`Controllable +0x18` Trackable, `GetGameObject`)
are the same `GameObject`, the one `UpdateFire` calls `+0xD4` and `+0x140` on.

**Block, deflect, blocked.** `EntitySoldier::Deflect` (vtable `+0xD4`, modtools
`0x00528C90`) hands the call to the held weapon's `Deflect` (Weapon vtable `+0x48`).
`WeaponMelee::Deflect` (Phantom `0x007C23F0`; modtools `0x00637670`, Steam `0x0068A550`,
GOG `0x0068B5E0`; `thiscall(Ordnance*, const PblVector3*, const PblVector3*)`, `RET 0xC`,
bool in AL) blocks only when:
- the combo state has a `Deflect`;
- `Combo::Deflect::TestDeflect` passes (the `DeflectAngle` window);
- the owner has the energy.

When it blocks, it plays the deflect animation, `m_DeflectSound` (`+0x340`) and effect
(`+0x33C`), and reflects an ordnance. Its first argument tells a bolt or beam (set) from a
melee strike (null, from `UpdateFire`). GameExt detours it:
- **The viewed unit blocking:** `DeflectShake` for a bolt or beam, `BlockShake` for a
  strike.
- **Inside the viewed unit's `UpdateFire`:** a block with no ordnance is that swing being
  blocked, so `SwingBlockedShake`, and the blocker is kept out of the strike.

Vanilla saber-against-saber blocking passes `Deflect` its two vectors swapped. GameExt's
`Lightsaber Block Direction Fix` patch set rewrites the two displacements inside
`UpdateFire` (`core/patch_table.cpp`); it does not touch the prologue the detour uses.

### Walkers

Walkers (`EntityWalker`, and `CommandWalker`, whose `IsRtti` answers `EntityWalker` too;
`EntityDroideka`'s does not) need no hooks: every shake reads what BF2 has already
recorded, once a frame, through `layout::Walker`. All three builds are read; Steam and GOG
share the walker code at the same addresses. Field offsets and the sites they were read at
are in `core/layout/walker.hpp`; the ABI audit checks them.

- **Steps.** `EntityWalker::DoFootImpactEffects` (Phantom `0x00595470`) walks the feet
  (`mNumFeet`, class data `+0x676`). When one lands it sets the controllers' stomp rumble
  (`StompRumbleLight`/`Heavy` and durations), plays the FoleyFX and `FootstepSound`, attaches
  `StompEffect`, and sets that foot's bit in `mFootState`; a foot that did not land has its
  bit cleared. The bit is on for exactly the update the foot lands in, so a bit coming on is
  a step. `StepShake` plays once for all the feet that came on together.
- **When a foot lands.** Each update takes the foot's height over the walker's origin
  (world y less the walker's) and compares it with the height it had last update
  (`mLastFootHeight`) and the lowest it has ever been (`mMinFootHeight`, never reset):
  - `StompDetectionType` 0, the default: the update the foot, coming down, passes below
    its lowest plus `StompThreshold` (0.15 unless the ODF sets it). This does not depend on
    the frame rate, but since the lowest is never reset, a foot that once sat low (on a
    slope or in a dip) can stop landing on flat ground.
  - `StompDetectionType` 1: an armed foot lands the first update it drops less than 0.1,
    and landing disarms it (`mFootState` bit 24 + its number). A disarmed foot is re-armed
    only by a drop of more than 0.1 in a single update. So a foot lands only if it comes
    down faster than 0.1 per update: 3 m/s at 30 updates a second, 6 at 60, 14.4 at 144.
    The faster the frame rate, the fewer feet land.
  - `[Diagnostic] WalkerFootDiag` (`entity/walker_foot_diag.cpp`) detours the function
    (modtools `0x00555B60`, Steam and GOG `0x00500710`; its one caller is `UpdateState`,
    once per update) and logs, for the walker the player drives, each foot's descents with
    their biggest single-update drop and whether they landed.
  - Measured with it on modtools at about 60 fps (2026-10-03), the user's AT-TE (type 1)
    brings its feet down at 4 to 5.5 m/s when walking, 0.06 to 0.09 an update, and most of
    those steps never landed; boosting, at 7 to 12 m/s, every step landed.
  - `[Fixes] WalkerStompFix` (`entity/walker_stomp_fix.cpp`) makes type 1's test a speed:
    BF2's 0.1 at 30 updates a second, 3 m/s, at any frame rate. `EntityWalker::UpdateState`
    (modtools `0x0055B3C0`, Steam and GOG `0x00502890`; thiscall, RET 0x14, through the
    walker vtables) takes the update's length as its first argument, the one it adds to
    `m_fGroundedTimer`. Its detour sets GameExt's value to 3 x that length before the call,
    and type 1's reads of the 0.1 (modtools' one FCOMP at `0x00555D0F`; retail's COMISS at
    `0x00500888` and MOVSS at `0x0050089D`) are pointed at it. The constant stays: retail
    `UpdateState` compares another field with it (`0x005028DA`). The landed bits feed only
    the stomp presentation (`DoFootImpactEffects` sets them, `UpdateState` zeroes them on a
    state change, and a modtools debug overlay at `0x00755E30` prints them), so the fix is
    local to each machine.
- **Foot order.** `EntityWalkerClass::SetProperty` appends each `TerrainLeft` (hash
  `0x21322ADB`) and `TerrainRight` (`0xB62DEB24`) body to `mFootPrimitives` as it reads it,
  so the feet run in ODF order: left, right for each leg pair in every stock walker. Even
  feet are left and odd feet right, which is all `StepShake`'s roll uses.
- **A name the model lacks loses that foot.** That append (Phantom `0x00599D10`, modtools
  `0x0055CEB9` and `0x0055DF9C`) looks the name up with `CollisionModel::GetMaskId`, the
  index of the collision body whose name hash matches, and skips the foot, without a
  warning, when there is none. The class's bodies are the model's collision primitives,
  added on `GeometryName` (`EntityGeometryClass::SetProperty`, Phantom `0x00538E80`) from
  the `prim` chunk's `NAME` strings, so `TerrainLeft` must spell the primitive exactly as
  the munged model does. The user's AT-TE named them `p_-tbv_sphere_foot1` where the
  model has `p_-tbv-sphere_foot1`, and so had no feet at all: `mNumFeet` (class data
  `+0x676`) 0, and no stomp, footstep sound or step from any foot.
- **Jumps and landings.** `EntityWalker::Jump` (Phantom `0x00596660`; `JumpHeight` sets
  `mJumpVerticalSpeed` = √(19.6 x `JumpHeight`)) adds the vertical speed, zeroes
  `m_fGroundedTimer`, clears flag `0x40` and sets `0x80`. `CollisionCallback` sets `0x40` on
  any ground contact (normal y above 0.707); `UpdateState` then zeroes `m_fGroundedTimer` and
  clears `0x80`, and otherwise adds the frame time to it. So `0x80` coming on is a jump, and
  the timer dropping back after half a second or more is a landing from a jump or a fall.
  Half a second, not the soldiers' quarter, to leave room for a walker in stride: `Jump`
  itself still counts a walker 0.3 s off the ground as grounded. The fall speed is the
  fastest downward `mVelocity` while the timer ran, since the ground contact zeroes it.
- **States.** `mState` runs `EntityWalker::sStateTable` (Phantom `0x00A8F0B0`): 11 states of
  7 inputs. `CheckTurn` (`0x005942F0`) feeds inputs 4 and 5 when the aim is past
  `TurnThreshold`, and from standing (0) those lead to 1 and 2, the turn-in-place states;
  `TurnShake` holds while in them. 3 is dying (input 6 from anywhere) and 4 dead; the walker
  shakes stop there. 5 to 10 are the walk states.
- **Boost.** `UpdateState` boosts a walker whose `BoostSpeed` is above its `MaxSpeed`,
  walking forward with the sprint input and the energy for `EnergyBoostDrain`, and sets
  `mBoost` bit 0 while it does. `BoostShake` reads that bit and grades the ground speed in
  its `Threshold`, by default `MaxSpeed` to `BoostSpeed`.

## Open

- Confirmed in play on modtools and Steam (2026-09-27): every shake, the limit and the
  still reticule. GOG has the same addresses verified but has not been played. The soldier
  shakes are final (the user, 2026-09-30); the blast's size per unit of `Shake` can still
  be tuned.
- The flyer rework (2026-09-30) is built and played on modtools only. For Steam
  and GOG it needs `mGetSpeedSpeed`, the forward axis, the class's speeds and turn
  rates, `mControlMove`, `mInLandingRegionFactor` and the two collision CALLs read there; retail `ApplyShake` (Steam `0x0044F4C0`, GOG `0x0044F4A0`)
  takes its amount and duration in XMM1 and XMM2 with a plain `RET`, so it needs a naked
  stand-in, and the two CALLs are at `0x004B24F2` and `0x004B4D2B` on both (not yet
  audited). Until then, on retail, `TurnShake`, `CollisionShake` and speed names in a
  `Threshold` do nothing (numbers still work), and the install log says so.
- Easing into `BoostShakeSteady` at top speed looked choppy in play, as if the ship
  bounced (2026-10-01, with a 2 m push). The cause was the push piling up in BF2's
  camera ([The camera eases from last frame's view](#the-camera-eases-from-last-frames-view)).
  No bounce has been reported since the hand-back. The surge replaying after a roll
  is settled by `SpeedUp` (see Rolling, under Flyers), built 2026-10-02 and not yet
  played.
- With `StompDetectionType = "1"`, the AT-TE landed only its middle right foot in play
  (2026-10-02), so `StepShake` played for that foot alone. Its Terrain names did not match
  its model (see Walkers), which may be why; the old model is gone, so it cannot be
  checked. Once the names matched, `WalkerFootDiag` measured the single-update rule
  costing most walking steps at 60 fps (Walkers). `WalkerStompFix`, built 2026-10-03, is
  confirmed in play the same day: the AT-TE lands its walking steps. A per-class speed is
  on the ROADMAP (Vehicles).
- BF2's own follow still depends a little on the frame rate (`v / T - v * dt / 2`
  behind, and a lurch on a slow frame). Following exactly at any frame rate would mean
  replacing its easing, per axis, with the exact step for a moving target: keep
  `e^(-T * dt)` of the old gap plus `(1 - e^(-T * dt)) / (T * dt)` of the frame's move.
  That would change the stock camera for every unit; not built.
- The walker shakes (2026-10-02) are built on all three builds and not played. The
  half-second landing gate assumes a walker in stride stays below it; the walker state
  numbers were read off Phantom's table only.
- The melee shakes (2026-10-02) are built on all three builds and not played. Whether a
  multiplayer client runs `UpdateFire` and `Deflect` for its own swings and blocks is
  not known yet. Per-attack shakes would need the combo file: unknown names there are
  safe (`configmunge` has no schema; `Combo::ReadConfig`, `State` and `Attack` log
  "unexpected data" and skip them), but BF2 packs each attack into bits, so GameExt
  would keep its own table.
- `TurnShake`'s hard-turn share (60% of the class's faster rate) is read, not played:
  whether full stick and a brisk mouse both clear it wants checking in play.
- Landing regions: easing into one slows the flyer toward the capped cruise speed, which
  `BrakeShake` does not count (the brake is not held). Whether that approach wants a shake
  of its own is to be decided on feel.
- The flyer defaults are first guesses, small for take-off and landing, to be tuned in
  play. The unused `CrashShake*` fields suggest a shake while a destroyed flyer spins
  down; not built.
- The spec's roll and jump sweep is not built yet: the camera trailing behind and looking
  toward the feet through rolls and jumps. `RollShake` is a plain shake until then.
- The blast's shape is set by the unit being viewed, not by the explosion, because the
  queue does not record where a shake came from. Shaping it per explosion would need two
  pieces:
  - a hook on `CameraManager::ApplyShake` (Phantom `0x00499B50`, modtools `0x004A0690`,
    thiscall(amount, duration), RET 8), with the explosion class in hand. Its callers are
    `ExplosionClass::CreateExplosion`, `EntityWalker::Render` and
    `EntityFlyer::CollisionCallback` / `PostCollisionUpdate` (the last two now
    retargeted on modtools for `CollisionShake`);
  - an `ExplosionClass::Read` Derive site, for inheritance.
- Damage that only reaches shields does not shake: `mCurShield` is not read.
- A passenger in a flyer's turret seat is following the turret, not the flyer, so gets
  none of the flyer's own shakes.
- The reticule holds still by design, so a big shake leaves it off the true aim point in
  the shaken picture by the shake's angle. Publishing the shake's screen offset as a HUD
  event (ROADMAP's HUD shake) would let a `.hud` choose to follow it.
- Lua cannot set these properties. `Lua_Callbacks::SetClassProperty` (Phantom `0x0064E9A0`)
  finds an `EntityClass` by name (`_GetEntityClass<EntityClass>`), turns a boolean value
  into `"1"`/`"0"`, hashes the property name and calls the class's own `SetProperty`
  through its vtable. The shake properties are taken at the ODF reader instead, so that
  `SetProperty` ignores them, and weapon classes are not found by that lookup at all.
  Hooking the soldier and flyer classes' `SetProperty` for these names would cover
  soldiers and flyers; weapons need a lookup of their own. On ROADMAP.
