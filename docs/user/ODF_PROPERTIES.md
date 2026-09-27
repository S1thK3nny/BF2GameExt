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
| `DisableProne` | `1` | The unit can never go prone, for the AI as well as the player. A double-tap of crouch just stands the soldier up again, and AI sent to a prone spot crouches there instead. | 1.1.0 |
| `DisableCrouch` | `1` | The unit can never crouch, for the AI as well as the player. The crouch key goes straight to prone instead, and pressing it again stands back up. With `DisableProne` also set, the crouch key does nothing. | 1.1.0 |
| `HitShake` | scale | Kicks your view when the unit takes damage, harder for a bigger hit. See [Camera shake](#camera-shake). | 1.2.0 |
| `LandShake` | scale | Dips your view when the unit lands a jump or fall. | 1.2.0 |
| `RollShake` | scale | Shakes your view as the unit starts a combat roll. Third person only. | 1.2.0 |
| `SprintShake` | scale | A light judder in step with the stride while the unit sprints. Third person only. | 1.2.0 |
| `BlastShake` | scale | How explosions shake your view while you play the unit. On without it; `0` turns it off. | 1.2.0 |

**Notes on the override texture slots.** The model needs a material named `override_texture3`, `override_texture4` or `override_texture5` for the matching slot to do anything, following the same naming the stock two slots use. `OverrideTexture` must also be set or none of the extra slots apply. They are read off the concrete class only and are not inherited through `ClassParent`.

## Weapon Classes

| Property | Class | Value | Description | Since |
|----------|-------|-------|-------------|-------|
| `DisguiseModel` | `WeaponDisguise` | model name | Swaps the soldier to a specific model while disguised, instead of cloning the first enemy soldier the game finds. The model has to be loaded in memory. Set it to a single space (`" "`) to keep the soldier's own model and suppress the swap entirely. Leave it out for stock behaviour. | 1.0.0 |
| `HeldOrdnanceEffectBone` | `WeaponCannon` | bone name | Holds the projectile's `TrailEffect` at an animated soldier bone until release, then transfers it to the projectile. Inherits through `ClassParent`; set to `""` on a child to disable it. On by default, controlled by `[Fixes] HeldOrdnanceEffect`. | 1.1.0 |
| `AnimTexture1` | `WeaponMelee` | texture name | Second frame of an animated lightsaber blade. Set under the blade's `WeaponMelee` section. | 1.0.0 |
| `AnimTexture2` | `WeaponMelee` | texture name | Third frame. | 1.0.0 |
| `AnimTexture3` | `WeaponMelee` | texture name | Fourth frame. | 1.0.0 |
| `FireShake` | any | scale | Kicks your view up and back each time you fire the weapon, on foot or in a vehicle or turret. See [Camera shake](#camera-shake). | 1.2.0 |

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
| `HitShake`, `BlastShake` | scale | As on soldiers, for any vehicle or turret: damage to it, and explosions while you are in it. See [Camera shake](#camera-shake). | 1.2.0 |
| `LandShake`, `RollShake`, `SprintShake` | scale | Flyers: touching down, a barrel roll or flip, and boosting. | 1.2.0 |
| `BrakeShake` | scale | Flyers: shakes your view while the flyer slows down, harder the faster it is losing speed. | 1.2.0 |

## Camera Shake

Camera shake moves only your own view, the one you are playing through, and only the picture: where you aim and where shots land are unchanged. Each shake is a property on a class, and every one takes the same detail properties after its name.

| Shake | Set on | Plays |
|-------|--------|-------|
| `FireShake` | any weapon | Each shot, as a kick up and back. |
| `HitShake` | any unit | When the unit takes damage, harder for a bigger hit. |
| `LandShake` | soldiers, flyers | Landing a jump or fall; a flyer touching down. |
| `RollShake` | soldiers, flyers | A combat roll, in third person only; a barrel roll or flip. |
| `SprintShake` | soldiers, flyers | While sprinting, in third person only; while boosting. |
| `BrakeShake` | flyers | While the flyer slows down, harder the faster it loses speed. |
| `BlastShake` | any unit | Explosions, walker deaths and flyer crashes, while `[CameraShake] Smooth=1`. |

Setting any property of a shake turns it on for that class. `BlastShake` is the exception: every unit has it, and a class uses it to reshape how explosions shake its view, or `BlastShake = "0"` to turn it off.

| Property | Value | Meaning |
|----------|-------|---------|
| `FireShake` | scale | Multiplies the whole shake. `0` turns it off, for example on a child of a class that shakes. Default `1`. |
| `FireShakePitch` | degrees, or `"min max"` | How far the view turns up; negative turns it down. |
| `FireShakeYaw` | degrees, or `"min max"` | How far it turns right; negative turns it left. |
| `FireShakeRoll` | degrees, or `"min max"` | How far it tilts clockwise; negative tilts it anticlockwise. |
| `FireShakePush` | metres, or `"min max"` | How far the camera moves back along the view; negative moves it forward. |
| `FireShakeLength` | seconds | How long one shake lasts. |
| `FireShakeRise` | 0 to 1 | The share of the length spent reaching the peak; the rest eases back. |
| `FireShakeRate` | per second | `0` makes one push in one direction. Above that the view swings back and forth this many times a second, inside the same rise and fall. |
| `FireShakeLimit` | 1 or more | How far repeats can pile up when they come faster than a shake fades, in shakes' worth. At `1` a repeat takes over from wherever the view is and never goes past one shake; at `2` rapid fire levels off at about two. |

The same nine work for every shake: `HitShakePitch`, `LandShakeLength`, `BlastShakeRoll` and so on. A `"min max"` range is picked from afresh each time the shake plays, so every shot kicks a little differently. `SprintShake`, `BrakeShake` and `BlastShake` play for as long as something lasts rather than once: for them the angles and push are how far the view swings each way (a range uses its larger end), `Rate` is how fast it swings, and `Length`, `Rise` and `Limit` are not used. `BlastShake`'s angles and push are per unit of the explosion's stock `Shake`, and its `Limit` is where a blast levels off, in units of `Shake`, so a detpack is not four times a grenade.

Defaults, for anything a class leaves out (degrees, metres, seconds):

| Shake | Pitch | Yaw | Roll | Push | Length | Rise | Rate | Limit |
|-------|-------|-----|------|------|--------|------|------|-------|
| Fire | `0.25 0.4` | `-0.12 0.12` | `0` | `0.015` | `0.22` | `0.25` | `0` | `1` |
| Hit | `2 4` | `-2 2` | `0` | `0` | `0.25` | `0.3` | `0` | `1` |
| Land, soldier | `-2.25` | `0.92` | `0.44` | `0` | `0.67` | `0.15` | `0` | `1` |
| Land, flyer | `-1.5` | `0` | `-0.5 0.5` | `0` | `0.5` | `0.12` | `0` | `1` |
| Roll, soldier | `1` | `1` | `1.5` | `0` | `0.4` | `0.1` | `5` | `1` |
| Roll, flyer | `1` | `1` | `2` | `0` | `0.6` | `0.1` | `5` | `1` |
| Sprint, soldier | `0.16` | `0.06` | `0` | `0` | | | `4` | |
| Sprint, flyer | `0.3` | `0.3` | `0.2` | `0` | | | `16` | |
| Brake | `0.4` | `0.4` | `0.2` | `0` | | | `11` | |
| Blast, per unit of `Shake` | `0` | `0` | `6` | `0.03` | | | `3` | `2.5` |

A weapon with a heavier kick than the default, and a soldier class that dips on landing:

```
[Properties]
FireShake      = "1.0"
FireShakePitch = "0.5 0.8"
FireShakePush  = "0.03"
```

```
[Properties]
LandShake       = "1.0"
LandShakeLength = "0.5"
```

- **Sprinting** judders rather than sways: the view flips between its limits in step with the stride, a stride lasting 2.8 / `Rate` seconds.
- **Hits** go by the unit's health, so they work the same on a multiplayer client. A hit of a tenth of the unit's maximum health or more gives the full shake, smaller ones down to a quarter of it, and slow drains such as a hero's never shake. Damage taken by shields alone does not count.
- **Landings** count after at least a quarter of a second in the air.
- **Views:** rolls and sprinting shake in third person only, since first person has its own camera motion for both. Everything else shakes in every view.
- **Several at once:** a shot, hit, landing or roll that comes before the last has faded takes over from wherever the view is, without a jump, and `Limit` decides how far they pile up. A blast follows the strongest explosion running rather than adding them together, and shrinks while you zoom in.
- **The reticle holds still:** the shake moves the picture under it, and the reticle stays where the unshaken view would put it, so it does not bounce with each shot.
- **Inheritance:** everything here inherits through `ClassParent`, so a value on a common parent covers every unit or weapon built on it.
- **INI:** in `[CameraShake]`, `Enabled=0` turns off all of these but the blast, which then keeps its defaults. `Smooth=0` puts back the stock shake, `Strength` scales everything, and `FireStrength` through `BlastStrength` scale one shake each.

## Animation Naming Conventions

Not properties, but ODF-adjacent: these are picked up by name out of an animation bank, with nothing to declare anywhere.

| Animation | Where | Description | Since |
|-----------|-------|-------------|-------|
| `<bank>_rifle_sprint` | first person soldier bank | Played while sprinting, in place of the run animation being sped up. Also `<bank>_bazooka_sprint` and `<bank>_tool_sprint`. Entirely optional: if the animation is not in the bank, nothing changes. | 1.0.0 |
| `boost` | flyer animation bank | Played automatically while boosting, blending in and out. Frame 0 should be the normal flying pose and the last frame the full boost pose. The length of the animation sets how long the blend takes, so add frames to slow it down and remove frames to speed it up. | 1.0.0 |
