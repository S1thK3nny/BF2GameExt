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
| `HitShake` | scale | Kicks your view when the unit takes damage, harder for a bigger hit. See [Camera shake](#camera-shake). | 1.2.0 |
| `LandShake` | scale | Dips your view when the unit lands a jump or fall. | 1.2.0 |
| `RollShake` | scale | Eases your view back and down through a combat roll, then settles. Third person only. | 1.2.0 |
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
| `SwingShake`, `StrikeShake`, `SwingBlockedShake`, `BlockShake`, `DeflectShake` | `WeaponMelee` | scale | Shake your view as you swing, as a swing lands, when something blocks your swing, and when you block a strike or deflect a bolt. See [Camera shake](#camera-shake). | 1.2.0 |

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
| `BoostShake`, `TurnShake`, `BrakeShake`, `CollisionShake` | scale | Flyers: turbulence while the flyer speeds up (by default, while it boosts), holds a hard turn or slows down, and a jolt when it bumps into something. Each takes a `Threshold` for when it plays. See [Flyer thresholds](#flyer-thresholds). | 1.2.0 |
| `TrickRollShake`, `TrickFlipShake`, `TakeoffShake`, `LandingShake` | scale | Flyers: a barrel roll, a flip, lifting off and touching down. | 1.2.0 |
| `StepShake`, `JumpShake`, `LandShake`, `TurnShake`, `BoostShake` | scale | Walkers: a thump each time a foot lands, rolled toward that foot; a jump starting; landing a jump or fall; turning on the spot; boosting. See [Walker shakes](#walker-shakes). | 1.2.0 |

## Camera Shake

Camera shake moves only your own view, the one you are playing through, and only the picture: where you aim and where shots land are unchanged. Each shake is a property on a class, and every one takes the same detail properties after its name.

| Shake | Set on | Plays |
|-------|--------|-------|
| `FireShake` | any weapon | Each shot, as a kick up and back. A shotgun's pellets, or a salvo with no `SalvoDelay`, kick once for the whole shot. |
| `SwingShake` | melee weapons | Each swing: every attack of a combo. On a melee weapon it plays instead of `FireShake`. |
| `StrikeShake` | melee weapons | A swing landing on something that does not block it. |
| `SwingBlockedShake` | melee weapons | A swing that something blocks. |
| `BlockShake` | melee weapons | Blocking a melee strike. |
| `DeflectShake` | melee weapons | Deflecting a blaster bolt or beam. |
| `HitShake` | any unit | When the unit takes damage, harder for a bigger hit. |
| `LandShake` | soldiers, walkers | Landing a jump or fall. On a walker, harder for a faster drop. |
| `RollShake` | soldiers | A combat roll, in third person only. |
| `SprintShake` | soldiers | While sprinting, in third person only. |
| `StepShake` | walkers | Each time a foot lands, rolled toward that foot. |
| `JumpShake` | walkers | A jump starting. |
| `BoostShake` | flyers, walkers | While a flyer goes fast (by default, while it boosts), or while a walker boosts. |
| `TurnShake` | flyers, walkers | While a flyer is held in a hard turn, after a couple of seconds, or while a walker turns on the spot. |
| `BrakeShake` | flyers | While you brake: slowing down with the brake or reverse input held. |
| `CollisionShake` | flyers | When the flyer bumps into something, harder for a harder bump. |
| `TrickRollShake` | flyers | A barrel roll. |
| `TrickFlipShake` | flyers | A flip. |
| `TakeoffShake` | flyers | Lifting off. |
| `LandingShake` | flyers | Touching down. |
| `BlastShake` | any unit | Explosions, walker deaths and flyer crashes. |

Setting any property of a shake turns it on for that class. `BlastShake` is the exception: every unit has it, and a class uses it to reshape how explosions shake its view, or `BlastShake = "0"` to turn it off.

| Property | Value | Meaning |
|----------|-------|---------|
| `FireShake` | scale | Multiplies the whole shake. `0` turns it off, for example on a child of a class that shakes. Default `1`. |
| `FireShakePitch` | degrees, or `"min max"` | How far the view turns up; negative turns it down. |
| `FireShakeYaw` | degrees, or `"min max"` | How far it turns right; negative turns it left. |
| `FireShakeRoll` | degrees, or `"min max"` | How far it tilts clockwise; negative tilts it anticlockwise. |
| `FireShakePush` | metres, or `"min max"` | How far the camera moves back along the view; negative moves it forward. Third person only. |
| `FireShakeLength` | seconds | How long one shake lasts. On a shake that lasts (sprint, boost, turn, brake), how long it takes to fade in and out, the two together. |
| `FireShakeRise` | 0 to 1 | The share of the length spent reaching the peak; the rest eases back. On a shake that lasts, the share of `Length` spent fading in; the rest is how long it takes to fade out. |
| `FireShakeRate` | per second | How many times a second the view swings back and forth. On a one-off shake `0` makes one push in one direction; a shake that lasts needs a rate above `0` to move at all. See [How Rate works](#how-rate-works). |
| `FireShakeLimit` | 1 to 8 | How many can run at once. Above `1`, each shot (or hit, landing and so on) is a shake of its own and those still running add together, so rapid fire builds up a little and every shot still kicks; when that many are running, the oldest fades out to make room. At `1` a repeat takes over from wherever the view is instead, so it never goes past one shake. A whole number; fractions round down. Default `8` for `FireShake`, `1` for the rest. |
| `BoostShakeThreshold` | a number, or `"from to"` | When the shake plays, in what that shake measures. One value: in full at or past it. Two: it starts at the first and grows to full at the second. Used by a flyer's `BoostShake`, `TurnShake`, `BrakeShake` and `CollisionShake` (see [Flyer thresholds](#flyer-thresholds)) and a walker's `StepShake`, `LandShake` and `BoostShake` (see [Walker shakes](#walker-shakes)). |
| `BoostShakeSteady` | 0 to 1 | `BoostShake` and `BrakeShake`: the share of the shake left once the flyer or walker reaches the speed it is heading for. |
| `FireShakePushOnce` | `1` or `0` | With a `Rate`, the push still goes out and back once, following the rise and fall, while the angles swing; on a shake that lasts, the camera eases back and holds there. `0` makes the push swing with the angles, which moves the camera in and out. Default `1`, except `BlastShake`, whose camera sways by default. |
| `StrikeShakeTeammates` | `1` or `0` | `StrikeShake`: whether a swing that lands on teammates counts. At `0` it shakes only when the swing lands on something that is not a teammate. Default `1`. |

The same thirteen work for every shake: `HitShakePitch`, `LandingShakeLength`, `BlastShakeRoll` and so on, and each shake uses the ones that mean something to it. A `"min max"` angle or push is picked afresh each time the shake plays, so every shot kicks a little differently. `SprintShake`, `BoostShake`, `TurnShake`, `BrakeShake` and `BlastShake` play for as long as something lasts rather than once: for them the angles and push are how far the view swings each way (a range uses its larger end), `Rate` is how fast it swings, `Length` and `Rise` set how quickly it fades in and out, and `Limit` is not used. `BlastShake` follows each explosion's own fade instead, so `Length` and `Rise` do nothing there; its angles and push are per unit of the explosion's stock `Shake`, and its `Limit` is where a blast levels off, in units of `Shake`, so a detpack is not four times a grenade.

Defaults, for anything a class leaves out (degrees, metres, seconds):

| Shake | Pitch | Yaw | Roll | Push | Length | Rise | Rate | Limit |
|-------|-------|-----|------|------|--------|------|------|-------|
| Fire | `0.25 0.4` | `-0.12 0.12` | `0` | `0.06` | `0.22` | `0.25` | `0` | `8` |
| Hit | `2 4` | `-2 2` | `0` | `0` | `0.25` | `0.3` | `0` | `1` |
| Land | `-2.25` | `0.92` | `0.44` | `0` | `0.67` | `0.15` | `0` | `1` |
| Roll | `-5` | `0` | `0` | `0.7` | `1` | `0.5` | `0` | `1` |
| Sprint | `0.16` | `0.06` | `0` | `0` | `0.667` | `0.5` | `4` | |
| Boost | `0.5` | `0.5` | `0.5` | `0` | `1` | `0.5` | `11` | |
| Turn | `0.5` | `0.5` | `0.5` | `0` | `1` | `0.5` | `11` | |
| Brake | `0.4` | `0.4` | `0.2` | `0` | `1` | `0.25` | `11` | |
| Collision | `1.5` | `1.5` | `1.5` | `0` | `0.3` | `0` | `11` | `1` |
| TrickRoll | `1` | `1` | `2` | `0` | `0.6` | `0.1` | `5` | `1` |
| TrickFlip | `2` | `1` | `1` | `0` | `0.8` | `0.1` | `5` | `1` |
| Takeoff | `0.3` | `0` | `-0.2 0.2` | `0` | `1` | `0.3` | `8` | `1` |
| Landing | `-0.75` | `0` | `-0.25 0.25` | `0` | `0.5` | `0.12` | `0` | `1` |
| Swing | `0.15 0.25` | `-0.2 0.2` | `-0.3 0.3` | `0` | `0.3` | `0.3` | `0` | `1` |
| Strike | `-0.6 -0.4` | `-0.4 0.4` | `-0.5 0.5` | `0.04` | `0.25` | `0.12` | `0` | `1` |
| SwingBlocked | `0.6 0.8` | `-0.6 0.6` | `-0.4 0.4` | `0.06` | `0.35` | `0.1` | `0` | `1` |
| Block | `0.4 0.6` | `-0.6 0.6` | `-0.6 0.6` | `0.05` | `0.3` | `0.1` | `0` | `1` |
| Deflect | `0.2 0.3` | `-0.3 0.3` | `-0.3 0.3` | `0.02` | `0.2` | `0.15` | `0` | `1` |
| Step (walker) | `-0.4 -0.25` | `-0.1 0.1` | `0.2 0.35`, toward the foot | `0` | `0.35` | `0.15` | `0` | `1` |
| Jump (walker) | `0.6 0.9` | `-0.2 0.2` | `-0.3 0.3` | `0` | `0.4` | `0.25` | `0` | `1` |
| Land (walker) | `-1.5 -1` | `-0.3 0.3` | `-0.6 0.6` | `0` | `0.5` | `0.12` | `0` | `1` |
| Turn (walker) | `0.15` | `0.1` | `0.4` | `0` | `1` | `0.5` | `1.5` | |
| Boost (walker) | `0.3` | `0.2` | `0.3` | `0` | `1` | `0.3` | `2` | |
| Blast, per unit of `Shake` | `0` | `0` | `6` | `0.08` | | | `3` | `2.5` |

The flyer, walker and melee defaults are first guesses, kept small for take-off and landing, until they are tuned in play.

A weapon with a heavier kick than the default, and a soldier class that dips on landing:

```
[Properties]
FireShake      = "1.0"
FireShakePitch = "0.5 0.8"
FireShakePush  = "0.12"
```

```
[Properties]
LandShake       = "1.0"
LandShakeLength = "0.5"
```

- **Sprinting** judders rather than sways: the view flips between its limits in step with the stride, a stride lasting 2.8 / `Rate` seconds.
- **Boost, turns and bumps** share one turbulence: pitch swings at `Rate`, yaw at 1.3 times it and roll at 0.7 times, so 11, 14.3 and 7.7 times a second at the default `11`. Braking sways on its own softer noise.
- **Hits** go by the unit's health, so they work the same on a multiplayer client. A hit of a tenth of the unit's maximum health or more gives the full shake, smaller ones down to a quarter of it, and slow drains such as a hero's never shake. Damage taken by shields alone does not count.
- **Landings** count after at least a quarter of a second in the air.
- **Melee:** BF2 decides what lands and what is blocked; the shakes follow. A swing that lands plays `StrikeShake` once, however many it lands on at the same moment, and one that something blocks plays `SwingBlockedShake`. You block a strike when your combo is in a state with a `Deflect`, the blade comes within its `DeflectAngle` and you have the energy; deflecting a bolt or beam works the same way and plays `DeflectShake`. On a melee weapon, `FireShake` does nothing; use `SwingShake`.
- **Views:** in first person, cockpits included, a shake turns the view but never moves it, so `Push` plays in third person only. A soldier's rolls and sprinting don't shake first person at all, since it has its own camera motion for both. Everything else, flyers included, shakes in every view.
- **Several at once:** shots add together, up to eight running at once, so a shot that lands before the last has faded still kicks on top of it. With a `Length` longer than the time between shots, rapid fire therefore pushes further than one shot does: about 1.2 times at a `Length` of 0.3 on a weapon that fires every 0.18 s, 1.7 times at 0.5. A hit, landing or roll that comes before the last has faded takes over from wherever the view is, without a jump. `Limit` sets which way each shake goes. A blast follows the strongest explosion running rather than adding them together, and shrinks while you zoom in.
- **The reticle holds still:** the shake moves the picture under it, and the reticle stays where the unshaken view would put it, so it does not bounce with each shot.
- **No caps:** every value is used as given. A `Push` of 5 moves the camera five metres and a `Pitch` of 30 turns the view thirty degrees, so keep values to what looks right.
- **The same at any frame rate:** a shake sits on top of the game's own camera and never feeds into how that camera follows the unit, so a `Push` of 0.5 is half a metre whether the game runs at 30 or 144 frames a second.
- **Inheritance:** everything here inherits through `ClassParent`, so a value on a common parent covers every unit or weapon built on it.
- **No INI settings:** camera shake is always on, and what plays is what the ODFs set. The stock shake from explosions is always drawn as the smooth blast.

### How Rate works

`Rate` is how often the view swings: full swings there and back each second. The angles and push say how far each swing goes.

- **One-off shakes** (Fire, Hit, Land, Roll, Collision, TrickRoll, TrickFlip, Takeoff, Landing, the walker's Step and Jump, and the melee Swing, Strike, SwingBlocked, Block and Deflect): `Length` and `Rise` set how big the shake is from moment to moment, rising to its peak and easing away, and `Rate` sets how often it swings inside that. At `0` it is one push in one direction and back. Above `0` it swings about `Rate` x `Length` times before it fades: `TrickRollShakeRate = "5"` over a `0.6` second length is three swings. Each shake starts at a random point in its swing, so repeats never look quite alike.
- **Shakes that last** (Sprint, Boost, Turn, Brake, Blast) only swing, so at `0` they do nothing. Their size follows whatever triggers them, fading in and out as `Length` and `Rise` set, and `Rate` is how fast they swing the whole time.
- **What swings** differs by shake:

| Shake | How it swings at a given `Rate` |
|-------|---------------------------------|
| Boost, Turn, Collision (flyers and walkers alike) | One smooth swing per axis: pitch at `Rate`, yaw at 1.3 times it, roll at 0.7 times and push at 1.1 times. At `11`, that is 11, 14.3 and 7.7 swings a second. |
| Brake, and every other one-off | A rougher swing: mostly at `Rate`, with a half-size ripple at nearly twice it and a quarter-size one at three times it, each axis out of step with the others. |
| Sprint | A judder: pitch flips between its limits about `Rate` times a second, and yaw follows a stride lasting 2.8 / `Rate` seconds. |
| Blast | Three slow swings at 1.14, 0.89 and 0.96 times `Rate`. |

- **The push** does not swing unless the shake's `PushOnce` is `0`: it goes out and back once (or holds, on a shake that lasts) while the angles swing.
- **Picking a rate:** the game draws 30 to 60 frames a second, so a swing faster than about 10 a second only gets a few frames and reads as jitter rather than movement. Roughly, 2 to 4 is a slow sway or buffet, 5 to 8 a rumble and 10 or more a buzz. The rougher swings carry ripples up to three times their `Rate`, so they need a lower one than the smooth ones for the same feel. The speed is the same at any frame rate; only how smooth it looks changes.

### Flyer thresholds

`Threshold` decides when a flyer's boost, turn, brake and collision shakes play. Each of the four measures its own thing:

| Shake | `Threshold` measures | Default |
|-------|----------------------|---------|
| `BoostShake` | The flyer's speed, in m/s: a number, or the class's own `MinSpeed`, `MidSpeed`, `MaxSpeed` or `BoostSpeed`. | `"MaxSpeed BoostSpeed"` |
| `TurnShake` | Seconds of hard turning: the nose swinging round at 60% or more of the class's faster `TurnRate` or `PitchRate`. | `"2 3"` |
| `BrakeShake` | The flyer's speed while it brakes, as for `BoostShake`. One value means at or below it. | `"MinSpeed MaxSpeed"` |
| `CollisionShake` | How much the bump changes the flyer's velocity, in m/s: a head-on stop from 40 m/s is 40. | `"0 40"` |

- **One value** is a switch: the shake plays in full at or past it, which for `BrakeShake` means at or below it. `BrakeShakeThreshold = "MidSpeed"` shakes while the flyer slows below its cruising speed.
- **Two values** are a ramp: nothing before the first, full size at the second, growing in between. Written high to low it runs the other way, so `BoostShakeThreshold = "MidSpeed MinSpeed"` grows as the flyer slows toward a stall.
- **Speed names** are read from each class's own ODF, so a value on a common parent suits fast and slow flyers alike. They always mean the ODF's numbers, landing region or not. `BoostSpeed` on a class that has none never plays: give such a class a number or another name.
- **Speeding up and slowing down** follow the throttle, not the speedometer. BF2 steers each flyer toward a speed: its `MidSpeed` with the throttle at rest, up to `MaxSpeed` held forward, down to `MinSpeed` held back, or its `BoostSpeed` while it boosts. Inside a landing region BF2 caps those speeds, `MaxSpeed` at 60, `MidSpeed` at 20 and `MinSpeed` at a fifth of itself (10 at most), and the shakes follow the same caps, so an approach to land counts as slowing down. The flyer is speeding up while it is more than a twentieth of its speed range below that speed, and slowing down while it is that far above it, so steering, which only dips the speed a little, never counts as either. Speeding up also needs that speed to have been raised, by pushing the throttle forward or boosting. Hard turns cost a little speed, and so does rolling with the throttle held when `[Fixes] FlyerRollThrottleFix` is off (stock BF2 reads the throttle and the roll as one stick and eases both off); getting that back does not replay the boost shake. Nor does the climb to cruising speed after take-off. Braking is slowing down with the brake or reverse input held back: easing back to `MidSpeed` after letting go of the throttle, or into a landing region, does not count.
- **`BoostShakeSteady` and `BrakeShakeSteady`:** the shake is at full strength while the flyer is still speeding up (boost) or braking (brake), then settles to this share once it gets there: `0.25` by default for the boost, `0` for the brake. `0` shakes only on the way, `1` keeps the whole shake afterwards.
- **Turning** is measured on the flyer itself, so a stick and a mouse count the same: it counts while the nose swings round at 60% or more of the faster of the class's `TurnRate` and `PitchRate`, and holds through a lull of up to a quarter of a second, such as a mouse between two pushes. Rolling in place and tricks do not count. The shake also grows with the flyer's speed, up to its `MaxSpeed`.
- **Fading in and out:** `Threshold` decides how strongly the shake should play at each moment; `Length` and `Rise` decide how quickly it follows. It fades in over `Rise` x `Length` seconds and out over the rest, covering most of the way in that time and easing into the end. A single-value threshold jumps straight from nothing to full, so the fade is all that softens it; a pair already ramps, so a short fade barely shows and a long one mostly adds lag. With `Length "1"` and `Rise "0.3"`, a brake shake builds over about 0.3 s when you hit the brake and dies away over about 0.7 s when you let go.
- **Bumps:** a bump at the first value still gives 30% of the shake, so a graze registers. A bump that plays `CollisionShake` no longer reaches the blast, and the damage it does is not shaken for again as a hit.

A flyer without a boost that shakes while it throttles up to top speed, and one whose brake shake, while you hold the brake, builds as it slows from cruise to its slowest:

```
[Properties]
BoostShake          = "1.0"
BoostShakeThreshold = "MidSpeed MaxSpeed"
BoostShakeSteady    = "0"
```

```
[Properties]
BrakeShake          = "1.0"
BrakeShakeThreshold = "MidSpeed MinSpeed"
```

### Every flyer shake property

Each flyer shake with every property it uses, at its defaults. Setting any line of a group turns that shake on for the class, so copy only the groups you want, or set a group's scale to `0`. `HitShake` and `BlastShake` work on any unit; `BlastShake` is on without any of its lines. `FireShake` goes on the weapon's ODF instead.

```
[Properties]
BoostShake              = "1"
BoostShakePitch         = "0.5"
BoostShakeYaw           = "0.5"
BoostShakeRoll          = "0.5"
BoostShakePush          = "0"
BoostShakeLength        = "1"
BoostShakeRise          = "0.5"
BoostShakeRate          = "11"
BoostShakeThreshold     = "MaxSpeed BoostSpeed"
BoostShakeSteady        = "0.25"
BoostShakePushOnce      = "1"

TurnShake               = "1"
TurnShakePitch          = "0.5"
TurnShakeYaw            = "0.5"
TurnShakeRoll           = "0.5"
TurnShakePush           = "0"
TurnShakeLength         = "1"
TurnShakeRise           = "0.5"
TurnShakeRate           = "11"
TurnShakeThreshold      = "2 3"
TurnShakePushOnce       = "1"

BrakeShake              = "1"
BrakeShakePitch         = "0.4"
BrakeShakeYaw           = "0.4"
BrakeShakeRoll          = "0.2"
BrakeShakePush          = "0"
BrakeShakeLength        = "1"
BrakeShakeRise          = "0.25"
BrakeShakeRate          = "11"
BrakeShakeThreshold     = "MinSpeed MaxSpeed"
BrakeShakeSteady        = "0"
BrakeShakePushOnce      = "1"

CollisionShake          = "1"
CollisionShakePitch     = "1.5"
CollisionShakeYaw       = "1.5"
CollisionShakeRoll      = "1.5"
CollisionShakePush      = "0"
CollisionShakeLength    = "0.3"
CollisionShakeRise      = "0"
CollisionShakeRate      = "11"
CollisionShakeLimit     = "1"
CollisionShakeThreshold = "0 40"
CollisionShakePushOnce  = "1"

TrickRollShake          = "1"
TrickRollShakePitch     = "1"
TrickRollShakeYaw       = "1"
TrickRollShakeRoll      = "2"
TrickRollShakePush      = "0"
TrickRollShakeLength    = "0.6"
TrickRollShakeRise      = "0.1"
TrickRollShakeRate      = "5"
TrickRollShakeLimit     = "1"
TrickRollShakePushOnce  = "1"

TrickFlipShake          = "1"
TrickFlipShakePitch     = "2"
TrickFlipShakeYaw       = "1"
TrickFlipShakeRoll      = "1"
TrickFlipShakePush      = "0"
TrickFlipShakeLength    = "0.8"
TrickFlipShakeRise      = "0.1"
TrickFlipShakeRate      = "5"
TrickFlipShakeLimit     = "1"
TrickFlipShakePushOnce  = "1"

TakeoffShake            = "1"
TakeoffShakePitch       = "0.3"
TakeoffShakeYaw         = "0"
TakeoffShakeRoll        = "-0.2 0.2"
TakeoffShakePush        = "0"
TakeoffShakeLength      = "1"
TakeoffShakeRise        = "0.3"
TakeoffShakeRate        = "8"
TakeoffShakeLimit       = "1"
TakeoffShakePushOnce    = "1"

LandingShake            = "1"
LandingShakePitch       = "-0.75"
LandingShakeYaw         = "0"
LandingShakeRoll        = "-0.25 0.25"
LandingShakePush        = "0"
LandingShakeLength      = "0.5"
LandingShakeRise        = "0.12"
LandingShakeRate        = "0"
LandingShakeLimit       = "1"
LandingShakePushOnce    = "1"

HitShake                = "1"
HitShakePitch           = "2 4"
HitShakeYaw             = "-2 2"
HitShakeRoll            = "0"
HitShakePush            = "0"
HitShakeLength          = "0.25"
HitShakeRise            = "0.3"
HitShakeRate            = "0"
HitShakeLimit           = "1"
HitShakePushOnce        = "1"

BlastShake              = "1"
BlastShakePitch         = "0"
BlastShakeYaw           = "0"
BlastShakeRoll          = "6"
BlastShakePush          = "0.08"
BlastShakeRate          = "3"
BlastShakeLimit         = "2.5"
BlastShakePushOnce      = "0"
```

On the flyer's weapon:

```
[Properties]
FireShake               = "1"
FireShakePitch          = "0.25 0.4"
FireShakeYaw            = "-0.12 0.12"
FireShakeRoll           = "0"
FireShakePush           = "0.06"
FireShakeLength         = "0.22"
FireShakeRise           = "0.25"
FireShakeRate           = "0"
FireShakeLimit          = "8"
FireShakePushOnce       = "1"
```

### Walker shakes

Walkers (`walker` and `commandwalker` classes) take five shakes. Each follows what BF2 itself records, so a step lands at the moment the stomp effect and footstep sound play:

| Shake | Plays | `Threshold` measures | Default `Threshold` |
|-------|-------|----------------------|---------------------|
| `StepShake` | Each time a foot lands. Feet landing in the same update count as one step. | The walker's speed along the ground, in m/s: a number, `MaxSpeed` or `BoostSpeed`. Graded like a bump: nothing below the first value, 30% at it, full at the second. | none: every step in full |
| `JumpShake` | A jump starting. Only a class with a jump (`JumpHeight`) ever jumps. | not used | |
| `LandShake` | Coming down after at least half a second off the ground, from a jump or off a ledge. | How fast it came down, in m/s, graded like a bump. | `"2 10"` |
| `TurnShake` | While the walker turns on the spot. Its feet re-planting still play `StepShake`. | not used | |
| `BoostShake` | While the walker boosts, which only a class with a `BoostSpeed` above its `MaxSpeed` can. In full while it is still speeding up, then `Steady` of that. | The walker's speed, as for `StepShake`. | `"MaxSpeed BoostSpeed"` |

- **Which way a step rolls:** `StepShakeRoll` sets how far; the shake tips the view toward the foot that landed, left for a left foot and right for a right one. BF2 numbers the feet in the order the ODF gives `TerrainLeft` and `TerrainRight`, left first in every stock walker; a class that lists a right foot first rolls the other way. A step landing both sides at once picks a side at random.
- **Steps that never come:** `StepShake` plays only for the steps BF2 counts, the same ones that play the stomp effect and footstep sound.
  - Each `TerrainLeft` and `TerrainRight` must name a collision primitive exactly as the model spells it (`p_-tbv-sphere_foot1`, not `p_-tbv_sphere_foot1`). BF2 silently drops a foot whose name the model lacks, and a walker with none never lands a step.
  - With `StompDetectionType = "1"`, a foot counts a step only when it comes down fast enough. Stock BF2 asks for more than 0.1 m in a single frame, so the higher the frame rate, the fewer steps count. `[Fixes] WalkerStompFix` (on by default) makes that 3 m/s at any frame rate. The default, type 0, does not depend on the frame rate.
  - `[Diagnostic] WalkerFootDiag=1` logs each foot's steps, how fast it came down and whether the step counted.
- **Speeds:** a walker has `MaxSpeed` and `BoostSpeed`. It has no `MinSpeed`, which reads as `0`, and no cruising speed, so `MidSpeed` reads as `MaxSpeed`.
- **Steady:** a walker's `BoostShakeSteady` defaults to `1`, so the sway lasts for the whole boost.
- **Dying walkers** stop stepping and swaying; the stock death shake plays through `BlastShake` as before.

Each walker shake with every property it uses, at its defaults:

```
[Properties]
StepShake               = "1"
StepShakePitch          = "-0.4 -0.25"
StepShakeYaw            = "-0.1 0.1"
StepShakeRoll           = "0.2 0.35"
StepShakePush           = "0"
StepShakeLength         = "0.35"
StepShakeRise           = "0.15"
StepShakeRate           = "0"
StepShakeLimit          = "1"

JumpShake               = "1"
JumpShakePitch          = "0.6 0.9"
JumpShakeYaw            = "-0.2 0.2"
JumpShakeRoll           = "-0.3 0.3"
JumpShakeLength         = "0.4"
JumpShakeRise           = "0.25"

LandShake               = "1"
LandShakePitch          = "-1.5 -1"
LandShakeYaw            = "-0.3 0.3"
LandShakeRoll           = "-0.6 0.6"
LandShakeLength         = "0.5"
LandShakeRise           = "0.12"
LandShakeThreshold      = "2 10"

TurnShake               = "1"
TurnShakePitch          = "0.15"
TurnShakeYaw            = "0.1"
TurnShakeRoll           = "0.4"
TurnShakeLength         = "1"
TurnShakeRise           = "0.5"
TurnShakeRate           = "1.5"

BoostShake              = "1"
BoostShakePitch         = "0.3"
BoostShakeYaw           = "0.2"
BoostShakeRoll          = "0.3"
BoostShakeLength        = "1"
BoostShakeRise          = "0.3"
BoostShakeRate          = "2"
BoostShakeThreshold     = "MaxSpeed BoostSpeed"
BoostShakeSteady        = "1"
```

## Animation Naming Conventions

Not properties, but ODF-adjacent: these are picked up by name out of an animation bank, with nothing to declare anywhere.

| Animation | Where | Description | Since |
|-----------|-------|-------------|-------|
| `<bank>_rifle_sprint` | first person soldier bank | Played while sprinting, in place of the run animation being sped up. Also `<bank>_bazooka_sprint` and `<bank>_tool_sprint`. Entirely optional: if the animation is not in the bank, nothing changes. | 1.0.0 |
| `boost` | flyer animation bank | Played automatically while boosting, blending in and out. Frame 0 should be the normal flying pose and the last frame the full boost pose. The length of the animation sets how long the blend takes, so add frames to slow it down and remove frames to speed it up. | 1.0.0 |
