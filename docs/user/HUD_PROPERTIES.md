# HUD Properties

Events, properties and transform parameters added by BF2GameExt to `.hud` files, on top of the stock ones. A game without the extension skips what it does not recognise and carries on, so one `.hud` works in both: a binding to one of these events logs "HUD Element unable to find event", and a `FillFrom` line logs "Error reading parameter" and leaves the bar filling from the left.

None of them needs an INI setting. Binding the event, writing the property or declaring the transform is the opt-in, and nothing changes until a `.hud` does one of those. For which builds each one works on, see the [compatibility table](../../README.md#compatibility). Worked examples and layouts are in [HUD System](HUD.md), and every edge case is in the [full reference](../RE/HUDAuthoring.md).

## Events

Bind these like any stock event. Only the first local viewport (`player1`) has them. They are sent from the HUD update, which runs on every machine, so they work on multiplayer clients as well as on the host.

The Type column says what an event can drive:

| Type | Bind it with |
|------|--------------|
| Uint texture | `EventBitmap` on a `Bitmap` or `BarBitmap`, which swaps in that texture |
| Uint number, Float | `EventNumber` on text, `EventValue` on a bar, `EventAlpha` on a group for a Float from 0 to 1 (see `OutputIsAlpha`), or the input of a transform |
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

### Unit and Weapon States

Each is a Float, `1` while the state lasts and `0` otherwise, with nothing in between. Feed one through a [`TransformNumberLerp`](#transformnumberlerp) to fade an element with it. See [Unit and weapon states](HUD.md#unit-and-weapon-states) for the reticule recipe.

| Event | Type | Description | Since |
|-------|------|-------------|-------|
| `player1.unit.state.sprint` | Float | `1` while sprinting. | 1.2.0 |
| `player1.unit.state.jump` | Float | `1` while jumping. | 1.2.0 |
| `player1.unit.state.fall` | Float | `1` while falling. | 1.2.0 |
| `player1.unit.state.roll` | Float | `1` while rolling. | 1.2.0 |
| `player1.unit.state.jet` | Float | `1` while jet jumping. | 1.2.0 |
| `player1.unit.state.hover` | Float | `1` while hovering on a jet pack. | 1.2.0 |
| `player1.unit.state.tumble` | Float | `1` while thrown, knocked down or getting back up. | 1.2.0 |
| `player1.unit.state.land` | Float | `1` for one update on touching down from a jump, a fall or a jet, then `0`. | 1.2.0 |
| `player1.weapon1.state.firing` | Float | `1` while the weapon in slot 1 fires, or a melee weapon attacks. | 1.2.0 |
| `player1.weapon1.state.charging` | Float | `1` while it charges a shot. | 1.2.0 |
| `player1.weapon1.state.reloading` | Float | `1` while it reloads. Never for a melee weapon. | 1.2.0 |
| `player1.weapon1.state.overheated` | Float | `1` while it is overheated and cooling down. | 1.2.0 |
| `player1.weapon1.state.blocking` | Float | `1` while a melee weapon blocks. | 1.2.0 |
| `player1.weapon1.state.shot` | Float | `1` for one update each time it fires, so fast fire can hold it at `1`. | 1.2.0 |
| `player1.weapon2.state.*` | Float | The same six for slot 2. | 1.2.0 |

The unit states are for soldiers on foot: a droideka, a vehicle, a turret and a remote read `0` throughout. The weapon states follow the same weapon as the stock `player1.weaponN.*` events.

### Target Bars

| Event | Type | Description | Since |
|-------|------|-------------|-------|
| `player1.weapon1.target.position` | Vector3 | Where weapon 1's current target is on screen: the top centre of its bounds, kept inside the screen's safe area while the game has it selected. Bind it with `EventPosition` on the group holding the target health bar and the bar floats over the unit. | 1.2.0 |
| `player1.weapon2.target.position` | Vector3 | The same for weapon 2. | 1.2.0 |
| `player1.weapon1.target.distance` | Float | How far away weapon 1's target is, in metres, from what you control (the camera while you are dead). With no target it sends a value `InfiniteDashes(1)` prints as `--`. | 1.2.0 |
| `player1.weapon2.target.distance` | Float | The same for weapon 2. | 1.2.0 |

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
| `player1.commandPostN.position` | Vector3 | Where the post is on screen, for `EventPosition` on a marker group: 1.3 m above its middle, where the stock objective markers sit. Off screen or behind you it stays on the screen's edge, keeping a 5% margin. | 1.2.0 |
| `player1.commandPostN.onScreen` | Bool | Sent as the post comes into view. | 1.2.0 |
| `player1.commandPostN.offScreen` | Bool | Sent as it leaves the view, and when the slot goes out of use, so it alone can hide a marker. | 1.2.0 |
| `player1.commandPostN.direction` | Vector3 | `(0, 0, angle)` for `EventRotation`, turning an arrow drawn pointing up toward the post. | 1.2.0 |
| `player1.commandPostN.distance` | Float | How far away the post is, in metres, from what you control (the camera while you are dead). | 1.2.0 |

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
| `TransformNumberLerp` | Float | Slides between A and B as its input goes from 0 to 1 (or across `InputRange`), over set times up and down, and sends the result as a new event. | 1.2.0 |
| `TransformNumberCompare` | Float, Bool | Compares A with B and sends 1 or 0, and optionally Bool events as the result turns on and off, for `EventEnable` and `EventDisable`. | 1.2.0 |

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
| `OutputIsAlpha` | `1`/`0` or `true`/`false` | Optional, off by default. On, the result is sent every HUD update rather than only when it changes, which a group's `EventAlpha` needs to hold. | 1.2.0 |

```
TransformNumberMath("player1example_healthpercent")
{
    Operation("Multiply")
    EventInputA("player1.healthFraction")
    ConstantB(100.00)
    EventOutput("player1.example.healthPercent")
}
```

Declare a transform after whatever sends its inputs and before anything that uses its output: a later declaration cannot be found. Each parameter appears once. A repeated or unknown parameter switches that transform off and writes the reason to `BF2GameExt.log` under `[HudNumberMath]`. Dividing by zero, or any other invalid result, sends nothing, so the bound element keeps its last value. A `// comment` after a parameter is fine with a space after the `//`; written `//comment`, it counts as a value and switches the transform off. The HUD editor's export drops these blocks, so keep your source file.

`EventAlpha` exists only on groups, and a value sent to it holds for one frame, because the group repaints its own alpha every frame. So a transform feeding one needs `OutputIsAlpha(1)`, and the group needs the stock `PropagateAlpha(1)` for its alpha to reach what is inside it. Leave `OutputIsAlpha` off otherwise: with `EventChanged` it would fire every frame.

### TransformNumberLerp

| Parameter | Value | Description | Since |
|-----------|-------|-------------|-------|
| `EventInput` | event name | Required. An Int, Uint or Float event: `0` gives A, `1` gives B, and anything in between that share of the way from A to B. Values outside count as the nearer end. | 1.2.0 |
| `InputRange` | min, max | Optional. The inputs that give A and B instead of `0` and `1`, such as `InputRange(0, 30)` for a clip of 30. Each end is a number or a quoted event name, which is followed as it changes. | 1.2.0 |
| `ConstantA` | number | The output at `0`, unquoted. Defaults to `0`. Use this or `EventInputA`. | 1.2.0 |
| `EventInputA` | event name | An Int, Uint or Float event for the output at `0`. Use this or `ConstantA`. | 1.2.0 |
| `ConstantB` | number | The output at `1`, unquoted. Defaults to `1`. Use this or `EventInputB`. | 1.2.0 |
| `EventInputB` | event name | An Int, Uint or Float event for the output at `1`. Use this or `ConstantB`. | 1.2.0 |
| `RiseTime` | seconds | Optional. How long the output takes to follow the input all the way up from `0` to `1`. `0`, the default, follows at once. | 1.2.0 |
| `FallTime` | seconds | Optional. The same for the way down from `1` to `0`. Defaults to `0`. | 1.2.0 |
| `EventOutput` | event name | Required. The name of the new Float event. It must not already exist. | 1.2.0 |
| `OutputIsAlpha` | `1`/`0` or `true`/`false` | Optional, off by default. On, the result is sent every HUD update rather than only when it changes, which a group's `EventAlpha` needs to hold. | 1.2.0 |

```
TransformNumberLerp("player1example_sprintalpha")
{
    EventInput("player1.unit.state.sprint")
    ConstantA(1.00)
    ConstantB(0.00)
    RiseTime(0.15)
    FallTime(0.30)
    EventOutput("player1.example.sprintAlpha")
    OutputIsAlpha(1)
}
```

A and B do any inverting: here `ConstantA(1.00)` and `ConstantB(0.00)` turn the sprint's `0` and `1` into an alpha of `1` and `0` with no `TransformNumberMath`, fading out over `RiseTime` as the sprint starts and back in over `FallTime` as it ends. The [HUD System](HUD.md#transformnumberlerp) page has the whole timeline. The times are for a whole swing, so half a swing takes half the time. With only a `FallTime`, a state that lasts a single update, such as `land` or `shot`, still shows in full before it fades. The first input is taken at once, so nothing fades in when the mission loads. With events for A or B, nothing is sent until each has arrived, and a change to either end moves the output at once. The declaring, parameter and logging rules are the same as `TransformNumberMath`'s.

### TransformNumberCompare

| Parameter | Value | Description | Since |
|-----------|-------|-------------|-------|
| `Operation` | `"Greater"`, `"GreaterOrEqual"`, `"Less"`, `"LessOrEqual"`, `"Equal"` or `"NotEqual"` | Required. A compared with B. Case does not matter. | 1.2.0 |
| `ConstantA` | number | A fixed number for the left side. Use this or `EventInputA`. | 1.2.0 |
| `EventInputA` | event name | An Int, Uint or Float event for the left side. Use this or `ConstantA`. | 1.2.0 |
| `ConstantB` | number | A fixed number for the right side. Use this or `EventInputB`. | 1.2.0 |
| `EventInputB` | event name | An Int, Uint or Float event for the right side. Use this or `ConstantB`. | 1.2.0 |
| `Hysteresis` | number, `0` or more | Optional. Once the result is `1`, it stays `1` until A is this far back past B. For `Equal` and `NotEqual`, how close counts as equal. Defaults to `0`. | 1.2.0 |
| `EventOutput` | event name | A new Float event, `1` or `0`. | 1.2.0 |
| `EventOutputTrue` | event name | A new Bool event sent as the result turns to `1`, for `EventEnable`. | 1.2.0 |
| `EventOutputFalse` | event name | A new Bool event sent as the result turns to `0`, for `EventDisable`. | 1.2.0 |
| `OutputIsAlpha` | `1`/`0` or `true`/`false` | Optional, off by default. Sends the Float every HUD update; the Bool events never repeat. | 1.2.0 |

```
TransformNumberCompare("player1example_lowammo")
{
    Operation("Less")
    EventInputA("player1.weapon1.totalClipFraction")
    ConstantB(0.25)
    Hysteresis(0.05)
    EventOutputTrue("player1.example.lowAmmo")
    EventOutputFalse("player1.example.lowAmmoOff")
}
```

At least one output is required. The Bool events are sent when the result changes, and once when the first result is known, so `EventEnable` and `EventDisable` on them show and hide an element with its fade. Nothing is sent until both sides are known. The declaring, parameter and logging rules are the same as `TransformNumberMath`'s.

## INI Settings

Not `.hud` properties, but these change how a `.hud` behaves. The full list is in [Configuration](CONFIGURATION.md).

| Setting | Default | Effect |
|---------|---------|--------|
| `[Features] TargetBarLatchSeconds` | `0.5` | How long `player1.weaponN.target.position` stays on the last target after the selection is lost. `0` never times out. |
| `[Fixes] ReticleCorrection` | `-1` | Moves the reticle back onto the aim point on widescreen displays. `-1` scales it with the aspect ratio, `0` turns it off, and a value up to `1` sets the strength by hand. |
| `[Fixes] WeaponIconFix` | `1` | Lets several mods' weapon icon `TransformNameMesh` tables load together without a stray second icon beside the right one. |
