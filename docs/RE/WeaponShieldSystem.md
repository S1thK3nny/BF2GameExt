# WeaponShield

How `ClassLabel = "shield"` weapons actually work, and why the original channel fix
made things worse when the shield was not the selected weapon.

Addresses are given per build:

| Build | `WeaponShield::Update` | `Weapon::Update` |
|---|---|---|
| Phantom (`Battlefront2_Phantom.exe`, named PDB) | `0x7D0190` (thunk `0x40317A`, vtable `0xA1F728` slot 1) | `0x7B13E0` |
| Modtools | `0x63F360` | `0x61D850` |
| Steam | `0x691A80` | `0x6781B0` |
| GOG | `0x692B10` | `0x679250` |

`EntitySoldier::EnterControllable`: Phantom `0x56DA20` (thunk `0x4092A0`),
modtools `0x5448D0`, Steam `0x4F0CA0`, GOG `0x4F0CA0` (ported with
`tools/port_gog.py code`, score 1.00). All four are `__thiscall bool(this, Controllable*)`,
`RET 4`, with `this` = the EntitySoldier **struct base**, not the Controllable
sub-object at `+0x240`.

---

## Struct offsets

`Weapon` (Phantom struct, confirmed identical on modtools and Steam for every field
used here):

| Offset | Field |
|---|---|
| `+0x60` | `mStart` (`WeaponClass*`) |
| `+0x64` | `mClass` (`WeaponClass*`) |
| `+0x68` | `mRenderClass` |
| `+0x6C` | `mOwner` (`Controllable*`) |
| `+0x70` | `mAimer` |
| `+0x74` | `mTrigger` (`Trigger*`) |
| `+0x78` | `mReload` |
| `+0x88` | `m_pAmmoCounter` |
| `+0x8C` | `m_pEnergyBar` |
| `+0xAC` | bitfield: bit0 `mHideWeapon`, bit1 `mFiredFlag`, **bit2 `mSelectedFlag`**, bits 3..8 `m_iSoldierState` |
| `+0xB0` | `mState` (`WeaponState`) |

`mSelectedFlag` at `+0xAC & 4` is confirmed at the same offset on Phantom, modtools
(`TEST byte ptr [ESI+0xAC],4` at `0x61DA35`) and Steam (`0x67838C`). Not yet checked
on GOG.

`Trigger` is **4 bytes**. `Controllable::mControlFire` is `Trigger[2]` at
`Controllable+0x38`, so a weapon's fire channel is
`(mTrigger - (mOwner + 0x38)) / 4`.

`AmmoCounter`: `+0x00 m_pClass`, `+0x04 mDefaultMaxClips`, `+0x08 mMaxClips`,
`+0x0C mNumClips`, `+0x10 mCurClip`, `+0x14 m_uiRefCount`.

`Controllable+0xCC` is `mCharacter` on the retail/modtools builds (the Phantom dev
build has an extra field and puts it at `+0xC8`). `Character` is `0x1B0` bytes on
every build: `+0x148 mUnit`, `+0x14C mVehicle`, `+0x150 mRemote`. "Riding
something" is `mVehicle || mRemote`.

`WeaponShieldClass` starts at `WeaponClass`'s size, `0x304`:

| Offset | Field | ODF |
|---|---|---|
| `+0x304` | `mMaxShield` | `MaxShield` |
| `+0x308` | `mAddShield` | `AddShield` (per second while up, normally negative) |
| `+0x30C` | `mAddShieldOff` | `AddShieldOff` (per second while down) |
| `+0x31C` | `m_vShieldSize` | `ShieldRadius` / shield extents |
| `+0x328` | `mCollisionBody` | |
| `+0x478` | `mShieldEffect` | `ShieldEffect` |
| `+0x47C` | `mSoundShield` | `ShieldSound` |
| `+0x490` | `mSoundShieldOff` | `ShieldOffSound` |

Shield state itself lives on the **owner's `Damageable`**, not on the weapon
(`mOwner+0x18`, vtable slot `+0x20` returns it):

| Offset | Meaning |
|---|---|
| `+0x150` | current shield strength |
| `+0x154` | max shield |
| `+0x158` | current per-second shield delta (set from `mAddShield` / `mAddShieldOff`) |
| `+0x1FC` bit 3 | alive |
| `+0x1FC` bit 5 | shield is up |

Because the "is up" bit is on the owner, shield state survives a weapon switch by
design.

---

## `WeaponShield::Update`

```
on = Damageable.flags bit5

if (curShield <= 0 || !alive)                     goto OFF
if (owner is EntityDroideka && state in {11,12,13}) goto OFF   // ball mode

if (m_pAmmoCounter->mMaxClips == FLT_MAX)         // toggle shield
    if ((*mTrigger & 3) == 3) on ^= 1             // bit0 pressed, bit1 changed
else if (curShield > 0)                           // auto shield
    on = 1

if (!on) goto OFF

ON:
    Damageable[+0x158] = mClass->mAddShield
    if (!mShieldEffect && mClass->mShieldEffect) {
        play ShieldSound, PlayFireSound, create + attach the effect
    }
    update loop sound gain from 1 - (cur/max)
    if (ShieldRadius > 0) reposition the collision body, AddShieldToCollisionManager
    goto DONE

OFF:
    on = 0
    Damageable[+0x158] = mClass->mAddShieldOff
    destroy mShieldEffect
    stop the loop sound, play ShieldOffSound
    RemoveFromCollisionManager
DONE:
    Damageable.flags bit5 = on
    return Weapon::Update(this, dt)
```

### Two shield modes

The mode is chosen by the ammo counter, not by a dedicated property:

* `mMaxClips == FLT_MAX` (no clip count in the ODF): **player toggle**. This is the
  only branch that reads `mTrigger`.
* `mMaxClips` finite: **auto**. Up whenever shield strength is above zero,
  regardless of input.

Stock `cis_weap_walk_droideka_shield.odf` sets `RoundsPerClip` but no clip count, so
it is the toggle variant.

---

## The bug

`WeaponShield::Update` reads `mTrigger` directly, in its own body, before the base
call. Every other weapon type reads the trigger from inside `Weapon::Update`'s state
machine, and every one of those reads sits behind `TEST byte [weapon+0xAC],4`
(`mSelectedFlag`). `WeaponShield` has no such gate, so the toggle fires whenever the
fire button on that channel is pressed, even if a completely different weapon is
selected.

## Why the first fix was wrong

The original fix diverted the call to `Weapon::Update` whenever the shield was not
the active weapon for its channel. That does suppress the toggle, but it also skips
everything else in the function:

* **The OFF path never runs.** A shield raised and then switched away from keeps
  draining (the drain rate is stored on the `Damageable`, and `Damageable::Update`
  applies it). When strength reaches zero, nothing destroys `mShieldEffect`, stops
  the loop sound or calls `RemoveFromCollisionManager`. Result: the bubble stays on
  screen and a stale collision body is left behind, while the protection itself is
  already gone.
* **The ON upkeep never runs.** Effect attach, sound gain and repositioning the
  collision body all stop.
* **Auto shields break entirely.** The `mMaxClips` finite branch never touches the
  trigger, so a shield that should be on purely because it has strength left never
  activates while deselected.
* The detour also declared `void` where the function returns `bool`, so the base
  `Weapon::Update` result was not reliably propagated to the vtable caller.

## The effect object

`WeaponShieldClass::ShieldEffect` resolves to a `ShieldEffectClass`, and the
instance the weapon holds in `mShieldEffect` is a `ShieldEffect`: a `Thread` +
`PblHandled` + `FLEffectObject` + `RedSceneObject` with five fields of its own.

| Phantom / modtools | Field |
|---|---|
| `+0xD8` | `m_pClass` |
| `+0xE0` | `m_fScrollTimer` |
| `+0xE4` | `m_fTurnOnTimer` |
| `+0xE8` | `m_fTurnOnFactor` |
| `+0xEC` | `m_bTurnOn` |
| `+0xED` | `m_bStopAndFinish` |

`ShieldEffect::Update` (Phantom `0x744D90`, modtools `0x773430`; the retail builds
strip the `"FLEffect::Update"` profiler string this was found by) is the whole
lifetime:

```
m_fTurnOnTimer += dt;  m_fScrollTimer += dt;
if (!m_bTurnOn) {
    if (m_fTurnOnTimer >= m_pClass->m_fTurnOffTime) {
        if (m_bStopAndFinish) return false;   // finished -> engine destroys it
        factor = 0;
    } else factor = 1 - m_fTurnOnTimer / m_pClass->m_fTurnOffTime;   // the dissolve
} else {
    factor = min(m_fTurnOnTimer / m_pClass->m_fTurnOnTime, 1);
}
m_fTurnOnFactor = factor;
return FLEffectObject::Update(this, dt);
```

`ShieldEffect::TurnOff` is `if (m_bTurnOn) { m_bTurnOn = false; m_fTurnOnTimer = 0; }`,
and `ShieldEffect::StopAndFinish` (vtable slot `0x20`) is `TurnOff(); m_bStopAndFinish = true`.
So the OFF path's effect release is a *fade*, not a removal.

`ShieldEffect::DeactivateEffect` (slot `0x1C`) does remove it at once -- scene
object out via `RedSceneObject(+0x80)->vtable[0x48]`, then
`FLEffectObject::DeactivateEffect` unlinks the thread. It is **not** usable on its
own: unlinking the thread means `Update` never runs again, never returns false, and
the object is never destroyed. The engine only ever calls it on a path that returns
false to the thread updater in the same breath (`FLEffectObject::Update`, when the
attached object has died).

`WeaponShield::mShieldEffect` is one of the few offsets here that is not
build-invariant: **modtools `+0x1C8`, Steam/GOG `+0x198`** (read off the ON path's
store at `0x63F511` / `0x691C2F`).

## The boarding bug

Nothing drops the shield when the soldier boards something.
`EntitySoldier::EnterControllable` releases the controller, deactivates physics,
removes the soldier from the collision manager and calls `Character::SetVehicle`,
but the shield's up bit lives on the `Damageable` and is never touched. The effect
is attached to the soldier's geometry, which now sits at the vehicle's origin, so
the bubble ends up floating at the root of whatever was boarded. Losing the
controller also kills the soldier's fire triggers, so the player cannot toggle it
back down.

The engine has the same problem with the jetpack and solves it by hand at exactly
this point (`TurnOffJetEffect` / `TurnOffJetIdleEffect` are called right after
`Character::SetVehicle`). The shield was simply left out.

## The fix

Hook `WeaponShield::Update`, and when the shield is not the active weapon for its
channel, still call the original but point `mTrigger` (weapon `+0x74`) at a zeroed
4-byte stand-in for the duration of the call, restoring it afterwards.

`(*mTrigger & 3) == 3` can then never be true, and nothing else in the function
changes behaviour. The base `Weapon::Update` at the tail reads the trigger too, but
only from inside `mSelectedFlag`-guarded branches, which a deselected weapon cannot
reach anyway.

For boarding, a second hook on `EntitySoldier::EnterControllable` walks the
soldier's `mWeapon[8]`, identifies shields by comparing vtable slot 1 against the
known `WeaponShield::Update` address (Detours patches the function body, not the
vtable, so the slot still holds the original address), and drives one shutdown
pass on each: clear `Damageable+0x1FC` bit 5, then call `WeaponShield::Update` once
with the trigger masked. `on` computes to zero, so the engine's own OFF path does
the teardown. The `Update` hook additionally holds the shield down for as long as
`mVehicle`/`mRemote` is set, which covers the case where the soldier keeps
updating while riding, and stops it being re-raised.

To make the bubble go with the soldier rather than dissolve behind them, the
released effect's clock is run out instead: call the effect's own `Update` (vtable
slot 1) with a 1-second step until it reports finished, capped at 64 steps. For a
`ShieldEffect` that is one call, since `TurnOff` has just reset `m_fTurnOnTimer` to
zero and any sane `m_fTurnOffTime` is under a second; the engine then destroys it on
its next pass exactly as it would have after the dissolve. Anything that does not
finish inside the cap is left alone and keeps stock behaviour, which is what makes
this safe against a `ShieldEffect` property pointing at some other effect type. The
effect pointer is captured before the call and only used if the weapon cleared its
own field, so it fires once and never on an effect still in use.

Note this only reaches the toggle variant. A finite-clip auto shield forces `on = 1`
whenever it has strength, so clearing the bit cannot hold it down; no stock content
uses that variant.

Implemented in `PatcherDLL/src/weapon/shield_channel_fix.cpp`.

### Open item

The predicate is still the soldier-layout walk (find this weapon in the owner's
`mWeapon[8]`, compare against `mWeaponIndex[channel]`), which deliberately bails out
and allows stock behaviour on non-`EntitySoldier` owners such as `EntityDroideka`.
`mSelectedFlag` (`weapon+0xAC & 4`) would replace all of it with a single byte read
and is exactly what the engine's own state machine uses, but it has not been
confirmed that `EntityDroideka` calls `Weapon::Select` on its shield. Confirm that
before simplifying, since a wrong verdict there would stop the droideka shield from
toggling at all.
