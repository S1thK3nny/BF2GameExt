# HUD Properties

Events, properties and transform parameters added by BF2GameExt to `.hud` files, on top of the stock ones. A game without the extension skips what it does not recognise and carries on, so one `.hud` works in both: a binding to one of these events logs "HUD Element unable to find event", and a `FillFrom` line logs "Error reading parameter" and leaves the bar filling from the left.

None of them needs an INI setting. Binding the event, writing the property or declaring the transform is the opt-in, and nothing changes until a `.hud` does one of those. For which builds each one works on, see the [compatibility table](../../README.md#compatibility). Worked examples and layouts are in [HUD System](HUD.md), and every edge case is in the [full reference](../RE/HUDAuthoring.md).

## Events

Bind these like any stock event. Only the first local viewport (`player1`) has them. They are sent from the HUD update, which runs on every machine, so they work on multiplayer clients as well as on the host.

The Type column says what an event can drive:

| Type | Bind it with |
|------|--------------|
| Uint texture | `EventBitmap` on a `Bitmap` or `BarBitmap`, which swaps in that texture |
| Uint number, Float | `EventNumber` on text, `EventValue` on a bar, or the input of a transform |
| Bool | `EventEnable` or `EventDisable` |
| Color | `EventColor` |
| Vector3 | `EventPosition` or `EventRotation`, as given for each event |

### Unit and Vehicle Icons

| Event | Type | Description | Since |
|-------|------|-------------|-------|
| `player1.unit.healthTexture` | Uint texture | The health icon of the soldier, hero or droideka you are playing, for its current stance. It is the class's stock `HealthTexture`, with stance variants found by name (below). | 1.2.0 |
| `player1.unit.healthTextureDisable` | Bool | Sent instead when the unit has no loaded icon. | 1.2.0 |
| `player1.unit.stance` | Uint number | The stance: `0` standing, `1` crouched, `2` prone, `3` droideka ball. Sprinting, jumping, rolling and jetting count as standing. | 1.2.0 |
| `player1.vehicle.healthTexture` | Uint texture | The stock `HealthTexture` of the vehicle or turret you are in, following the same seat as the stock `player1.vehicle.*` events. | 1.2.0 |
| `player1.vehicle.healthTextureDisable` | Bool | Sent instead on foot, or when the vehicle has no loaded icon. | 1.2.0 |

Show an icon with `EventEnable` on its texture event and hide it with `EventDisable` on the Disable event:

```
Bitmap("player1classicon")
{
    EventBitmap("player1.unit.healthTexture")
    EventEnable("player1.unit.healthTexture")
    EventDisable("player1.unit.healthTextureDisable")
}
```

**Stance icons.** Each stance tries its own texture first and falls back, so a class needs only the variants it has art for:

| Stance | Tries, in order |
|--------|-----------------|
| Standing | `<name>` |
| Crouched | `<name>_crouch`, `<name>` |
| Prone | `<name>_prone`, `<name>_crouch`, `<name>` |
| Droideka, fully balled | `<name>_ball`, `<name>` |

`<name>` is the class's `HealthTexture`, inherited through `ClassParent` like any other property. Only loaded textures count, so pack every icon and variant into a level the mission loads. Texture names are shared across every loaded level, so give icons a prefix: a vehicle skin is often named after its class and would clash with its icon.

### Weapon Icons

| Event | Type | Description | Since |
|-------|------|-------------|-------|
| `player1.weapon1.iconTexture` | Uint texture | The stock `IconTexture` of the weapon in slot 1, following the same weapon as the stock `player1.weapon1.*` events: the soldier's on foot, the seat's in a vehicle or turret. | 1.2.0 |
| `player1.weapon1.iconTextureDisable` | Bool | Sent instead for an empty slot, or a weapon with no loaded icon. | 1.2.0 |
| `player1.weapon2.iconTexture` | Uint texture | The same for slot 2. | 1.2.0 |
| `player1.weapon2.iconTextureDisable` | Bool | The same for slot 2. | 1.2.0 |

`IconTexture` goes in the weapon ODF's `[WeaponClass]` section. Weapons have no stance variants.

### Target Bars

| Event | Type | Description | Since |
|-------|------|-------------|-------|
| `player1.weapon1.target.position` | Vector3 | Where weapon 1's current target is on screen: the top centre of its bounds, kept inside the screen's safe area while the game has it selected. Bind it with `EventPosition` on the group holding the target health bar and the bar floats over the unit. | 1.2.0 |
| `player1.weapon2.target.position` | Vector3 | The same for weapon 2. | 1.2.0 |

The last target stays for `[Features] TargetBarLatchSeconds` (default `0.5`) after the selection is lost, then the `.hud`'s own `FadeOutTime` plays. A vehicle and its exposed rider no longer make the bar flicker between them. Bind these with `EventPosition` only: an `EventEnable` on them keeps restarting the fade. The event replaces the group's own `Position`, so put offsets on child groups.

### Reticule

| Event | Type | Description | Since |
|-------|------|-------------|-------|
| `player1.reticule.horizonRotation` | Vector3 | `(0, 0, angle)` in degrees for `EventRotation`, keeping a group level with the horizon while the camera banks, in first and third person. Looking almost straight up or down, it holds its last angle. | 1.2.0 |

Put the rotation on a group with `Scale(1,1,1)` and give the artwork its own scale in a child group, since a stretched group changes shape as it rotates. It turns the picture only; aim is unchanged.

### Command Post Strip

`N` is the slot, from 1 to 16.

| Event | Type | Description | Since |
|-------|------|-------------|-------|
| `player1.commandPosts.count` | Uint number | How many slots are in use. | 1.2.0 |
| `player1.commandPostN.icon` | Uint texture | The owning team's icon, as set by `SetTeamIcon`. A neutral post shows team 0's. | 1.2.0 |
| `player1.commandPostN.iconDisable` | Bool | Sent instead when that team has no loaded icon. | 1.2.0 |
| `player1.commandPostN.color` | Color | The owner as friendly, enemy or neutral, in the minimap's colours. | 1.2.0 |
| `player1.commandPostN.capture` | Float | How much of the post its side holds, `0` to `1`. A held post sits at `1` and drains while another team neutralises it; a neutral post sits at `0` and fills as a team captures it. | 1.2.0 |
| `player1.commandPostN.captureColor` | Color | The team gaining or holding the post, so one bar coloured by it shows the whole fight. | 1.2.0 |
| `player1.commandPostN.disable` | Bool | The slot is not in use. | 1.2.0 |

Slots follow each post's `HUDIndex`, lowest first, then posts without one in map order. Posts with `HUDIndexDisplay = 0`, such as the invisible spawn posts and command vehicles, are left out, and a destroyed post drops out of the row. Give neutral posts an icon from the mission script with `SetTeamIcon(0, "icon_name")`. On a multiplayer client, owners are always right but capture progress only moves for posts near the player.

## Element Properties

| Property | Element | Value | Description | Since |
|----------|---------|-------|-------------|-------|
| `FillFrom` | `BarBitmap` | `"Left"`, `"Right"`, `"Bottom"` or `"Top"` | Which end of the bar stays put while its value changes. `"Left"` is the stock fill and the default. `"Right"` keeps the right end and grows to the left, `"Bottom"` keeps the bottom edge and grows upward, and `"Top"` keeps the top edge and grows downward. | 1.2.0 |

The texture is never stretched or flipped: each point of the bar shows the part of the picture the full bar shows there. So two bars can share one picture, such as a health silhouette with the missing part in another colour, and an upright icon fills from the bottom without being rotated.

```
BarBitmap("player1health_missing")
{
    EventValue("player1.example.missingHealth")
    FillFrom("Right")
    Bitmap("hud_healthbar_soldier")
}
```

"Right" is the bar's own far end, not the screen's right, so a rotated bar turns with it. Any `TexCoords` work, including flipped ones the stock fill gets wrong. `"Right"` keeps the bar flash; the vertical modes have none. Case does not matter, and any other value is logged once under `[BarFillFrom]` and fills from the left.

## Transforms

| Transform | Output | Description | Since |
|-----------|--------|-------------|-------|
| `TransformNumberMath` | Float | Works out `A operation B`, where each side is a constant or a live number event, and sends the result as a new event. | 1.2.0 |

### TransformNumberMath

| Parameter | Value | Description | Since |
|-----------|-------|-------------|-------|
| `Operation` | `"Add"`, `"Subtract"`, `"Multiply"`, `"Divide"`, `"Min"` or `"Max"` | Required. Case does not matter. `Subtract` is A minus B and `Divide` is A divided by B. | 1.2.0 |
| `ConstantA` | number | A fixed number for the left side, unquoted. Use this or `EventInputA`. | 1.2.0 |
| `EventInputA` | event name | An Int, Uint or Float event for the left side. Use this or `ConstantA`. | 1.2.0 |
| `ConstantB` | number | A fixed number for the right side, unquoted. Use this or `EventInputB`. | 1.2.0 |
| `EventInputB` | event name | An Int, Uint or Float event for the right side. Use this or `ConstantB`. | 1.2.0 |
| `Clamp` | minimum, maximum | Optional. Keeps the result between the two numbers. The minimum cannot be above the maximum. | 1.2.0 |
| `EventOutput` | event name | Required. The name of the new Float event. It must not already exist. | 1.2.0 |

```
TransformNumberMath("player1example_healthpercent")
{
    Operation("Multiply")
    EventInputA("player1.healthFraction")
    ConstantB(100.00)
    EventOutput("player1.example.healthPercent")
}
```

Declare a transform after whatever sends its inputs and before anything that uses its output: a later declaration cannot be found. Each parameter appears once. A repeated or unknown parameter switches that transform off and writes the reason to `BF2GameExt.log` under `[HudNumberMath]`. Dividing by zero, or any other invalid result, sends nothing, so the bound element keeps its last value. The HUD editor's export drops these blocks, so keep your source file.

## INI Settings

Not `.hud` properties, but these change how a `.hud` behaves. The full list is in [Configuration](CONFIGURATION.md).

| Setting | Default | Effect |
|---------|---------|--------|
| `[Features] TargetBarLatchSeconds` | `0.5` | How long `player1.weaponN.target.position` stays on the last target after the selection is lost. `0` never times out. |
| `[Fixes] ReticleCorrection` | `-1` | Moves the reticle back onto the aim point on widescreen displays. `-1` scales it with the aspect ratio, `0` turns it off, and a value up to `1` sets the strength by hand. |
| `[Fixes] WeaponIconFix` | `1` | Lets several mods' weapon icon `TransformNameMesh` tables load together without a stray second icon beside the right one. |
