# ODF Properties

Properties added by BF2GameExt, on top of the stock ones. The game ignores properties it does not recognise, so an ODF using these still loads and plays without the extension installed. The property simply does nothing.

For which builds each one works on, see the [compatibility table](../../README.md#compatibility).

## Soldier Classes

Set on the concrete soldier class.

| Property | Value | Description |
|----------|-------|-------------|
| `FirstPersonAnimationBank` | bank name | Gives the class its own first person animation bank instead of the one global set every soldier shares. Partial banks are fine: any animation the bank does not have falls back to the default (`humanfp` / `droidekafp`), so a bank holding only a reload is valid. |
| `OverrideTexture3` | texture name | A third runtime texture override slot, on top of the stock `OverrideTexture` and `OverrideTexture2`. |
| `OverrideTexture4` | texture name | Fourth slot. |
| `OverrideTexture5` | texture name | Fifth slot. |

**Notes on the override texture slots.** The model needs a material named `override_texture3`, `override_texture4` or `override_texture5` for the matching slot to do anything, following the same naming the stock two slots use. `OverrideTexture` must also be set or none of the extra slots apply. They are read off the concrete class only and are not inherited through `ClassParent`.

## Weapon Classes

| Property | Class | Value | Description |
|----------|-------|-------|-------------|
| `DisguiseModel` | `WeaponDisguise` | model name | Swaps the soldier to a specific model while disguised, instead of cloning the first enemy soldier the game finds. The model has to be loaded in memory. Set it to a single space (`" "`) to keep the soldier's own model and suppress the swap entirely. Leave it out for stock behaviour. |
| `AnimTexture1` | `WeaponMelee` | texture name | Second frame of an animated lightsaber blade. Set under the blade's `WeaponMelee` section. |
| `AnimTexture2` | `WeaponMelee` | texture name | Third frame. |
| `AnimTexture3` | `WeaponMelee` | texture name | Fourth frame. |

The blade's existing texture is frame one, so the three properties complete a four frame cycle. Ported from the Xbox version, where PC blades only ever used a single static texture.

```
[WeaponMelee]
AnimTexture1 = "blade_red_2"
AnimTexture2 = "blade_red_3"
AnimTexture3 = "blade_red_4"
```

## Ordnance Classes

| Property | Class | Value | Description |
|----------|-------|-------|-------------|
| `CableTexture` | `OrdnanceGrapplingHook` | texture name | Re-skins the grappling hook's cable. Defaults to `com_bldg_minigun`. Modtools only. |

**One texture for the whole mod.** The cable texture is shared by every grappling hook rather than set per weapon, so with more than one grapple ODF loaded the last one parsed wins. The texture also has to be one the map already loads.

The engine's own `SoldierAnimation` property on the same class is worth knowing about here, since nothing else in the game reaches it: give it an animation name and the soldier plays that clip for the whole pull. Leave it out and the soldier keeps their normal pose.

## Vehicle Classes

| Property | Value | Description |
|----------|-------|-------------|
| `DisableBallMode` | `1` | Removes the droideka's roll outright, for the AI as well as the player, turning the chassis into a plain walking unit. The spawn screen follows suit and previews the unit standing and idling instead of curled into a ball. Set on a `walkerdroid` class. Off by default, and inherits through `ClassParent` like a stock property, so `DisableBallMode = 0` on a child restores the roll. |

## Animation Naming Conventions

Not properties, but ODF-adjacent: these are picked up by name out of an animation bank, with nothing to declare anywhere.

| Animation | Where | Description |
|-----------|-------|-------------|
| `<bank>_rifle_sprint` | first person soldier bank | Played while sprinting, in place of the run animation being sped up. Also `<bank>_bazooka_sprint` and `<bank>_tool_sprint`. Entirely optional: if the animation is not in the bank, nothing changes. |
| `boost` | flyer animation bank | Played automatically while boosting, blending in and out. Frame 0 should be the normal flying pose and the last frame the full boost pose. The length of the animation sets how long the blend takes, so add frames to slow it down and remove frames to speed it up. |
