# HUD System

The stock HUD can only show what the game sends it, and a `.hud` file has no way to do
arithmetic, follow a unit on screen or pick up a class's own icons. BF2GameExt adds
new HUD events, three new transforms and a new bar property that work alongside the
stock ones in any `.hud` file.

Everything here is opt-in: nothing changes until a `.hud` binds one of the new events
or uses the new property. Only the first local viewport (`player1`) has these events.
A game without BF2GameExt logs "HUD Element unable to find event" for each new
binding and carries on, so one `.hud` serves both.

To check for what builds these are available on, see the [compatibility table](../../README.md#compatibility).

[HUD Properties](HUD_PROPERTIES.md) lists every event, property and parameter on one
page, with the version each arrived in. Every parameter and edge case, the four native
transforms, math recipes, test procedures and troubleshooting are in the
[full reference](../RE/HUDAuthoring.md).

## TransformNumberMath

Calculates `A operation B` from two numbers and publishes the result as a new Float
event. Either side can be a constant or a live numeric event.

| Parameter | Syntax | Description |
|-----------|--------|-------------|
| `Operation` | `Operation("name")` | Required. `Add`, `Subtract`, `Multiply`, `Divide`, `Min` or `Max` |
| `ConstantA` | `ConstantA(value)` | A fixed number for the left side. Unquoted |
| `EventInputA` | `EventInputA("event")` | An Int, Uint or Float event for the left side. Use this **or** `ConstantA` |
| `ConstantB` | `ConstantB(value)` | A fixed number for the right side |
| `EventInputB` | `EventInputB("event")` | An event for the right side. Use this **or** `ConstantB` |
| `Clamp` | `Clamp(min, max)` | Optional. Keeps the result between the two numbers |
| `EventOutput` | `EventOutput("event")` | Required. The name of the new event. It must not already exist |
| `OutputIsAlpha` | `OutputIsAlpha(1)` | Optional. Sends the result every update rather than only when it changes. Needed for a group's `EventAlpha` (below) |

```
TransformNumberMath("player1example_healthpercent")
{
    Operation("Multiply")
    EventInputA("player1.healthFraction")
    ConstantB(100.00)
    EventOutput("player1.example.healthPercent")
}
```

Bind the output like any other event, such as `EventNumber("player1.example.healthPercent")`
with `FloatFormat("%.0f")` on a text element.

- Declare a transform **before** anything that uses its output, and after whatever
  sends its inputs. A later declaration cannot be found.
- Order matters for `Subtract` and `Divide`: the result is A minus B, or A divided by B.
- One transform does one operation. Chain several for longer formulas.
- Dividing by zero, or any invalid result, sends nothing; the bound element keeps its
  last value.
- A mistake in the block, such as a repeated or unknown parameter, switches that
  transform off and writes the reason to `BF2GameExt.log` under `[HudNumberMath]`.
- A `// comment` after a parameter on the same line is fine. Leave a space after
  the `//`: written `//comment`, it counts as a value and switches the transform off.
- `EventAlpha` only works on a group, and only holds for one frame, because the
  group repaints its own alpha every frame. For an alpha, add `OutputIsAlpha(1)` so
  the value is sent every update, and give the group `PropagateAlpha(1)` so the
  alpha reaches what is inside it. Leave `OutputIsAlpha` off for anything else, and
  never use it with `EventChanged`, which would then fire every frame.
- The HUD editor's export drops these blocks, so keep your source file.

## TransformNumberLerp

Slides a value between two ends, A and B, and publishes it as a new Float event. The
lerp keeps a point between 0 and 1 and sends the matching value between A and B:

| Point | Output |
|-------|--------|
| 0 | A |
| 0.5 | Halfway from A to B |
| 1 | B |

A and B do any inverting, so no `TransformNumberMath` is needed for it. With
`ConstantA(1.00)` and `ConstantB(0.00)`, a point of 0 gives 1 and a point of 1 gives
0. Left out, A is 0 and B is 1, and the output is the point itself.

The input says where the point should be. When it changes, the point does not jump:
it travels there at a steady speed, so an alpha or a bar made from it fades rather
than pops. `RiseTime` is the seconds a whole trip up from 0 to 1 takes, and
`FallTime` the seconds from 1 back down to 0. Both follow the input, not the output.
The sprint alpha from [Unit and weapon states](#unit-and-weapon-states), with A = 1,
B = 0, `RiseTime(0.15)` and `FallTime(0.30)`, runs like this:

| Moment | Sprint event | Point | Output |
|--------|--------------|-------|--------|
| Walking | 0 | 0 | 1.0 |
| Sprint starts | 1 | 0 | 1.0 |
| 0.075 s later | 1 | 0.5 | 0.5 |
| 0.15 s later | 1 | 1 | 0.0 |
| Sprint ends | 0 | 1 | 0.0 |
| 0.15 s later | 0 | 0.5 | 0.5 |
| 0.30 s later | 0 | 0 | 1.0 |

Any input from 0 to 1 works, not only 0 and 1: fed `player1.healthFraction`, the
point glides after the health. For any other range, `InputRange` gives the inputs
that mean 0 and 1: `InputRange(0, 30)` takes a clip of 30 bullets straight in, with
15 as the halfway point. Either end can be an event instead of a number, such as
`InputRange(0, "player1.example.clipSize")`, and the point then follows it when it
changes. Inputs past either end count as that end.

| Parameter | Syntax | Description |
|-----------|--------|-------------|
| `EventInput` | `EventInput("event")` | Required. An Int, Uint or Float event: where the point should be |
| `InputRange` | `InputRange(min, max)` | Optional. The inputs that give point 0 and point 1. Each is a number or a quoted event name. Defaults to 0 and 1 |
| `ConstantA` | `ConstantA(value)` | The output at point 0. Defaults to 0 |
| `EventInputA` | `EventInputA("event")` | An event for the output at point 0. Use this **or** `ConstantA` |
| `ConstantB` | `ConstantB(value)` | The output at point 1. Defaults to 1 |
| `EventInputB` | `EventInputB("event")` | An event for the output at point 1. Use this **or** `ConstantB` |
| `RiseTime` | `RiseTime(seconds)` | Optional. Seconds for the point to travel from 0 up to 1. Defaults to 0, at once |
| `FallTime` | `FallTime(seconds)` | Optional. Seconds for the point to travel from 1 down to 0. Defaults to 0, at once |
| `EventOutput` | `EventOutput("event")` | Required. The name of the new event. It must not already exist |
| `OutputIsAlpha` | `OutputIsAlpha(1)` | Optional. Sends the value every update rather than only when it changes. Needed for a group's `EventAlpha`, as under `TransformNumberMath` above |

```
TransformNumberLerp("player1example_landflash")
{
    EventInput("player1.unit.state.land")
    FallTime(0.40)
    EventOutput("player1.example.landFlash")
    OutputIsAlpha(1)
}
```

Bound with `EventAlpha` on a group with `PropagateAlpha(1)`, this shows what is in
the group in full on landing and fades it out over 0.4 seconds.

- The times are for a whole trip: half a trip takes half the time, and a point
  that turns round part way goes back from where it got to.
- A time of 0 follows at once, so with only a `FallTime` a change that lasts a
  single update, such as a landing or a shot, still shows in full before it fades.
- The first input is taken at once, so nothing fades in when the mission loads.
- With events for A or B, nothing is sent until each has arrived. A change to either
  end moves the output at once; only the point travels.
- Everything else follows `TransformNumberMath`'s rules above: declare it before its
  consumers, give each parameter once, and read `[HudNumberMath]` in the log for
  mistakes.

## TransformNumberCompare

Compares A with B and publishes 1 when the comparison holds and 0 when it does not,
so a HUD can react to a value crossing a line: low ammo, low health, a full charge.
Either side can be a constant or a live numeric event.

| Parameter | Syntax | Description |
|-----------|--------|-------------|
| `Operation` | `Operation("name")` | Required. `Greater`, `GreaterOrEqual`, `Less`, `LessOrEqual`, `Equal` or `NotEqual`: A compared with B |
| `ConstantA` | `ConstantA(value)` | A fixed number for the left side |
| `EventInputA` | `EventInputA("event")` | An Int, Uint or Float event for the left side. Use this **or** `ConstantA` |
| `ConstantB` | `ConstantB(value)` | A fixed number for the right side |
| `EventInputB` | `EventInputB("event")` | An event for the right side. Use this **or** `ConstantB` |
| `Hysteresis` | `Hysteresis(amount)` | Optional. Once on, it stays on until A is this far back past B, so a value sitting on the line does not flicker. For `Equal` and `NotEqual`, how close counts as equal. Defaults to 0 |
| `EventOutput` | `EventOutput("event")` | A new Float event: 1 or 0 |
| `EventOutputTrue` | `EventOutputTrue("event")` | A new Bool event, sent each time the result turns to 1. For `EventEnable` |
| `EventOutputFalse` | `EventOutputFalse("event")` | A new Bool event, sent each time the result turns to 0. For `EventDisable` |
| `OutputIsAlpha` | `OutputIsAlpha(1)` | Optional. Sends the Float every update, as under `TransformNumberMath` above |

At least one of the three outputs is required.

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

Show a warning with `EventEnable("player1.example.lowAmmo")` and
`EventDisable("player1.example.lowAmmoOff")`. It comes on below a quarter of a clip
and goes off again only once the clip is back above 30%. To fade the warning
instead, send `EventOutput` through a `TransformNumberLerp`.

- The Bool events are sent only when the result changes, and once when the first
  result is known, so a fade on the element plays each time rather than restarting.
- Nothing is sent until both sides are known. An invalid value holds the last result.
- Everything else follows `TransformNumberMath`'s rules above.

## Floating target bars

`player1.weapon1.target.position` and `player1.weapon2.target.position` carry the
position on screen of that weapon's current target. Bind one with `EventPosition` on
the group holding the target health bar and the bar floats over the unit.

```
Group("player1targetbar")
{
    EventPosition("player1.weapon1.target.position")
    // The existing target health bar, glow and name label go here.
}
```

- The bar stays on the last target for a moment after your aim leaves it, then the
  `.hud`'s own fade plays. The hold is set by `[Features] TargetBarLatchSeconds`
  (default `0.5`, `0` never times out); the fade is `FadeOutTime` on the elements.
- A big vehicle up close keeps its bar on screen while you are aiming at it. Once you
  stop, the bar leaves the screen with its target.
- A vehicle and its exposed rider no longer make the bar flicker between them.
- Use `EventPosition` only. Binding the position event to `EventEnable` keeps
  restarting the fade.
- Sizes, offsets and labels stay in the `.hud`: put offsets on child groups, since
  the event replaces the parent's own `Position`.

## Reticule horizon levelling

`player1.reticule.horizonRotation` rotates a group so it stays level with the horizon
while the camera banks, in first and third person.

```
Group("player1reticule_group")
{
    EventPosition("player1.weapon1.reticule.position")
    Position(0.500000,0.500000,0,"Viewport")
    Scale(1,1,1)
    EventRotation("player1.reticule.horizonRotation")
    EventEnable("player1.weapon1.reticule.position")
    EventDisable("player1.weapon1.reticule.disable")
    EventAlpha("player1.reticule.alpha")

    Group("player1reticule_artwork")
    {
        Position(0,0,0,"Viewport")
        Scale(0.750000,0.800000,0.750000)
        // The existing reticule Model3D blocks go here, unchanged.
    }
}
```

- Put the rotation on a group with `Scale(1,1,1)` and move the artwork's own scale
  into a child group, as above. A stretched group changes shape as it rotates.
- Looking almost straight up or down, the reticule holds its last angle.
- It rotates the picture only; aim is unchanged.

## Unit and weapon states

These Float events are 1 while you are doing something and 0 otherwise, so a HUD can
react to it. Each jumps straight between 0 and 1: feed it through a
`TransformNumberLerp` to fade something rather than pop it.

| Event | 1 while |
|-------|---------|
| `player1.unit.state.sprint` | Sprinting |
| `player1.unit.state.jump` | Jumping |
| `player1.unit.state.fall` | Falling |
| `player1.unit.state.roll` | Rolling |
| `player1.unit.state.jet` | Jet jumping |
| `player1.unit.state.hover` | Hovering on a jet pack |
| `player1.unit.state.tumble` | Thrown, knocked down or getting back up |
| `player1.unit.state.land` | One update, on touching down from a jump, a fall or a jet |
| `player1.weapon1.state.firing` | The weapon in slot 1 fires, or a melee weapon attacks |
| `player1.weapon1.state.charging` | It charges a shot |
| `player1.weapon1.state.reloading` | It reloads |
| `player1.weapon1.state.overheated` | It has overheated and is cooling down |
| `player1.weapon1.state.blocking` | A melee weapon blocks |
| `player1.weapon1.state.shot` | One update, each time it fires |

`weapon2` is the same for slot 2.

To fade the reticule out while you sprint, turn the sprint into an alpha that goes
from 1 to 0:

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

A and B turn the sprint round, so no `TransformNumberMath` is needed. `RiseTime` is
the fade out as the sprint starts and `FallTime` the fade back in as it ends; the
timeline is under [TransformNumberLerp](#transformnumberlerp). `OutputIsAlpha(1)`
keeps the reticule hidden for the whole sprint rather than only while it fades.
Bind the result on a child group, so the reticule group keeps its own alpha:

```
Group("player1reticule_group")
{
    // Existing position, rotation, enable and EventAlpha lines stay as they are.
    Group("player1reticule_artwork")
    {
        EventAlpha("player1.example.sprintAlpha")
        PropagateAlpha(1)
        // The existing reticule Model3D blocks go here, unchanged.
    }
}
```

- The unit states are for soldiers on foot. A droideka, a vehicle, a turret and a
  remote read 0 throughout.
- The weapon states follow the same weapon as the stock `player1.weaponN.*` events:
  the soldier's on foot, the seat's in a vehicle or turret, and a remote's while you
  control one.
- A melee weapon's block counts as `blocking`, never as reloading, and it never
  reads charging or overheated.
- Fast fire can keep `shot` at 1 from one update to the next.

## Class, stance and vehicle icons

These events carry the health icon of whatever you are playing, so a HUD can follow
the class, its stance and any vehicle without Lua. They also work on multiplayer
clients, where Lua callbacks never run.

| Event | Type | Carries |
|-------|------|---------|
| `player1.unit.healthTexture` | Uint | The soldier, hero or droideka's icon for its current stance |
| `player1.unit.healthTextureDisable` | Bool | Sent instead when the unit has no loaded icon |
| `player1.unit.stance` | Uint | 0 stand, 1 crouch, 2 prone, 3 ball |
| `player1.vehicle.healthTexture` | Uint | The vehicle or turret's icon |
| `player1.vehicle.healthTextureDisable` | Bool | Sent instead on foot, or when the vehicle has no loaded icon |
| `player1.weapon1.iconTexture` | Uint | The icon of the weapon in slot 1; `weapon2` for slot 2 |
| `player1.weapon1.iconTextureDisable` | Bool | Sent instead for an empty slot or a weapon with no loaded icon |

Bind a texture the way the stock team icons are bound:

```
Bitmap("player1classicon")
{
    // Existing position, size and colour lines stay as they are.
    EventBitmap("player1.unit.healthTexture")
    EventEnable("player1.unit.healthTexture")
    EventDisable("player1.unit.healthTextureDisable")
}
```

A health bar that already shows and hides on the health events only needs the
`EventBitmap` line on each of its `BarBitmap` layers.

The icon is the class's stock `HealthTexture` property, inherited through
`ClassParent` like any other:

```
[Properties]
HealthTexture = "hud_icon_rep_rifleman"
```

Stance icons are found by name, so they need no extra property. Each stance tries
its own texture first and falls back:

| Stance | Tries, in order |
|--------|-----------------|
| Standing (also sprinting, jumping, rolling and jetting) | `hud_icon_rep_rifleman` |
| Crouched | `hud_icon_rep_rifleman_crouch`, `hud_icon_rep_rifleman` |
| Prone | `hud_icon_rep_rifleman_prone`, `hud_icon_rep_rifleman_crouch`, `hud_icon_rep_rifleman` |
| Droideka, fully balled | `<name>_ball`, `<name>` |

### Weapon icons

`player1.weaponN.iconTexture` carries the stock `IconTexture` of the weapon that the
stock `player1.weaponN.*` events are showing: the soldier's weapons on foot, the
seat's weapons in a vehicle or turret. `IconTexture` goes in the weapon ODF:

```
[WeaponClass]
IconTexture = "hud_icon_rep_dc15a"
```

- Only loaded textures count: pack every icon and variant into a level the mission
  loads. A missing variant falls back; a missing icon sends the Disable event.
- Texture names are shared across every loaded level, so give icons a prefix. A
  vehicle skin is often named after its class and would clash with its icon.
- The stock vehicle seating mesh still draws over the vehicle icon.

## Command post strip

A row of command post icons, each showing who owns the post and how far a capture
has got. BF2GameExt sends one set of events per post; the `.hud` lays them out.
`N` runs from 1 to 16.

| Event | Type | Carries |
|-------|------|---------|
| `player1.commandPosts.count` | Uint | How many slots are in use |
| `player1.commandPostN.icon` | Uint | The owning team's icon, as set by `SetTeamIcon` |
| `player1.commandPostN.iconDisable` | Bool | Sent instead when that team has no loaded icon |
| `player1.commandPostN.color` | Color | The owner as friendly, enemy or neutral, as on the minimap |
| `player1.commandPostN.capture` | Float | How much of the post its side holds, 0 to 1 |
| `player1.commandPostN.captureColor` | Color | The team gaining or holding it |
| `player1.commandPostN.disable` | Bool | The slot is not in use |

`capture` is 1 for a held post and drains while another team neutralises it. A
neutral post sits at 0 and fills as a team captures it. One bar coloured by
`captureColor` shows the whole fight.

Slots follow each post's `HUDIndex`, lowest first, then posts without one in map
order. Posts with `HUDIndexDisplay = 0`, such as the invisible spawn posts and
command vehicles, are left out, and a destroyed post drops out of the row.

A neutral post shows team 0's icon. Set one where the mission sets the others:

```lua
SetTeamIcon(0, "bf3_neutral_icon")
```

One slot, repeated at a fixed spacing inside a row group:

```
Group("cp_slot1")
{
    Position(0.000, 0.000, 0.000, "Viewport")
    Bitmap("cp_slot1_icon")
    {
        Bitmap("bf3_neutral_icon")
        BitmapRect(0.030, 0.030, "Center", "Center", "Viewport")
        EventBitmap("player1.commandPost1.icon")
        EventColor("player1.commandPost1.color")
        EventEnable("player1.commandPost1.icon")
        EventDisable("player1.commandPost1.disable")
    }
    BarBitmap("cp_slot1_capture")
    {
        Bitmap("hud_cp_capture_bar")
        BitmapRect(0.030, 0.004, "Left", "Center", "Viewport")
        Position(-0.015, 0.022, 0.000, "Viewport")
        EventValue("player1.commandPost1.capture")
        EventColor("player1.commandPost1.captureColor")
        EventEnable("player1.commandPost1.icon")
        EventDisable("player1.commandPost1.disable")
    }
}
```

- To keep the row centred, drive the row group's `EventPosition` from
  `player1.commandPosts.count` with a `TransformNumberVector3`.
- A `Scale` on the row group also shrinks the spacing toward slot 1. Put it on each
  slot group to keep the spacing.
- On a multiplayer client, owners are always right, but capture progress only moves
  for posts near the player. Elsewhere a post shows full for its owner or empty when
  neutral. On the host and in single player every post is live.

## FillFrom

A `BarBitmap` normally keeps its left end and moves its right edge. `FillFrom` picks
the end it grows from. The texture is never stretched or flipped: each point shows
the same part of the picture the full bar shows there.

| Value | Behaviour |
|-------|-----------|
| `FillFrom("Left")` | Stock behaviour, and the default |
| `FillFrom("Right")` | Keeps its right end and grows to the left |
| `FillFrom("Bottom")` | Keeps its bottom edge and grows upward, with the picture upright |
| `FillFrom("Top")` | Keeps its top edge and grows downward |

Two bars can share one picture, such as a health silhouette with the missing part in
another colour. Feed the second bar `1 - health` from a `TransformNumberMath`:

```
TransformNumberMath("player1example_missinghealth")
{
    Operation("Subtract")
    ConstantA(1.00)
    EventInputB("player1.healthFraction")
    Clamp(0.00, 1.00)
    EventOutput("player1.example.missingHealth")
}

BarBitmap("player1health_missing")
{
    EventValue("player1.example.missingHealth")
    FillFrom("Right")
    Bitmap("hud_healthbar_soldier")
    // Same BitmapRect, TexCoords, Rotation, position and enable/disable
    // events as the health bar.
}
```

- "Right" is the bar's own far end, not the screen's right: a rotated bar turns with it.
- `"Bottom"` lets an upright icon, such as a team emblem, fill without being rotated.
- Any `TexCoords` works, including flipped ones the stock fill gets wrong.
- `"Right"` keeps the bar flash. The vertical modes have none.
- A game without BF2GameExt logs `Error reading parameter` for the line and fills
  from the left.
