# Soldier action animations

How a soldier's actions (jumps, rolls, landings, deaths, jets) pick their
animations, how the stock directional jumps work, how a soldier's pose is built
each frame, and the research behind `UseDirectionalRolls` (2026-10-04) and
`UseDirectionalJets` (2026-10-05). Names and logic are from the Phantom PDB
build in Ghidra; every address used by the DLL was then read on modtools, Steam
and GOG and is audited by `tests/directional_rolls_abi_tests.py` and
`tests/directional_jets_abi_tests.py`.

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
for the tables and the lookup (`soldier_anim_tables::find_named`, shared with
the directional jets since 2026-10-05).

An earlier version on the `dinput-hook` branch (March 2026) hooked `SetAction`
and the two getters. It had three problems:

- One flag for every soldier, which any other soldier's `SetAction` cleared.
- The leg angle left alone.
- Names read from `mName`, which is only set when animation debugging is on.

It also read a bank's parent from `+0x20`, which is the name hash, and picked
left and right the other way round from the engine's jumps (left for a negative
move along the right row).

## How SetupPose builds a pose

`SoldierAnimator` holds the pose and the players that fill it, at the same
offsets on every build:

| Member | Offset | |
|--------|--------|-|
| `mUpperBodyAnimMask`, `mLowerBodyAnimMask` | `0x58`, `0x5C` | `PblBitVector<32>`: one bit per skeleton joint |
| `mZephyrSkeleton` | `0xD0` | `ZephyrSkeleton<32>`: the shared skeleton, then 32 world matrices from `+0x10` |
| `mZephyrPoseStatic` | `0x8E0` | `ZephyrPoseStatic<32>`: the local pose being built |
| `mZephyrPoseDynUpper`, `mZephyrPoseDynLower` | `0xC64`, `0x1614` | `ZephyrPoseDyn<32>` (`0x9B0` each): each half's player |
| `m_pAnimLower` | `0x1FC4` | the `SoldierAnimation` on the legs |
| `mBlendFactorLower` | `0x1FE0` | how far the legs' animation has blended in |

Each frame the soldier is drawn, `EntitySoldier::Render` calls `SetAction` with
the matrix it draws (network-interpolated for other players) and then
`SetupPose`, which:

1. Switches on `mAction`. Most actions, `JET` among them, go through
   `UpdateActionAnimation`: it starts or advances each half's player and lays
   it on `mZephyrPoseStatic` through that half's mask, with
   `ZephyrPoseStatic<32>::Set` once the animation has blended in and `::Blend`
   by the step until then. The pose stays from frame to frame, so a change of
   animation blends from the last pose drawn.
2. Turns `DummyRoot` to the leg angle (`RotateLowerBody`), setting its rotation
   outright after both halves are laid on. For `JET` the leg angle eases to 0.
3. Calls `ApplyProceduralAnimationAndBuildWorldMatrices`: the aim's turn of
   `bone_a_spine` (or `bone_abdomen`), `bone_b_spine`, `bone_ribcage`,
   `bone_neck` and `bone_head`, made on world matrices it builds as it goes,
   then the rest of the world matrices.
4. Hands the world matrices to the renderer (`RedPose::ConvertFromZephyrPose`).

`SoldierAnimatorClass::SetupBodyMasks` makes the masks. The lower body is ten
bones (`auiNormalLowerBodyBoneHash`): `bone_root`, `bone_pelvis` and
`bone_l_`/`bone_r_` `thigh`, `calf`, `foot` and `toe`; a skeleton with an
acklay's bone uses a list of fifteen. The upper body is every other joint but
joint 0.

The bone names are from their PblTEMPHash: `DummyRoot` `0x4446E9B8`,
`bone_root` `0x16E27226`, `bone_pelvis` `0xE6E47876`, `bone_a_spine`
`0x5DE52389`, `bone_abdomen` `0x93E93819`, `bone_b_spine` `0x8C1C9BBA`,
`bone_ribcage` `0xE5B51072`, `bone_neck` `0xD2AFB28A`, `bone_head`
`0x8AFDF5E4`.

For a pilot animation, `SetupPose` already lays an extra animation (the
overlay) on the pose this way: a `ZephyrPoseDyn<32>` on its stack, `Open`ed on
the skeleton, given the animation with `SetAnimation(anim, 30.0)` and laid on
through the upper mask with `Blend`.

### Zephyr

A `ZephyrPoseDyn<32>` plays one `ZephyrAnim`: `m_kAnim` (a
`ZephyrAnimInst<32>`: per-joint decoder state and the joint maps between the
animation and the skeleton, room for 32 joints) and its time `m_fCurT`, 0 at
the first frame and 1 at the last. `GetJointTransform` reads frame
`(frames - 1) * m_fCurT` and interpolates to the next; with `m_bLoop` the frame
after the last is frame 1. The decoders (`DecompQuatToFrame`) run forward
through run-length deltas, from frame 0 again when asked for an earlier frame,
and check no bounds: a one-frame animation that loops reads past its data.

| `ZephyrPoseDyn<32>` | Offset | | `ZephyrAnim` | Offset |
|---------------------|--------|-|--------------|--------|
| `m_kAnim.m_piAnimJointIdx` | `0x940` | | `m_u16NumFrames` | `0x8` |
| `m_kAnim.m_pkAnim` | `0x980` | | `m_u16NumJoints` | `0xA` |
| `m_pSkel` | `0x988` | | | |
| `m_fCurT` | `0x99C` | | | |
| `m_bLoop`, `m_bInterpolate` | `0x9AA`, `0x9AD` | | | |

`ZephyrPoseStatic<32>` is a skeleton pointer and one transform per joint
(quaternion then translation, `0x1C` bytes); a quaternion `w` of 1e6 marks a
joint not set yet. Its `Blend` comes in four forms, from another static pose or
a player, with or without a mask; `UpdateActionAnimation` uses
`Blend(ZephyrPoseDyn<32>*, PblBitVector<32>*, float t)`, which moves each joint
the mask and the animation both have `t` of the way to the animation (slerp and
lerp), or sets one not set yet.

| | modtools | Steam | GOG |
|-|----------|-------|-----|
| `ZephyrPoseDyn<32>::SetAnimation`: thiscall(anim, fps), RET 8 | `0x0082AAC0` | `0x0072D430` | `0x0072E500` |
| `ZephyrPoseStatic<32>::Blend(dyn, mask, t)`: thiscall, RET 0xC | `0x0082D450` | `0x0072DB30` | `0x0072EC00` |
| `ApplyProceduralAnimationAndBuildWorldMatrices` | `0x00579F10` | `0x00642860` | `0x00643900` |
| `SetupPose`'s call of it | `0x0057D3ED` | `0x006406DC` | `0x0064177C` |

`ApplyProceduralAnimationAndBuildWorldMatrices` is thiscall(float dt), RET 4,
on modtools; on Steam and GOG (LTCG) it takes the frame time in XMM1 and
returns with a plain RET. `SetupPose` is its only caller.

## Jets

`SetAction` plays `JET` (`jetpack_hover`) for both `JET_JUMP` and `JET_HOVER`.
`EntitySoldier::Update` moves them differently:

- **`JET_HOVER`** calls `EntitySoldier::MoveJetHover`: the velocity decays by
  `e^(-2 dt)`, then is pushed toward target speeds, along the eye direction and
  the body's right row, by at most `JetAcceleration * dt`. The targets are the
  stick times `MaxSpeed` (forward) or `MaxStrafeSpeed` (backward), times
  `mThrustFactor[jet]`, and the strafe stick times `MaxStrafeSpeed` times
  `mStrafeFactor[jet]`.
- **`JET_JUMP`** adds `JetPush * dt` upward and calls `MoveOffGround` with the
  stick times `JetAcceleration` as the push: no target speed.

`mThrustFactor`, `mStrafeFactor` and `mTurnFactor` hold one value per
`ControlSpeed` posture (`ControlSpeed = "<posture> thrust strafe turn"`, read by
`EntitySoldierClass::SetProperty`); `jet` is index 6, with defaults 0.3, 0.3
and 1.0 from the class constructor. `EntitySoldierClass_data` sits as one block,
`0xA0` lower on modtools than on Phantom and `0x294` lower on release. Where
`Update` reads them for `MoveJetHover`:

| `EntitySoldierClass` | modtools | Steam and GOG |
|----------------------|----------|---------------|
| `mMaxSpeed` | `0x890` @`0x00548AE1` | `0x69C` @`0x004EBA85` |
| `mMaxStrafeSpeed` | `0x894` @`0x00548AE9` | `0x6A0` @`0x004EBA8F` |
| `mThrustFactor[6]` | `0x8D8` @`0x00548AF7` | `0x6E4` @`0x004EBABE` |
| `mStrafeFactor[6]` | `0x8F8` @`0x00548B05` | `0x704` @`0x004EBAB6` |
| `mJetAcceleration` | `0x9BC` @`0x00548B11` | `0x7C8` @`0x004EBAAA` |

## Directional jets (built 2026-10-05)

`UseDirectionalJets` is a soldier ODF switch, kept per class and inherited
through `ClassParent` like `UseDirectionalRolls`.

- **Names.** `jetpack_hover_forward`, `_backward`, `_left` and `_right`, each
  the pose while moving that way. Each is looked up for the soldier's map like
  the side dives (`soldier_anim_tables::find_named`), for the lower half
  (`_lower`, plain, `_full`). One with more than 32 joints is skipped and
  logged. A way without one keeps the hover for its share.
- **Where.** `SetupPose`'s call of `ApplyProceduralAnimationAndBuildWorldMatrices`
  is retargeted to a stand-in that keeps every general and XMM register and the
  stack, does the blend, and jumps on to the function: the local pose is
  finished there (the leg twist included), and nothing has been turned to the
  aim or built into world matrices yet.
- **When.** For a soldier in `JET` whose class has the switch and whose legs
  play the map's `JET` lower animation (not a melee swing).
- **How much.** The body's speed along its forward and right rows is its move
  this frame (`mMovement`, from the drawn matrix) over the frame time, as
  `SetAction` reads it for directional jumps. Each part over the unit's top jet
  speed that way (the `MoveJetHover` targets at full stick; the run speed where
  the jet factor is 0, as it can still move off a jet jump) gives the lean, kept
  in the unit disc. The legs' lean follows it at `min(dt * 7.5, 1)` of the way a
  frame, the rate `UpdateActionAnimation` turns the leg angle. Its length is the
  directional share, split between its forward-or-backward way and its side in
  proportion to the two parts.
- **Blend.** For each way with a share, a `ZephyrPoseDyn<32>` on the stack,
  zeroed, with `m_pSkel` the animator's skeleton and `m_bInterpolate` set,
  takes the animation with `SetAnimation(anim, 30.0)` and the hover player's
  `m_fCurT`, never looping. `Blend` lays it on `mZephyrPoseStatic` through
  `mLowerBodyAnimMask` by its share over the share laid so far, the hover's
  included, so each ends with its own share.

The next frame `UpdateActionAnimation` sets the legs from the hover again, so
nothing builds up; when the soldier leaves `JET`, the next animation blends in
from the leaning pose.

Code: `entity/directional_jets.cpp`, the weights in
`entity/directional_jets_core.hpp` (`tests/directional_jets_tests.cpp`); audit
`tests/directional_jets_abi_tests.py`.

## Additive layers (researched 2026-10-11, not built)

The idea: lay a flinch, breathing, recoil or lean over whatever a soldier is already doing, as a change
added to the pose, instead of a full animation for every case. Read in Ghidra by a research pass;
nothing is built. P = Phantom, M = modtools, S = Steam, G = GOG.

**Zephyr has no additive blending.** Its pose operations are `Set` and `Blend` (slerp or lerp toward
a target), four forms each, plus `SetBasePose` and the `SetJointLocal` setters (the method list,
P `0x0086CA70` to `0x00874450`). The only delta is root motion (`GetRootTransDelta`). The one layer laid
over a pose is the procedural aim turn in `ApplyProceduralAnimationAndBuildWorldMatrices`
(P `0x007518A0`). A blend's factor is its timer over its duration, applied each frame as an
incremental `Blend` step (`CalculateBlendFactorDelta`, P `0x007527C0`) through the half's mask.

**The stock flinch shows one frame.** `SetDamageDir` (P `0x00754C30`, M `0x005774E0`, S `0x00643470`,
G `0x00644510`; thiscall(dir), RET 4), called from `EntitySoldier::ApplyDamage` (M `0x0054BE26`,
S `0x004E1C64`), sets `mOverlayAnim` (+0x90) to the upper body's `HIT_*` animation, `mOverlayTime`
(+0x94) to 1 and `mOverlayBlendFactor` (+0x98) to 0.5. `SetupPose` decays it at 8 a second (constant
M `0x00A44FDC`), and `UpdateMovementAnimation` (M `0x0057B650`) lays it on with a fresh stack player,
so only frame 0 ever shows: at most half strength, for an eighth of a second, in the `MOVE` action
only, on the legs only while idle, never prone. The stock `stand_hit*` animations have 16 frames,
15 of them never seen.

**What is there to build on.**

- `ZephyrPoseDyn<32>::GetJointTransform(out, joint, t)` samples one joint at a given time without
  moving the player's own clock: P `0x00871300`, M `0x0082A5B0`, S `0x0072CDC0`, G `0x0072DE90`;
  thiscall, RET 0xC, read on all three. S and G match byte for byte except the stack-cookie address
  (S `0x007E3CB8`, G `0x007E4CB8`), which moves with the exe, so a prologue guard must mask it. The
  two-argument form (M `0x0082A790`) uses the player's `m_fCurT`. The decoders run forward and start
  again from frame 0 when asked for an earlier frame.
- `ZephyrAnimInst<32>::SetAnim` (P `0x00874340`) maps a clip's joints to the skeleton's by CRC, with
  no bounds check; skeleton joints the clip lacks are skipped, so a partial clip touches only its own
  joints.
- `BuildWorldMatricesRecurseDown` (P `0x0086D8E0`) uses a pose's translation only on the joints
  `SetupJointTransformAnimationFlag` marks (`bone_root` and `hp_weapons` on human skeletons) and
  normalizes each rotation, so for most joints only rotations count.
- The directional jets' stand-in: the local pose is finished there and nothing is in world matrices yet.

**A design.** One shared stand-in on that call runs the jets' lean, saves the joints the layers will
touch, adds the layers, calls `ApplyProceduralAnimationAndBuildWorldMatrices`, and puts the saved
joints back. The restore is essential: the pose persists between frames and blends from the last one
drawn, so a delta left in it would build up through every blend (an estimated 3 to 5 times
overshoot; inferred). The simplest place to restore is a second register-keeping stand-in on the next
call, `RedPose::ConvertFromZephyrPose` (M `0x0057D400`, S `0x006406EB`, G `0x0064178B`), which only stores
pointers to the world matrices (P `0x008B7CB0`). Jets refuse to install over an already retargeted
call, so both features need the one shared hook.

- Each layer: a stack player set up like the jets' (zeroed, `m_pSkel`, `m_bInterpolate`,
  `SetAnimation(anim, 30)`); the clip's frame 0 as the reference, cached once per clip; the delta is
  the reference's inverse times the current frame, slerped from no rotation by the layer's weight and
  composed onto the pose. Skip joint 0, `DummyRoot` (`RotateLowerBody` sets it every frame) and joints
  not yet set. A loop's time is wrapped by hand: `m_bLoop` would add root translation.
- Triggers: a hit (detour `SetDamageDir`, play `add_hit<dir>`, and optionally zero `mOverlayTime` to
  drop the stock one-frame flinch); a shot (`SetWeaponState` going to `FIRE`: P `0x00755360`,
  M `0x00576A80`); breathing (a loop scaled by energy); a lean (from `mMovement`, as jets do).
- Authoring: names like `<bank>_<weapon>_add_<name>`, found through `soldier_anim_tables` with the
  weapon and bank parents, `_upper` or `_lower` to narrow it; the clip existing is the opt-in, as the
  flyer's `boost` clip is. At least 2 frames, at most 32 joints, frame 0 neutral, rotations only,
  munged with `/keepframe0` into a new numbered sub-bank so `AnimationBankAppend` picks it up.
- A procedural version (a spring-damped kick, sine breathing, a roll lean spread over the spine as the
  aim spreads its yaw) needs no art and could share the core.

The hook never reaches the pilot and choke branch of `SetupPose` (it builds its own matrices),
low-resolution pooled soldiers (`SoldierAnimatorLowRes_Pool::GetPose`, P `0x00764150`) or first-person
arms. Flyers (and probably hovers) play 32-joint clips at render time where the flyer boost already
builds a pose; walkers use 64-joint poses (`EntityWalker::BuildWorldMatrices`, P `0x005942B0`: `ApplyBlend`,
M `0x0054FFD0`, then `CorrectJoints`), which would need more research.

**Risks.** The quaternion multiplication order must be checked (D3DX reverses it). A fresh player
decodes from frame 0 every frame, which is fine for short clips. Arm deltas move the animated
`hp_fire`, so they move the host's barrel fire origin. Nothing is sent online, so each trigger must
fire on every machine. The ODF listener's four property handlers are all taken, so a new ODF property
needs a fifth slot or a shared handler.

**Open:** whether `SetDamageDir` runs on a multiplayer client for remote and local soldiers; whether a
remote shooter reaches `SetWeaponState` on clients (inferred yes); how zenasset handles partial-skeleton
exports; the cost with many soldiers on screen. The stock `stand_hit*` frame 0 is 20-50° from the idle
pose on the spine and head, so it is probably the hit pose itself and new clips are needed (inferred).
