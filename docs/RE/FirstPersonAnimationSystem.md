# First-Person Animation System

How the first-person soldier animation state machine works, and why the first shot
after entering first person plays no animation.

Soldiers only. Vehicles go through `FirstPersonRenderable::UpdateCockpit`, which has
none of this state machine, which is why the bug never shows in a cockpit.

Addresses are given per build:

| Build | `FirstPersonRenderable::UpdateSoldier` | `FirstPerson::Update` | `FirstPersonRenderable::Activate` |
|---|---|---|---|
| Phantom (`Battlefront2_Phantom.exe`, named PDB) | `0x5B8F20` | - | `0x5B55C0` (ctor at same module) |
| Modtools | `0x4A9BE0` | `0x4AAF80` | `0x4A74E0` (vtable `0xA37090` slot 1) |
| Steam | `0x51FB70` | - | - |
| GOG | `0x51FB70` | - | - |

Supporting functions (modtools): `FirstPerson::Init` `0x4AB64A` region,
`FirstPersonRenderable::FirstPersonRenderable` `0x4AB400`,
`FirstPersonRenderable::SetAnimation` `0x4A73E0` region,
`FirstPersonRenderable::SetAnimAndTransition` `0x4AAF10`,
`FirstPersonRenderable::UpdateSoldierAimer` (Phantom `0x5B94E0`),
`Weapon::SignalFire` `0x61C870`, `Weapon::Update` `0x61D850`.

---

## Struct offsets

`FirstPersonRenderable` (Phantom PDB struct, 0x1650 bytes; layout is build-invariant,
confirmed against the modtools and Steam disassembly for every field used here):

| Offset | Field |
|---|---|
| `+0x1560` | `mZephyrPoseDynamic.m_bAnimFinished` |
| `+0x1564` | `mBlendFactor` |
| `+0x15F0` | `mAnimPlaySpeed` |
| `+0x15F4` | `mEntitySoldierState` |
| `+0x15F8` | `mAimer` |
| `+0x15FC` | `mObjectModel` (`RedModel*`) |
| `+0x1600` | `mCurrentWeapon` (`Weapon*`) |
| `+0x1604` | `mWeaponClass` |
| `+0x1608` | `mSoldierState` |
| `+0x160C` | `mObjectType` |
| `+0x1610` | `mOwner` (`Controllable*`) |
| `+0x1614` | `mUpdatedSoldierAimer` |
| `+0x1618` | `fTransitionTimer` |
| `+0x161C` | `fTransitionTimerMax` |
| `+0x1620` | `mTransitionAnim` |
| `+0x1624` | `mBlendHandsDown` |
| `+0x1648` | `mCameraId` |
| `+0x164C` | `bReadyToRender` |
| `+0x164D` | `bStreamingModel` |

`Weapon` (Phantom struct):

| Offset | Field |
|---|---|
| `+0xAC` bit 0 | `mHideWeapon` |
| `+0xAC` **bit 1** | **`mFiredFlag`** |
| `+0xAC` bit 2 | `mSelectedFlag` |
| `+0xAC` bits 3..8 | `m_iSoldierState` |
| `+0xB0` | `mState` (`WeaponState`: 0 IDLE, 1 FIRE, 2 FIRE2, 3 CHARGE, 4 RELOAD, ...) |
| `+0xC4` | `mMuzzleFlashStartTime` |

## Animation slots

`FirstPerson::mAnim` (modtools `0xB70E30`, Steam `0x1E55E30`, GOG `0x1E572E0`) is a flat
`ZephyrAnim*[48]` indexed `weaponClass * 11 + soldierState`.

`soldierState` values, as produced by `UpdateSoldier`:

| Value | Meaning | Produced by |
|---|---|---|
| 0 | idle | movement default, standing |
| 1 | run | movement default, moving |
| 2 | shoot | fire branch, `mState == FIRE` |
| 3 | shoot2 | fire branch, `mState == FIRE2` |
| 5 | reload | `mState == RELOAD` |
| 7 | jump | `mEntitySoldierState` 4 or 5 |
| 8 | flail | `mEntitySoldierState` 8 |
| 9 | handsdown | weapon swap, hands-down blend, `field_0x2b4 & 0x20` weapons |
| 10 | land | jump transition target |

**2 and 3 are produced by nothing but the fire branch** (`state = (mState != FIRE) + 2`).
Nothing else in the function can yield them, and every `mTransitionAnim` the engine
ever stores is 0, 9 or 10. That makes "state is 2 or 3" an exact test for "this frame
is animating a shot".

`FirstPerson::Init` resolves all 48 slots once, at `ReadDataFile("ingame.lvl")` time,
and **falls any unresolved slot back to `humanfp_tool_idle`** rather than leaving it
null. So a missing shoot animation in a custom bank presents as a permanently idle
weapon, never as a crash, and never as an intermittent one - useful for telling that
failure apart from the bug below.

---

## The fire path

The shoot animation is edge-triggered by a latch:

1. `Weapon::SignalFire` sets `mFiredFlag` (`Weapon+0xAC` bit 1) once per shot, and
   stamps `mMuzzleFlashStartTime`. `WeaponCannon::EnterFire` also sets it up front when
   `WeaponClass+0x2B0 & 0x40`.
2. `UpdateSoldier` reaches its `FIRE`/`FIRE2` branch only when `mFiredFlag` is set,
   picks state 2 or 3, and **clears the latch in the same breath**:

```c
case FIRE: case FIRE2:
    if ((weapon->field_0xac >> 1 & 1) == 0) break;      // no shot pending -> movement states
    if (weapon->mClass && (weapon->mClass->field_0x2b4 & 0x20)) { ... handsdown ... }
    weapon->field_0xac &= 0xFFFFFFFD;                    // <-- latch consumed here
    soldierState = (weapon->mState != FIRE) + 2;
    bForceSetAnimation = true;
```

Because the latch is consumed rather than re-tested, anything that overrides
`soldierState` *after* this point does not delay the shot's animation. It destroys it.

---

## The bug

Further down the same call, after the state has been chosen:

```c
// modtools 0x4A9F13 .. 0x4A9F43,  Steam/GOG 0x51FED8 .. 0x51FF09
if (speedSq < 0.1f || this->mEntitySoldierState != 2) {
    if (bTransitionAllowed && this->fTransitionTimer < this->fTransitionTimerMax) {
        soldierState = this->mTransitionAnim;                 // <-- overwrites the shoot state
        this->fTransitionTimer += GameLoop::sDeltaTime;
    }
}
```

`bTransitionAllowed` is initialised `true` at the top of the function and is cleared
only by the JUMP (`mEntitySoldierState == 4`) and FLAIL (`== 8`) branches. The fire
branch `goto`s straight past both, so it is still `true` while a shot is being
animated. The shoot state is replaced by `mTransitionAnim` and `mFiredFlag` is already
spent, so the shot silently animates as whatever the transition is.

### What arms it on entering first person

`FirstPersonRenderable::Activate` - vtable slot 1, reached from
`Trackable::SetFirstPersonView` -> `FirstPerson::Activate` when the player toggles into
first person - does:

```c
this->bReadyToRender  = false;   // +0x164C
this->mCurrentWeapon  = NULL;    // +0x1600
this->mBlendHandsDown = true;    // +0x1624
```

`UpdateSoldier` consumes that latch at modtools `0x4A9F44` / retail `0x51FF0A`:

```c
if (this->mBlendHandsDown && this->bReadyToRender) {
    soldierState = 9;                    // handsdown
    this->mBlendHandsDown   = false;
    this->fTransitionTimer  = 0.0f;
    this->fTransitionTimerMax = 1.0f;    // <-- one full second
    this->mTransitionAnim   = 0;         // <-- blending back to idle
}
```

So entering first person arms a **1.0 second** hands-down to idle blend, and every shot
fired inside that second is swallowed by the transition block above. That is the
reported symptom, and it explains both halves of the usual description of it: waiting
a moment fixes it (the timer expires), and it feels like an animation has to play first
(the hands-down blend is that animation).

The timer only advances on frames where `speedSq < 0.1 || mEntitySoldierState != 2`, so
the window is wall-clock only while standing still.

### Why the first time is worse

`FirstPerson::Update` (modtools `0x4AAF80`) only clears `bStreamingModel` once the FP
model is actually resident:

```c
if (modelIdx != s_CurModel[cam] && GameLoop::mNumCameras == 1) {
    model = LoadDynamicModel(name, 1);
    if (model == 0) { renderable->bReadyToRender = 0; renderable->bStreamingModel = 1; }
    else            { s_CurModel[cam] = modelIdx; renderable->bStreamingModel = 0; }
}
```

and `UpdateSoldier` gates `bReadyToRender` on it:

```c
if (this->bStreamingModel == false) this->bReadyToRender = true;
```

The `mBlendHandsDown` block requires `bReadyToRender`, so while the FP model streams in
the latch just sits there and the one-second countdown has not even started. The first
time a given FP model loads, the dead window is therefore noticeably longer than a
second - which is the "it needs time to construct properly" half of the description.

### Same bug, other triggers

Anything that arms a transition opens the same window:

| Trigger | `fTransitionTimerMax` | `mTransitionAnim` |
|---|---|---|
| Enter first person (`Activate` -> `mBlendHandsDown`) | 1.0 s | 0 (idle) |
| Jump, `mEntitySoldierState == 4` | 0.5 s | 10 (land) |
| Flail, `mEntitySoldierState == 8` | 1.0 s | 9 (handsdown) |
| `SetAnimAndTransition` (`0x4AAF10`, FP camera events) | caller-supplied | caller-supplied |

Note that the JUMP and FLAIL branches clear `bTransitionAllowed` for the frame that
arms them, so the override starts on the following frame.

---

## The fix

Implemented in `PatcherDLL/src/entity/fp_fire_animation_fix.cpp`.

Exempt the shoot states from the transition override. Since 2 and 3 are produced by the
fire branch and by nothing else, testing for them is exact and needs no extra state.
The timer is left advancing, so the blend still expires on schedule around the shot.

The patch site is the single instruction that performs the override. Both builds are the
same instruction with a different register allocation, and both are exactly 6 bytes:

| Build | Site | Bytes | Instruction | Resume |
|---|---|---|---|---|
| Modtools | `0x4A9F32` | `8B AE 20 16 00 00` | `MOV EBP,[ESI+0x1620]` | `0x4A9F38` `FADD [ESI+0x1618]` |
| Steam | `0x51FEFC` | `8B 9F 20 16 00 00` | `MOV EBX,[EDI+0x1620]` | `0x51FF02` `MOVSS [EDI+0x1618],XMM1` |
| GOG | `0x51FEFC` | `8B 9F 20 16 00 00` | `MOV EBX,[EDI+0x1620]` | `0x51FF02` `MOVSS [EDI+0x1618],XMM1` |

GOG ported from Steam with `tools/port_gog.py code`, score 1.00, shift +0. All three
sites were byte-verified against the shipped executables before the patch was written.

The site is displaced into a 21-byte cave:

```
    CMP  state, 2
    JE   skip
    CMP  state, 3
    JE   skip
    MOV  state, [this+0x1620]     ; the displaced instruction
skip:
    JMP  resume
```

The state register is derived from the `reg` field of the verified `MOV` rather than
hardcoded per build, so the cave cannot disagree with the bytes it displaced.

Three things make the displacement safe:

- **x87** - on modtools the site sits between `FLD sDeltaTime` and `FADD`, so ST0 is
  live across it. The cave is integer-only.
- **XMM** - on retail XMM1 holds the already-advanced timer across the site. Same
  argument.
- **EFLAGS** - dead across the site on both builds. The next reader is the fresh
  `CMP byte ptr [this+0x1624],..` of the `mBlendHandsDown` block, so the cave's `CMP`s
  are free to clobber it.

### Residual, not patched

The `mBlendHandsDown` block immediately after the site also forces state 9
unconditionally. It runs on exactly one frame - it clears the flag as it fires - so it
can still eat a shot when the two coincide. That turns a one-second window into a
one-frame race. Protecting the shoot state there too would push the hands-down blend a
frame later on entering first person, which is a visible change for a 1-in-60 case, so
it was left alone deliberately.

---

## Related

- [`docs/RE/CharacterWeaponSystem.md`](CharacterWeaponSystem.md) - `Weapon` layout, and the
  stale `mCurrentWeapon` hazard when a weapon is swapped out from under the FP renderable.
- `PatcherDLL/src/entity/soldier_fp_animation_override.cpp` - the
  `FirstPersonAnimationBank` ODF property, which hooks the same `UpdateSoldier` to swap
  `FirstPerson::mAnim` for the duration of the call.
