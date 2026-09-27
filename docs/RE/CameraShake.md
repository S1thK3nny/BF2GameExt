# Camera shake - RE notes

How the stock camera shake works, why it feels harsh, why it never moves the aim, and
how GameExt redraws it as a blast and adds shake for firing, hits, landings, rolls,
sprinting and flyer boost and braking (`render/camera_shake.cpp`, built 2026-09-27 for
modtools, Steam and GOG; the shapes follow a BFIII-derived camera spec). Read on the Phantom build, which has symbols,
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

1. The owner's `SetupInterpolatedCameraTrackMatrix` (Trackable vtable `+0x08`) writes
   the camera matrix into `mMatrix`.
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
`FLT_MAX` for the call when `[CameraShake] Smooth` is on, so the stock turn is skipped,
then moves and turns `mMatrix` itself and calls `RedCamera::SetMatrix` again. The offset
is in the camera's own frame: pitch, yaw and roll, and a move along its right, up and
back axes, clamped at 0.35 radians and 0.5 m per axis. `Strength` multiplies all of it,
and each shake has its own strength as well. Everything is sampled by time, in double
precision so a long session does not coarsen the phase, so the frame rate does not
change the result. `mPreShakeMatrix` gets the unshaken matrix, as stock leaves it. Like
stock, the moved `mMatrix` stays in place for the rest of the frame, so the first-person
arms and cockpit follow the view.

**The blast.** With `Smooth` on, the stock queue is drawn as the spec's blast:
- **Motion:** roll and a small camera move on three sines at the spec's ratios (3.43,
  2.67 and 2.87 Hz at the default rate of 3), with no pitch or yaw.
- **Size:** the strongest queue entry (`mShakeCount` at `+0x9C`, `mShakeAmount[4]` at
  `+0xA0`), not `mShake`'s sum, so overlapping explosions never add up. The queue's own
  linear decay fades it.
- **Ceiling:** it levels off above about 2.5 units (`2.5 × tanh(x / 2.5)`), so a
  detpack's 2.0 is not four times a grenade's 0.5.
- **Zoom:** it is divided by `RedCamera::_fZoom` (`+0x140`), so it shrinks while zoomed
  in.
- **Defaults and override:** 6 degrees of roll and 0.03 m of movement per unit. The
  unit being viewed can reshape it with `BlastShake`, or turn it off.

**Shakes and triggers,** for the object the chase camera follows (Trackable
`GetGameObject`):

| Shake | Trigger | Kind |
|---|---|---|
| `FireShake` | `Weapon::SignalFire` detour, when the weapon's owner (`Weapon +0x6C`, its Trackable at `+0x18`) is that object; class from `Weapon +0x64`, else `+0x60` | one-off; restarts |
| `HitShake` | its health (`GameObject +0x144`) dropping, below | one-off; restarts |
| `LandShake` | soldier `mState` going from airborne (4 `JUMP`, 6 `JET_JUMP`, 7 `JET_HOVER`, 8 `FALL`) to grounded (0 `STAND`, 1 `CROUCH`, 2 `PRONE`, 3 `SPRINT`, 5 `ROLL`, 19 `SLIDE`) after at least 0.25 s; flyer `mState` going to 0 (`LANDED`) from 3 (`LANDING`) or 2 (`FLYING`) | one-off; restarts |
| `RollShake` | soldier `mState` turning to 5 (`ROLL`), third person only; `EntityFlyer::DoTrick` detour when it starts a trick | one-off; restarts |
| `SprintShake` | soldier `mState` 3 (`SPRINT`), third person only; flyer flag bit `0x04` while `mState` is 2 (`FLYING`) | held |
| `BrakeShake` | how fast the length of the flyer's `mVelocity` is falling, smoothed: 0 at 4 to 1 at 30 units/s² | held |
| `BlastShake` | the stock queue, while `Smooth` is on | held; on by default |

Third person is `Tracker::IsFirstPersonView` on the Trackable's `mTracker` being false:
the same test `SetupCamera` makes, which honours a class's `ForceMode`. Any other soldier
state (a knockdown, a death) ends a jump without a landing.

**One-off shakes.**
- **Envelope:** a one-off shake rises to its peak over `Rise` of its `Length` and eases
  back, a smoothstep each way.
- **Rate 0:** it is one push, drawn afresh each time from the class's ranges.
- **Rate above 0:** each axis swings on its own noise under the same envelope.
- **Random numbers:** the DLL's own xorshift generator, since drawing from `PblRandom`
  would change the simulation's sequence.
- **Restarts:** a repeat begins from wherever the channel stands and fades that out
  over the rise, so the view never jumps.
- **Limit:** a repeat also carries `1 − 1/Limit` of where the view stands into its own
  peak. Repeats faster than the rise then settle at `x = p + x(1 − 1/Limit)`, which is
  `Limit` times the peak, and slower ones settle below it. At `Limit` 1 (every one-off
  default) nothing is carried, which is BFIII's restart-per-shot for fire. The earlier
  design added up to eight kicks and dropped the oldest; with long kicks, that dropped a
  kick before it had faded and showed as a hitch.

**Held shakes.**
- **Soldier sprint:** the spec's railed judder. Each signal is driven six times past its
  limits and clipped. Pitch follows noise at `Rate`; yaw follows the stride,
  `sin(4π·phase)·|sin(4π·phase)|`, with a stride of 2.8 / `Rate` seconds. It eases in
  and out at 3 per second.
- **Flyer boost and brake:** smooth sways.

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
- **Names:** seven shakes, each with a scale (`FireShake`) and seven details
  (`FireShakePitch`, `Yaw`, `Roll`, `Push`, `Length`, `Rise`, `Rate`). The 56 names are
  hashed at compile time and checked there for collisions.
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

Where the fields live:
- **Same on every build:** the chase camera's fields and its queue
  (`core/layout/chase_camera.hpp`), and the render camera's zoom
  (`core/layout/red_camera.hpp`).
- **Per build:** the flyer's fields (`core/layout/flyer.hpp`), pinned at `DoTrick` and
  `RecalculateSpeed` (modtools `0x004F2F80`, Steam and GOG `0x004ABC70`).
- **`ChaseCamera::Update`:** the vtable slot before `SetupCamera` on each build; modtools
  reaches both through thunks.
- **RTTI hashes:** `PblHash` of the class name, which is how both initialisers make them.

`tests/camera_shake_abi_tests.py` checks every guard, field and site, and the vtable
link, on all three executables; `tests/camera_shake_tests.cpp` checks the maths.

## Open

- Confirmed in play on modtools and Steam (2026-09-27): every shake, the limit and the
  still reticule. GOG has the same addresses verified but has not been played. The soldier
  roll's default (back and down over a second) was the user's call; the flyer defaults and
  the blast's size per unit of `Shake` can still be tuned.
- The spec's roll and jump sweep is not built yet: the camera trailing behind and looking
  toward the feet through rolls and jumps. `RollShake` is a plain shake until then.
- The blast's shape is set by the unit being viewed, not by the explosion, because the
  queue does not record where a shake came from. Shaping it per explosion would need two
  pieces:
  - a hook on `CameraManager::ApplyShake` (Phantom `0x00499B50`, thiscall(amount,
    duration), RET 8), with the explosion class in hand. Its callers are
    `ExplosionClass::CreateExplosion`, `EntityWalker::Render` and
    `EntityFlyer::CollisionCallback` / `PostCollisionUpdate`;
  - an `ExplosionClass::Read` Derive site, for inheritance.

  The retail copies of `ApplyShake` are not found yet.
- Damage that only reaches shields does not shake: `mCurShield` is not read.
- A passenger in a flyer's turret seat is following the turret, not the flyer, so gets no
  boost, brake, trick or landing shake.
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
