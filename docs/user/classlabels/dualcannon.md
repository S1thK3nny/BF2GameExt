# dualcannon

*Since 1.1.0.*

A weapon class for dual pistols and other paired guns. It is a `cannon` in every respect, plus a second gun drawn in the soldier's other hand, and its shots alternate between the two. It is still one weapon: one ammo count, one heat bar, one HUD icon.

For which builds it works on, see the [compatibility table](../../../README.md#compatibility). **A level that contains a `dualcannon` weapon crashes a game without BF2GameExt**, so a mod that uses one requires the extension. See [Class Labels](README.md).

## Properties

Every `cannon` property works as usual. On top of those:

| Property | Value | Description |
|----------|-------|-------------|
| `OffhandGeometryName` | model name | The second gun. Can be the same model as `GeometryName` or a different one. It is packed into the level automatically, like `GeometryName`. |
| `OffhandHardPoint` | hardpoint or bone name | Where the second gun sits on the soldier, the way `hp_weapons` holds the first. Without it no second gun is drawn and every shot leaves the first gun. |
| `OffhandFirePointName` | hardpoint name | Where the second gun's shots and muzzle flash come from, on the `OffhandGeometryName` model. Defaults to `hp_fire`, like `FirePointName`. |
| `AlternateMode` | `shot` or `salvo` | When the guns switch. `shot` (the default) switches on every trigger pull, so a whole salvo leaves one gun. `salvo` switches on every shot of a salvo, so `SalvoCount = 4` fires right, left, right, left. With `SalvoCount = 1` the two are the same. |

All four inherit through `ClassParent`. Several projectiles of one shot (`ShotsPerShot`) always leave the same gun.

## Example

The example in [`GameAssets/Examples/DualCannon`](../../../GameAssets/Examples/DualCannon) follows the stock pistol's layout: a common parent sets the class and the weapon stats, and each side's child only adds its models.

`com_weap_inf_dualpistol.odf`, like the stock `com_weap_inf_pistol.odf` with `cannon` swapped for `dualcannon`:

```
[WeaponClass]
ClassLabel      = "dualcannon"

[Properties]
AnimationBank   = "pistol"
FirePointName   = "hp_fire"
...
```

`rep_weap_inf_dualpistol.odf`:

```
[WeaponClass]
ClassParent          = "com_weap_inf_dualpistol"
GeometryName         = "rep_weap_inf_pistol.msh"

[Properties]
GeometryName         = "rep_weap_inf_pistol"
HighResGeometry      = "rep_1st_weap_inf_pistol"
OrdnanceName         = "rep_weap_inf_pistol_ord"

OffhandFirePointName = "hp_fire"
OffhandGeometryName  = "rep_weap_inf_pistol"
OffhandHardPoint     = "hp_weapons_2"
AlternateMode        = "shot"
```

## The offhand hardpoint

The stock human skeleton has one weapon hardpoint, `hp_weapons`, in the right hand. For the second gun:

- **`bone_l_hand`** works with no new art. The gun follows the left hand at the bone's own position and angle.
- **A hardpoint of your own**, such as `hp_weapons_2`, lets you place and rotate the gun in the left hand the way `hp_weapons` is in the right. It goes in the skeleton of the soldier's animation bank, so every soldier using that bank holds the gun the same way. The example uses one, so it needs a soldier whose bank has it.

## Animations

The first gun plays the soldier's normal `shoot` animation and the second gun plays `shoot2`. In the secondary weapon slot they are `shoot_secondary` and `shoot_secondary2`. Name them like any other soldier animation:

```
<bank>_<AnimationBank>_<posture>_shoot2_upper
```

For example `dualwield_pistol_stand_shoot2_upper` for a `dualwield` bank and `AnimationBank = "pistol"`. A bank without `shoot2` falls back the same way any other missing animation does, so both guns may end up playing the same shoot animation. The stock `FireAnim` property is ignored on this class, because the gun that fires picks the animation.

## Multiplayer

Works online: on every machine both guns fire, flash and animate the way the host sees them. Everyone in the match needs BF2GameExt on a map that uses `dualcannon`, since a game without it crashes loading the map.

## Notes

- **First person shows one gun.** Only `HighResGeometry` is drawn.
- **Known issue:** on a multiplayer client, other players' `dualcannon` bolts are white instead of their own colour. The host's view and your own shots are correct.
- **ODF mistakes are written to the game log**, starting with `[DualCannon]`: an `OffhandGeometryName` model that is not loaded, an `OffhandFirePointName` the offhand model does not have, an `AlternateMode` other than `shot` or `salvo` (it falls back to `shot`), and `FireAnim`.
