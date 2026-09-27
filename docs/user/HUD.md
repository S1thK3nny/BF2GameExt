# HUD System

The stock HUD can only show what the game sends it, and a `.hud` file has no way to do
arithmetic, follow a unit on screen or pick up a class's own icons. BF2GameExt adds
new HUD events, a new transform and a new bar property that work alongside the stock
ones in any `.hud` file.

Everything here is opt-in: nothing changes until a `.hud` binds one of the new events
or uses the new property. Only the first local viewport (`player1`) has these events.
A game without BF2GameExt logs "HUD Element unable to find event" for each new
binding and carries on, so one `.hud` serves both.

To check for what builds these are available on, see the [compatibility table](../../README.md#compatibility).

Every parameter and edge case, the four native transforms, math recipes, test
procedures and troubleshooting are in the [full reference](../RE/HUDAuthoring.md).

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
- The HUD editor's export drops these blocks, so keep your source file.

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
