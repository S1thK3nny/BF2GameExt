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

**What it cannot do.** `ClassLabel` is decided before any property is read, so `ClassLabel@GameExt` does nothing: a GameExt-only class still needs its own ODF. `[InstanceProperties]` and world layer overrides are also not covered.

## Soldier Classes

Set on the concrete soldier class.

| Property | Value | Description | Since |
|----------|-------|-------------|-------|
| `FirstPersonAnimationBank` | bank name | Gives the class its own first person animation bank instead of the one global set every soldier shares. Partial banks are fine: any animation the bank does not have falls back to the default (`humanfp` / `droidekafp`), so a bank holding only a reload is valid. | 1.0.0 |
| `OverrideTexture3` | texture name | A third runtime texture override slot, on top of the stock `OverrideTexture` and `OverrideTexture2`. | 1.0.0 |
| `OverrideTexture4` | texture name | Fourth slot. | 1.0.0 |
| `OverrideTexture5` | texture name | Fifth slot. | 1.0.0 |

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
| `boost` | flyer animation bank | Played automatically while boosting, blending in and out. Frame 0 should be the normal flying pose and the last frame the full boost pose. The length of the animation sets how long the blend takes, so add frames to slow it down and remove frames to speed it up. | 1.0.0 |
