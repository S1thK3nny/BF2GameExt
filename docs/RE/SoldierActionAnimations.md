# Soldier action animations

How a soldier's actions (jumps, rolls, landings, deaths) pick their animations,
how the stock directional jumps work, and the research behind
`UseDirectionalRolls` (2026-10-04). Names and logic are from the Phantom PDB
build in Ghidra; every address used by the DLL was then read on modtools, Steam
and GOG and is audited by `tests/directional_rolls_abi_tests.py`.

## Actions and the action table

`SoldierAnimator::SetAction(SoldierState, PblMatrix*, uint)` (thiscall, RET 0xC)
runs every frame. It first sets `mMovement` to the body's move since the last
call (`matrix.trans - mLegMatrix.trans`, zeroed past 10 m or when the frame time
is 0), copies the matrix into `mLegMatrix`, and when the state changed it picks
`mAction` and zeroes `mActionTime`:

- `ROLL` and `TUMBLE_RECOVER`: `DIVE`.
- `JUMP`: `JUMP_UP`, or with `m_bUseDirectionalJumps` set a directional jump
  (below).
- `FLY`: `FLY_FORWARD`/`BACKWARD`/`LEFT`/`RIGHT` by the larger of the move along
  the body's forward and right rows, always.

`ActionAnimation` has 39 values; their names are `s_aszActionAnimationName`
(Phantom `0x00A99BB0`): `sprint`, `jump`, `jump_forward`, `jump_backward`,
`jump_left`, `jump_right`, `landsoft`, `landhard`, `fall`, the `thrown_*` set,
`stand_getupfront`/`back`, `diveforward` (24), `jetpack_hover`, the prone
transitions, `stand_death_forward`/`backward`/`left`/`right` and
`stand_hitfront`/`back`/`left`/`right`. There is no left or right dive.

`SoldierAnimator::SetupPose(RedPose*)` (thiscall, RET 4) builds the pose each
frame the soldier is drawn. It switches on `mAction` and plays it through
`UpdateActionAnimation`, which fetches the animation from the class's action
table, then adds the frame time to `mActionTime` at its end: the first
`SetupPose` of an action sees `mActionTime == 0`. The table belongs to
`SoldierAnimatorClass::sInstance`; its getters are thiscall(action, map),
RET 8, and read `[class + (map * 0x97 + action) * 8 + 0x24]` for the upper body
and `+ 0x28` for the lower. `map` is the animator's `mWeaponAnimationMap`.

|                                   | modtools     | Steam        | GOG          |
|-----------------------------------|--------------|--------------|--------------|
| `SoldierAnimator::SetAction`      | `0x00575D50` | `0x0063ED60` | `0x0063FE00` |
| `SoldierAnimator::SetupPose`      | `0x0057C490` | `0x0063FAA0` | `0x00640B40` |
| `SoldierAnimator::UpdateActionAnimation` | `0x0057AFD0` | `0x00640860` | `0x00641900` |
| `GetUpperBodyActionAnimation`     | `0x0057DCA0` | `0x00643940` | `0x006449E0` |
| `GetLowerBodyActionAnimation`     | `0x0057DCC0` | `0x00643960` | `0x00644A00` |
| `SoldierAnimatorClass::FindAnimation` | `0x0057DE40` | `0x006442A0` | `0x00645340` |
| `SoldierAnimatorClass::sInstance` | `0x00B8D3C4` | `0x01EAFB1C` | `0x01EB0FD0` |
| `GameLoop::sClientDeltaTime`      | `0x00C6A9AC` | `0x01E56058` | `0x01E574F0` |

`UpdateActionAnimation` is plain thiscall on modtools, but on retail (LTCG) it
takes the frame time in XMM2 and the rate in XMM3, with the action and the leg
angle on the stack. `SetupPose` keeps the plain convention on every build: it is
called from `EntitySoldier::Render` and `SoldierElement::RenderUsingContext`,
and none of those callers keeps anything in a volatile register across it.

The `SoldierAnimator` members used here (`mLegMatrix` +0x00, `mOwner` +0x50,
`mSoldierAction` +0x70, `mWeaponAnimationMap` +0x74, `mMovement` +0xAC,
`mAction` +0x1FEC, `mActionTime` +0x1FF0) are at the same offsets on every
build. `mOwner` is the `EntitySoldier`'s start: `SetupPose` hands
`NetGame::GetJoystickIndex` its `+0x314`, which is `Controllable::mPlayerId`
(+0xD4) of the Controllable part at +0x240.

## Directional jumps and deaths

Both are soldier ODF switches, read by `EntitySoldierClass::SetProperty`
(Phantom `0x0057ABC0`) into the class's flag word (Phantom `+0x54`, the word
`NumTentacles` and `BonesPerTentacle` share):

| Property | Hash | Class flag | Immediate (modtools / Steam and GOG) |
|----------|------|------------|-------------------------------------|
| `UseDirectionalJumps`  | `0x3AF4F6FD` | bit 1 | `0x005403E0` / `0x004F8D1A` |
| `UseDirectionalDeaths` | `0x523E0533` | bit 6 | `0x00540A63` / `0x004F91ED` |

`SoldierAnimator::SetNewOwner` (Phantom `0x00754D60`) copies them into the
animator's `+0x201C` (`m_bUseDirectionalJumps` bit 0, `m_bUseDirectionalDeaths`
bit 1).

With the jump flag set, `SetAction` takes the move this frame over the frame
time along the body's forward row (`f`) and right row (`s`). Under 2 m/s
(`f*f + s*s < 4`) it plays `JUMP_UP`. Otherwise `JUMP_FORWARD` when
`f > |s|`, `JUMP_BACKWARD` when `-f > |s|`, else `JUMP_LEFT` when `s > 0` and
`JUMP_RIGHT` when not. So the body's "right" row points to its left, and the
`FLY` case agrees: a positive move along it is `FLY_LEFT`.

When the class's table is built (`SoldierAnimatorClass::_PostLoad`, Phantom
`0x0075EB30`), a missing `jump_forward`, `jump_backward`, `jump_left` or
`jump_right` (and `jetpack_hover`) takes `jump`'s entry, and a missing `sprint`
takes `stand_runforward`'s. Anything still missing is logged as
`Can't find soldier animation %s_%s_%s(_%s)`.

## How the finder names animations

`SoldierAnimationBank` keeps three static tables, filled as soldier classes
load. The first entries are the stock human set:

|                         | stride | layout | modtools | Steam | GOG |
|-------------------------|--------|--------|----------|-------|-----|
| `s_aBank[16]`   | 0x2C | `szName[32]`, `uiNameHash`, `eParent` +0x24, `eLowResBank` | `0x00ACECF8` | `0x007E9440` | `0x007EA070` |
| `s_aWeapon[20]` | 0x30 | `szName[32]`, `uiNameHash`, `eCombo`, `eParent` +0x28, `bHasAlertState` | `0x00ACF198` | `0x007E9070` | `0x007EA330` |
| `s_aMap[30]`    | 0x08 | `eBank`, `eWeapon` | `0x00ACF558` | `0x007E9700` | `0x007EA8E0` |

The stock entries are the `human` bank, the weapons `rifle`, `bazooka`, `tool`
(parent `rifle`), `pistol` and `melee` (parent `tool`), and their five maps. A
root bank or weapon is its own parent. Retail orders these tables differently
in `.data` (Steam puts the weapons before the banks), and on Steam and GOG
every code reference to them has a relocation.

`AnimationFinder::AssignAnimation` (Phantom `0x0075B760`) fills one half of one
entry. For an action whose own scope is the full body it tries
`<anim>_upper` (or `_lower`), then the plain `<anim>`, then `<anim>_full`.
Each try goes through `FindZephyrAnimation` (`0x0075CB80`), which names the
animation `"%s_%s_%s"` (bank, weapon, name), hashes it with PblTEMPHash and
looks it up. `_PostLoad` asks the soldier's bank first, then its parent banks
(`AssignAnimationFromBankParent`), then the weapon's parents
(`AssignAnimationFromWeaponParent`). The scope names are `sScopeName`:
`NONE`, `upper`, `lower`, `full`.

`SoldierAnimation` is 0x10 bytes: the `ZephyrAnim*`, a word packing the root
velocity and the scope (top two bits: 1 upper, 2 lower, 3 full), the
`SoldierAnimationData*` with its blend settings, and `mName`.
`SoldierAnimation::Init` (`0x0074D490`) only sets `mName` while
`g_bDebugAnims` is on, so an animation's name cannot be read back in play.
`SoldierAnimatorClass::FindAnimation(hash, name)` searches every loaded soldier
bank by hash (Steam ignores the name); `entity/anim_bank_append.cpp` hooks it.

## With ComboAnimIncrease

`[LimitIncreases] ComboAnimIncrease` (on by default, `entity/combo_anim_limit.cpp`)
is installed with the patch sets, before the feature installers. It changes
three things a reader of the tables above depends on:

- It replaces the body getters (both action getters among them) with shims, so
  their stock code is gone.
- It keeps every map's table in its own storage: `combo_anim_map`, whose first
  member is `action[38][2]` (upper, lower). A slot it never assigned holds
  `0xFFFFFFFF`.
- It moves `s_aMap` and `s_aBank` into registries of 90 and 64 entries, copying
  the stock rows; `s_aWeapon` keeps its 20.

`SoldierAnimatorClass::FindAnimation` is untouched. `entity/soldier_anim_tables.cpp`
gives one view of both layouts: it reads the stock tables, or
`combo_anim_limit_map_registry`, `_bank_registry` and `_action_slot` while
ComboAnimIncrease is in place. It only checks the stock getters when they are
still the stock code. The first directional rolls build skipped this, so with
ComboAnimIncrease on it refused to install.

## Rolls

`EntitySoldier::CheckMoveControlForRoll(move, strafe)` (Phantom `0x0056B520`)
allows a roll when the stick is pushed at least 0.9 of the way
(`f*f + s*s >= 0.81`) and not mostly backward (`f > 0`, or `|s| > -sqrt(3) * f`,
which is within 30 degrees of straight sideways). A roll is the crouch press
while moving: `EntitySoldier::Update` (Phantom `0x00580B80`) asks
`CheckMoveControlForRoll` when the crouch button goes down, and calls the
soldier's `Crouch` instead when it says no, so a backward stick crouches. While
rolling,
`EntitySoldier::Update` calls `EntitySoldier::MoveOffGround(dt, omega, thrust,
strafe)` (Phantom `0x00571840`). The thrust and strafe are the current stick
input times the class's `mAcceleration` (`+0x92C`) and its roll entries of
`mThrustFactor` and `mStrafeFactor` (per-state arrays at class `+0x960` and
`+0x980` on Phantom, roll at index 5).
It adds them along the body's current forward and right rows to a velocity
that keeps its momentum under a speed-dependent drag. It then turns the body by
`omega`: `GetOmega` of the turn input times `mTurnFactor[ROLL]` (`+0x9B4`), so
a soldier can turn with the camera mid-roll while the roll's way bends only
gradually. A roll ends after `0.75 * (frames - 1) / 30` seconds of
`GetLowerBodyActionAnimation(DIVE, HUMAN_RIFLE)`: three quarters of
human_rifle's dive whatever the soldier holds.

The animation is always `DIVE`. `SetupPose`'s DIVE case passes
`atan2(mMovement . right, mMovement . forward)` as the leg angle
(modtools x87 at `0x0057CC3F`, Steam `0x0063FDDC`, GOG `0x00640E7C`).
`UpdateActionAnimation` eases `mLegAngle` toward it at `min(dt * 7.5, 1)` a
frame, and `RotateLowerBody` turns one joint (CRC `0x4446E9B8`) by it, which
`ApplyProceduralAnimationAndBuildWorldMatrices` counters in the upper body. So a
roll to the side is the forward dive with the body turned toward the move. The
same aim keeps any dive on the soldier's real move when the body turns mid-roll:
the root turns back as the body turns.

## Directional rolls (built 2026-10-04)

`UseDirectionalRolls` (pbl_hash `0x3F77C468`, read by GameExt's ODF listener and
inherited through `ClassParent`) follows the jumps' model.

- **Choice.** On a roll's first `SetupPose`, the side comes from the jump rule
  above, using the move `SetAction` stored and `GameLoop::sClientDeltaTime`.
- **Lookup.** `diveleft` or `diveright` is looked up for the soldier's map the
  way the finder would: the map's weapon then each parent weapon, each with the
  map's bank then each parent bank, each by `_upper`/`_lower`, plain and
  `_full`.
- **Fallback.** A half without one keeps diveforward's, and a side with neither
  half plays the stock roll.
- **Play.** For that soldier's `SetupPose`, the DIVE entries for its map point
  at copies of diveforward's entries carrying the side dive, and `mMovement` is
  turned a quarter turn back in the body's plane (by -90 degrees for the left
  dive, +90 for the right). The leg angle then aims the root a quarter turn from
  the real move. A side dive moves a quarter turn from its root, so it follows
  the move as a forward dive does: a roll straight to the side starts facing the
  aim, and the root turns back when the soldier turns mid-roll. Both are
  restored as `SetupPose` returns.

The first build laid `mMovement` along the forward row instead (leg angle 0).
The dive then stayed square to the body and swung with the camera whenever the
soldier turned mid-roll; found in play 2026-10-05.

Code: `entity/directional_rolls.cpp`, through `entity/soldier_anim_tables.cpp`
for the tables.

An earlier version on the `dinput-hook` branch (March 2026) hooked `SetAction`
and the two getters. It had three problems:

- One flag for every soldier, which any other soldier's `SetAction` cleared.
- The leg angle left alone.
- Names read from `mName`, which is only set when animation debugging is on.

It also read a bank's parent from `+0x20`, which is the name hash, and picked
left and right the other way round from the engine's jumps (left for a negative
move along the right row).
