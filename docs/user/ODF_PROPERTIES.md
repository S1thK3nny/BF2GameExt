# ODF Properties

Properties added by BF2GameExt, on top of the stock ones. The game ignores properties it does not recognise, so an ODF using these still loads and plays without the extension installed. The property simply does nothing.

For which builds each one works on, see the [compatibility table](../../README.md#compatibility).

## GameExt-Only Values

*New in 1.1.0.*

One ODF, two sets of values: the normal line is what a stock game sees, and a second line ending in `@GameExt` is what the extension uses instead. Nothing else changes, and the ODF still loads and plays normally without the extension installed.

```
WeaponName         = "all_weap_inf_rifle"
WeaponName@GameExt = "all_weap_inf_bowcaster"
```

A stock game gives the unit the rifle. With BF2GameExt installed it gets the bowcaster. This works for any property, stock or added by the extension, so a mod no longer needs a duplicate unit, a second side, or a Lua `SetClassProperty` call just to change one value.

**Put the `@GameExt` line directly under the line it replaces.** It has to be in the same ODF, within about a dozen properties of the plain version, and the plain version has to actually be there. Inheriting the plain value from a `ClassParent` is not enough: write it out in the child as well.

Anything the property references is packed into the level automatically, exactly as if you had written the plain line, so there is nothing extra to add to a `.req`.

Sections work the way you would expect, since the override lands wherever you wrote it:

```
WEAPONSECTION      = 1
WeaponName         = "all_weap_inf_rifle"
WeaponName@GameExt = "all_weap_inf_bowcaster"
WeaponAmmo         = 6
```

Only slot 1 changes. The other slots are untouched, and no extra weapon is added.

**What it cannot do.** `ClassLabel` is decided before any property is read, so `ClassLabel@GameExt` does nothing: a GameExt-only class still needs its own ODF. Pointing an `@GameExt` line at such an ODF does not keep it away from a stock game either: the ODF is packed into the level all the same, and a game without the extension crashes loading it (see [Class Labels](classlabels/README.md)). `[InstanceProperties]` and world layer overrides are also not covered.

## New Class Labels

Whole new `ClassLabel` values, each with its own properties, are listed in **[Class Labels](classlabels/README.md)**, one page per label.

| ClassLabel | Description | Since |
|------------|-------------|-------|
| [`dualcannon`](classlabels/dualcannon.md) | A `cannon` with a second gun in the other hand, firing from each in turn. | 1.1.0 |

## Soldier Classes

Set on the concrete soldier class.

| Property | Value | Description | Since |
|----------|-------|-------------|-------|
| `FirstPersonAnimationBank` | bank name | Gives the class its own first person animation bank instead of the one global set every soldier shares. Partial banks are fine: any animation the bank does not have falls back to the default (`humanfp` / `droidekafp`), so a bank holding only a reload is valid. | 1.0.0 |
| `OverrideTexture3` | texture name | A third runtime texture override slot, on top of the stock `OverrideTexture` and `OverrideTexture2`. | 1.0.0 |
| `OverrideTexture4` | texture name | Fourth slot. | 1.0.0 |
| `OverrideTexture5` | texture name | Fifth slot. | 1.0.0 |
| `DisableProne` | `1` | The unit can never go prone, for the AI as well as the player. A double-tap of crouch just stands the soldier up again, and AI sent to a prone spot crouches there instead. | 1.1.0 |
| `DisableCrouch` | `1` | The unit can never crouch, for the AI as well as the player. The crouch key goes straight to prone instead, and pressing it again stands back up. With `DisableProne` also set, the crouch key does nothing. | 1.1.0 |
| `UseDirectionalRolls` | `1` | A roll to the side plays `diveleft` or `diveright` instead of a forward dive turned sideways, and follows the roll's real direction through turns as a forward roll does. The side is picked the way the stock `UseDirectionalJumps` picks a side jump: moving faster than 2 m/s, with more of the move sideways than forward. A side whose dive the bank lacks rolls as stock. Inherited through `ClassParent`. Names in [Animation Naming Conventions](#animation-naming-conventions). | 1.2.0 |
| `UseDirectionalJets` | `1` | While jetting (a jet jump or a jet hover) the legs blend toward `jetpack_hover_forward`, `_backward`, `_left` and `_right` by the way the soldier moves, more the faster it goes: all the way at the unit's top jet speed that way, `MaxSpeed` (forward) or `MaxStrafeSpeed` (backward and sideways) times its `ControlSpeed = "jet ..."` thrust or strafe, or the run speed alone where that factor is 0. The legs swing to a new way over a fraction of a second rather than snapping. A way without its animation keeps `jetpack_hover`. Lower body only. Inherited through `ClassParent`. Names in [Animation Naming Conventions](#animation-naming-conventions). | 1.2.0 |

**Notes on the override texture slots.** The model needs a material named `override_texture3`, `override_texture4` or `override_texture5` for the matching slot to do anything, following the same naming the stock two slots use. `OverrideTexture` must also be set or none of the extra slots apply. They are read off the concrete class only and are not inherited through `ClassParent`.

## Weapon Classes

| Property | Class | Value | Description | Since |
|----------|-------|-------|-------------|-------|
| `DisguiseModel` | `WeaponDisguise` | model name | Swaps the soldier to a specific model while disguised, instead of cloning the first enemy soldier the game finds. The model has to be loaded in memory. Set it to a single space (`" "`) to keep the soldier's own model and suppress the swap entirely. Leave it out for stock behaviour. | 1.0.0 |
| `HeldOrdnanceEffectBone` | `WeaponCannon` | bone name | Holds the projectile's `TrailEffect` at an animated soldier bone until release, then transfers it to the projectile. Inherits through `ClassParent`; set to `""` on a child to disable it. On by default, controlled by `[Fixes] HeldOrdnanceEffect`. | 1.1.0 |
| `AnimTexture1` | `WeaponMelee` | texture name | Second frame of an animated lightsaber blade. Set under the blade's `WeaponMelee` section. | 1.0.0 |
| `AnimTexture2` | `WeaponMelee` | texture name | Third frame. | 1.0.0 |
| `AnimTexture3` | `WeaponMelee` | texture name | Fourth frame. | 1.0.0 |

The blade's existing texture is frame one, so the three properties complete a four frame cycle. Ported from the Xbox version, where PC blades only ever used a single static texture.

```
[WeaponMelee]
AnimTexture1 = "blade_red_2"
AnimTexture2 = "blade_red_3"
AnimTexture3 = "blade_red_4"
```

**Held effects.** Set `HeldOrdnanceEffectBone` in the cannon weapon's `[Properties]` section. The weapon's `OrdnanceName` must point to a projectile with a `TrailEffect`, and the soldier's animation must contain the named bone. Spelling and case must match.

```
[Properties]
HeldOrdnanceEffectBone = "hp_bubble"
```

Any animated bone works. Leave the property out, or use a bone or projectile with no matching effect, and the weapon fires normally without a held effect. Cancelling fire or putting the weapon away removes it. If one shot creates several projectiles, the first takes the held effect and the rest create their normal trails. Launcher and grappling-hook classes do not use this property.

## Ordnance Classes

| Property | Class | Value | Description | Since |
|----------|-------|-------|-------------|-------|
| `CableTexture` | `OrdnanceGrapplingHook` | texture name | Re-skins the grappling hook's cable. Defaults to `com_bldg_minigun`. Modtools only. | 1.0.0 |

**One texture for the whole mod.** The cable texture is shared by every grappling hook rather than set per weapon, so with more than one grapple ODF loaded the last one parsed wins. The texture also has to be one the map already loads.

The engine's own `SoldierAnimation` property on the same class is worth knowing about here, since nothing else in the game reaches it: give it an animation name and the soldier plays that clip for the whole pull. Leave it out and the soldier keeps their normal pose.

## Vehicle Classes

| Property | Value | Description | Since |
|----------|-------|-------------|-------|
| `DisableBallMode` | `1` | Removes the droideka's roll outright, for the AI as well as the player, turning the chassis into a plain walking unit. The spawn screen follows suit and previews the unit standing and idling instead of curled into a ball. Set on a `walkerdroid` class. Off by default, and inherits through `ClassParent` like a stock property, so `DisableBallMode = 0` on a child restores the roll. | 1.0.0 |

## Animation Naming Conventions

Not properties, but ODF-adjacent: these are picked up by name out of an animation bank, with nothing to declare anywhere.

| Animation | Where | Description | Since |
|-----------|-------|-------------|-------|
| `<bank>_rifle_sprint` | first person soldier bank | Played while sprinting, in place of the run animation being sped up. Also `<bank>_bazooka_sprint` and `<bank>_tool_sprint`. Entirely optional: if the animation is not in the bank, nothing changes. | 1.0.0 |
| `<bank>_<weapon>_diveleft`, `<bank>_<weapon>_diveright` | soldier animation bank | The side rolls of a unit with `UseDirectionalRolls`, named like its `diveforward`: whole-body `human_rifle_diveleft_full`, or `_upper` and `_lower` halves. Found like any soldier animation: the unit's bank and then its parent banks, for its weapon and then the weapon's parents (a pistol uses the tool's and then the rifle's). A side without one keeps the stock roll. Make them at least three quarters of `human_rifle_diveforward`'s length, where every roll ends, or they hold their last frame until then. | 1.2.0 |
| `<bank>_<weapon>_jetpack_hover_forward`, `_backward`, `_left`, `_right` | soldier animation bank | The jet poses of a unit with `UseDirectionalJets`, each the pose while moving that way (`_left`: moving left, legs trailing to the right): `human_rifle_jetpack_hover_forward`, or with `_lower` or `_full`. Only the lower body is used (`bone_root`, `bone_pelvis` and the legs), blended over `jetpack_hover`. They play in step with it, at the same point of its cycle, so one of another length is stretched or squeezed to it; a one-frame pose works too. Found like the side dives. At most 32 joints. A way without one keeps the hover. | 1.2.0 |
| `boost` | flyer animation bank | Played automatically while boosting, blending in and out. Frame 0 should be the normal flying pose and the last frame the full boost pose. The length of the animation sets how long the blend takes, so add frames to slow it down and remove frames to speed it up. | 1.0.0 |
