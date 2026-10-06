# HUD authoring: full reference

The long form of [HUD.md](../user/HUD.md). The user doc covers what GameExt adds and
[HUD_PROPERTIES.md](../user/HUD_PROPERTIES.md) lists it on one page; this file keeps
the detail behind it, every parameter and edge case, in five parts:

- **How events are wired**: file structure, names, load order and which element
  property takes which type of event.
- **GameExt events**: class, stance, vehicle and weapon icons, unit and weapon
  states, floating target bar positions, a horizon-levelled reticule angle and the
  command post strip.
- **Bar fill direction**: `FillFrom` on a `BarBitmap`.
- **TrueWidescreen files**: `TrueWidescreen(1)` in a `FileInfo`, a whole file laid out
  at 4:3 and drawn without the stock wide-screen stretch.
- **Transforms**: `TransformNumberMath`, `TransformNumberLerp`,
  `TransformNumberCompare` and the four native transforms they sit beside, with
  math recipes and an input event catalogue.

Test procedures, troubleshooting and references come last. The GameExt additions
arrived after version 1.1.0, work on Modtools, Steam and GOG, and change nothing
until a `.hud` uses them. Native details below were checked against the open
Phantom/reference executable and the repository's reverse-engineering notes.
Reference-build quirks are called out explicitly rather than assumed to be fixes
supplied by GameExt.

## Contents

- [How events are wired](#how-events-are-wired)
- [GameExt events](#gameext-events)
  - [Class, stance, vehicle and weapon icons](#class-stance-vehicle-and-weapon-icons)
  - [Unit and weapon states](#unit-and-weapon-states)
  - [Floating target bars](#floating-target-bars)
  - [Reticule horizon levelling](#reticule-horizon-levelling)
  - [Command post strip](#command-post-strip)
- [Bar fill direction](#bar-fill-direction)
- [TrueWidescreen files](#truewidescreen-files)
- [Transforms](#transforms)
  - [Choose a transform](#choose-a-transform)
  - [TransformNumberMath](#transformnumbermath)
  - [TransformNumberLerp](#transformnumberlerp)
  - [TransformNumberCompare](#transformnumbercompare)
  - [Shared native numeric parameters](#shared-native-numeric-parameters)
  - [TransformNumberColor](#transformnumbercolor)
  - [TransformNumberVector3](#transformnumbervector3)
  - [TransformNumberColorBlend](#transformnumbercolorblend)
  - [TransformNameMesh](#transformnamemesh)
  - [Practical math recipes](#practical-math-recipes)
  - [Useful input events](#useful-input-events)
- [Testing and verification](#testing-and-verification)
- [Troubleshooting and limits](#troubleshooting-and-limits)
- [Evidence and related references](#evidence-and-related-references)

## How events are wired

An element shows an event by binding its name, as in `EventValue` or `EventColor`;
a transform turns one event into a new one. This part covers where transforms go,
how names and load order work, and which element property takes which type of
event.

### Minimal file structure

Your existing `hudtransforms.hud` uses this arrangement:

```text
FileInfo("hudtransforms")
{
    Viewports(1)
}

Viewport("Transforms")
{
    EventNameFilter("player%")

    TransformNumberMath("player1example_healthpercent")
    {
        Operation("Multiply")
        EventInputA("player1.healthFraction")
        ConstantB(100.00)
        EventOutput("player1.example.healthPercent")
    }
}
```

When editing the existing file, add blocks inside its viewport; do not duplicate
the file header or replace the other transforms. Munge it through the normal HUD
asset pipeline and load it before the HUD elements that consume its new events.

The quoted name after the class is the **transform instance name**. The string
in `EventOutput` is a separate **event name**. A consumer binds the event name:

```text
// Inside a Text element, with its usual font/position/visibility settings:
EventNumber("player1.example.healthPercent")
FloatFormat("%.0f")
```

Use unique instance and output names. Names are hashed case-insensitively by the
engine; changing capitalisation does not create a separate event. Use descriptive
names, not bare numbers. Prefixing custom events with `player1.example.` in this
guide keeps examples separate from your existing stock/derived events.

### Ordering and filtering

1. Load the source event's producer.
2. Declare the transform using that event.
3. Declare downstream transforms, then elements consuming their outputs.

Binding happens during HUD loading. A missing forward reference is not repaired
when that event appears later. `EventNameFilter("player%")` belongs to the viewport,
not inside a math transform. It uses the engine's player-name filtering for both
input bindings and output creation; retain the existing convention of authoring
`player1.*` names inside that viewport.

Math requires a **new** output name and refuses collisions. Native transforms
use the engine's get-or-create output path instead; this can allow multiple
writers or an incompatible pre-existing event type. Prefer unique outputs for
native transforms too. Do not use a stock input name as your output.

### Match the consumer to the output type

| Output type | Typical consumer |
|---|---|
| Float | `EventValue` on a bar; `EventNumber` on text; `EventAlpha` on a group for opacity |
| Color | `EventColor` |
| Vector3 | `EventPosition`, `EventScale`, `EventRotation` on a suitable group |
| Model | `EventMesh` on `Model3D` |

A Float cannot directly drive `EventScale` or `EventRotation`; pass it through
`TransformNumberVector3` first. A Model output is not a bitmap Texture. The
transform does not change an element's mesh, dimensions or layout unless you
explicitly connect an appropriate output to that property.

`EventEnable`/`EventDisable` are event notifications, **not numeric comparisons**.
Sending zero to an `EventEnable` binding does not mean "disable". Keep existing
visibility events and use `EventAlpha` for a continuously varying opacity.

`EventAlpha` exists only on groups, and a value sent to it lasts **one frame**:
`Element::Update` repaints the group's alpha from its own fade every frame, so
the value has to be sent again every frame to hold. Stock alpha events are; a
GameExt transform is only when it has `OutputIsAlpha(1)`. Give the group
`PropagateAlpha(1)`, a stock property, so its alpha reaches the elements inside
it.

## GameExt events

These events carry what the stock HUD never published. Bind them like any stock
event; the binding is the opt-in, and none of them needs an INI setting. They are
sent from the HUD update, which runs on every machine, so they work on multiplayer
clients too. Only the first local viewport (`player1`) has them.

A game without GameExt ignores these bindings: the HUD loader logs "HUD Element
unable to find event" and carries on, so one `.hud` serves both.

### Class, stance, vehicle and weapon icons

These events carry the health icon of whatever you are playing and the icon of each
weapon in use, so a HUD can follow the spawned class, its stance and any vehicle
without Lua. Unlike a Lua icon swap, they also work on multiplayer clients, where
`OnCharacterSpawn` and `OnCharacterEnterVehicle` never fire.

| Event | Type | Carries |
|---|---|---|
| `player1.unit.healthTexture` | Uint | Texture name hash of the soldier, hero or droideka's icon for its stance |
| `player1.unit.healthTextureDisable` | Bool | Sent instead when the unit has no loaded icon |
| `player1.unit.stance` | Uint | 0 stand, 1 crouch, 2 prone, 3 ball |
| `player1.vehicle.healthTexture` | Uint | Texture name hash of the vehicle or turret seat's icon |
| `player1.vehicle.healthTextureDisable` | Bool | Sent instead on foot, or when the vehicle has no loaded icon |
| `player1.weapon1.iconTexture` | Uint | Texture name hash of weapon 1's icon; `weapon2` for weapon 2 |
| `player1.weapon1.iconTextureDisable` | Bool | Sent instead when that slot is empty or its weapon has no loaded icon |

Bind a texture the way the stock team icons are bound. `EventEnable` on the texture
event shows the bitmap when an icon arrives, and the Disable twin hides it:

```text
Bitmap("player1classicon")
{
    // Existing position, size and colour lines stay as they are.
    EventBitmap("player1.unit.healthTexture")
    EventEnable("player1.unit.healthTexture")
    EventDisable("player1.unit.healthTextureDisable")
}
```

A health bar that already shows and hides on the health events only needs the
`EventBitmap` line on each of its `BarBitmap` layers; keep its `Bitmap("...")` line as
the texture it starts with.

Each event is sent on the first HUD update of a mission and then only when it
changes. Nothing is published unless a `.hud` binds at least one of them.

#### Setting up the classes

The icon is the class's stock `HealthTexture` ODF property. Vehicles and turrets often
set it already, and soldiers, heroes and droidekas can set it too. It is inherited
through `ClassParent` like any other property:

```text
[Properties]
HealthTexture = "hud_icon_rep_rifleman"
```

Texture names are global across every loaded level, so give icons a prefix: a vehicle
skin is often named after its class, and an icon with the same name would clash with it.

Stance icons are found by name, so they need no extra property. Each stance tries its
own texture first, then falls back:

| Stance | Tries, in order |
|---|---|
| Standing, and every other soldier state | `hud_icon_rep_rifleman` |
| Crouched | `hud_icon_rep_rifleman_crouch`, `hud_icon_rep_rifleman` |
| Prone | `hud_icon_rep_rifleman_prone`, `hud_icon_rep_rifleman_crouch`, `hud_icon_rep_rifleman` |
| Droideka, fully balled | `<name>_ball`, `<name>` |

A droideka shows its ball icon only while fully balled; rolling up and unrolling keep
the standing icon. Sprinting, jumping, rolling, jetting and being knocked down all
count as standing. Only loaded textures count, so pack every icon and variant into a
level the mission loads; a variant that is not there simply falls back.

`player1.vehicle.healthTexture` follows the same vehicle or turret seat as the stock
`player1.vehicle.*` events, and carries that class's own `HealthTexture`. On foot, or
in a vehicle with no loaded icon, its Disable twin is sent instead, so give every
vehicle and turret an icon, or a generic one through their parent ODFs. The stock
`player1.vehicle.seatingMesh` is untouched, so the seat mesh still draws over the icon
as before. Remote-controlled units are not covered, as with the stock vehicle events.

#### Weapon icons

`player1.weapon1.iconTexture` and `player1.weapon2.iconTexture` carry the stock
`IconTexture` of the weapon each stock `player1.weaponN.*` event family is showing:
the soldier's weapons on foot, the seat's weapons in a vehicle or turret, and a
remote's while controlling one. They change whenever that weapon or its class does.
`IconTexture` goes in the weapon ODF:

```text
[WeaponClass]
IconTexture = "hud_icon_rep_dc15a"
```

Weapons have no stance variants. Stock weapon ODFs already name icons, such as
`HUD_all_lascannon_icon`, but those show only when their textures are loaded; the
Disable twin is sent otherwise, and for an empty slot.

#### Limits

- The stance event is a plain number for text or `TransformNumberMath`; change the
  picture with the texture events.
- A texture a script loads after the icon was chosen is picked up at the next
  stance, class or vehicle change.
- Setting `HealthTexture` on a soldier changes nothing else in the stock game: only
  the class loader and the ODF parser read it.
- A mission script can keep an older Lua icon swap for games without GameExt by
  checking `type(GameExt) == "table"` first; the check is on the type because
  `GameExt` can fall back to a plain `true`.

### Unit and weapon states

Float flags, `1` while a state lasts and `0` otherwise, for what the local player's
soldier and weapons are doing. They step straight between the two; ease them with a
[`TransformNumberLerp`](#transformnumberlerp).

| Event | 1 while | Read from |
|---|---|---|
| `player1.unit.state.sprint` | Sprinting | Soldier `mState` 3 (`SPRINT`) |
| `player1.unit.state.jump` | Jumping | `mState` 4 (`JUMP`) |
| `player1.unit.state.fall` | Falling | `mState` 8 (`FALL`) |
| `player1.unit.state.roll` | Rolling | `mState` 5 (`ROLL`) |
| `player1.unit.state.jet` | Jet jumping | `mState` 6 (`JET_JUMP`) |
| `player1.unit.state.hover` | Hovering on a jet pack | `mState` 7 (`JET_HOVER`) |
| `player1.unit.state.tumble` | Thrown, knocked down or getting up | `mState` 9 to 13 (`FLY`, `TUMBLE`, `BOUNCE`, `FLY_RECOVER`, `TUMBLE_RECOVER`) |
| `player1.unit.state.land` | One update after touching down | Jump, jet jump, hover or fall, then stand, crouch, prone, sprint, roll or slide |
| `player1.weaponN.state.firing` | Firing, or a melee attack | Weapon `mState` 1 or 2 (`FIRE`, `FIRE2`) |
| `player1.weaponN.state.charging` | Charging a shot | `mState` 3 (`CHARGE`), not melee |
| `player1.weaponN.state.reloading` | Reloading | `mState` 4 (`RELOAD`), not melee |
| `player1.weaponN.state.overheated` | Overheated, cooling down | `mState` 5 (`OVERHEAT`), not melee |
| `player1.weaponN.state.blocking` | A melee block | `mState` 4 on a weapon whose `IsMelee` is true |
| `player1.weaponN.state.shot` | One update after each shot | A new `mLastFireTime`, which `SignalFire` stores |

`N` is 1 or 2, as for the stock weapon events.

A melee weapon runs its attack through `FIRE` and its block through `RELOAD`, and
uses `OVERHEAT` for the recovery after a swing, so a melee weapon reports `firing`
and `blocking` and nothing for charge, reload or overheat. A swing also stores a fire
time, so it reads as a `shot`.

The unit flags come from the soldier in `Character::mUnit`, and only while the
Character has no vehicle and no remote: `EntitySoldier::EnterControllable`
deactivates the soldier without resetting `mState`, so a soldier that boards while
sprinting would otherwise read as sprinting for the whole ride. A droideka has its
own state machine and reads `0` throughout. A landing needs the soldier to go
straight from an airborne state to a grounded one; being thrown, dying or boarding
in between cancels it.

The weapon flags follow the weapon the stock `player1.weaponN.*` events show: the
remote's, else the vehicle or turret seat's, else the soldier's. Switching weapons is
not a shot.

Each flag is sent on the first HUD update of a mission and then only when it changes,
from the same update and under the same rule as the icon events: nothing is sent
unless a `.hud` binds at least one GameExt icon or state event. `land` and `shot` go
to `1` for one update and back to `0` on the next, so a consumer bound to them
directly sees a one-frame blink; a lerp with only a `FallTime` turns that into a flash.
At a fire rate above the HUD update rate, `shot` can stay at `1` across updates.

To hide the reticule while sprinting, ease `player1.unit.state.sprint` from `1` to `0`
with a lerp that has `OutputIsAlpha(1)`, and bind the result with `EventAlpha` on the
reticule's artwork child group, which also needs `PropagateAlpha(1)`. The outer group
keeps its stock `EventAlpha("player1.reticule.alpha")`. The recipe is in
[HUD.md](../user/HUD.md#unit-and-weapon-states).

### Floating target bars

`player1.weapon1.target.position` and `player1.weapon2.target.position` publish
Vector3 viewport-relative target anchors, so a target health bar can float over its
target. Bind with `EventPosition`, not `EventEnable`. Support is inherently on, but
inert unless the HUD binds the event. Sizing, artwork offsets, labels and fading
remain authored in the `.hud`.

#### Which target it shows

Selection retention replaces the old hit latch: no hit is required. A naturally
selected target accepted by the native HUD refreshes the hold; after selection
is lost, that target remains for 0.5 seconds by default. A different target
replaces it immediately. The retained target cannot refresh its own timer, and
retention performs no new line-of-sight/range/frustum check. The game's existing
selection rules still decide acquisition; this does not port SWBFIII autoaim.
Weapon channels retain targets independently, only while their position event
has a listener. Weapon/controlled-object changes reset retention.

A vehicle and its exposed rider trade the game's selection back and forth when the
crosshair is between them. The bar stays on whichever of the two was selected
first, and moves to the other only once that has held the selection for 0.3
seconds, so aiming at the rider on purpose still shows the rider. Any other target
replaces them at once. This changes only what the HUD shows; aim assist, lock-on
and firing see the game's own selection.

`[Features] TargetBarLatchSeconds=0.5` controls the hold, not the fade. Existing
INI overrides (including an old `2.5`) still apply; change them to `0.5` for
reference timing. Zero keeps the explicit no-timeout option.

#### Where the bar sits

The anchor is the top centre of the target's world bounding box, so it does not
wobble with animated bones. Units use bounds sized for their stance, vehicles their
model's bounds, and the anchor is snapped to whole pixels unless
`[Features] HudSubPixel` is on.

While the game has the target selected, its anchor is kept inside the screen's
safe area, so a big vehicle up close, whose top is above the screen, still shows
its bar. Once the selection is lost, during the hold and the fade, the bar follows
the target's true position and leaves the screen with it, clipped by the edge. A
target wholly behind the camera hides it. The safe area is built in, with no INI
setting for its size.

On death or removal, the bar retains its last live world-space anchor for the
existing HUD fade instead of dropping onto the corpse or sticking to your view. Its
screen position is recalculated as the camera moves. Living targets still move
normally during fade-out, and a newly selected target gets its own position.
Immediate zero-health hiding is distinct from the preserved native death fade and
is not implemented.

#### What can be authored entirely in the HUD

- `FadeInTime(0)` and `FadeOutTime(0.25)` give immediate appearance and a
  quarter-second fade after the DLL's hold ends. Put them on the existing
  independently enabled bar/glow/text elements. Keep their health/name enable
  and target/shield-name disable bindings.
- `BitmapRect`, `Scale`, child `Position`, alignment, text fonts/scales,
  `Alpha`, colour and blend mode control the artwork and labels as before.
  Position the artwork right/up from the anchor for the reference layout.
- Do not add the half-second hold again to the HUD fade; the DLL already delays
  `target.disable`. A longer fade alone cannot retain an object or keep its live
  health/name data updating.
- Keep `EventPosition` on the parent(s), never use that position event for
  `EventEnable`. Static parent position is replaced by the event; cosmetic
  offsets belong on children.

The BF3 `Common/hud/legacy_gameext_hud.hud` inspected for this revision has nine
target bar/glow/label `FadeOutTime(0.65)` entries. Changing those to `0.25` is an
asset-only way to match the reference's fade. That file was not edited here.

#### Distance

`player1.weapon1.target.distance` and `player1.weapon2.target.distance` are Floats in
metres, sent when they change: the distance from the translation of what the player
controls (remote, else vehicle, else soldier), as the stock lock-on distance measures,
to the target's translation. While the player is dead the camera stands in. The target
is the one the engine's HUD update settled on, so it includes the retention and rider
pair lends when a position is bound, and is the engine's own pick when only the
distance is. With no target the value is `FLT_MAX`, which a `Text` prints as nothing,
or as `--` with `InfiniteDashes(1)`. The stock `player1.lockOnDistance` exists only in
flyers and remote turrets; this works for everything.

### Reticule horizon levelling

This event keeps a reticule level with the horizon as the camera banks. Bind it on
the reticule's rotation pivot:

```text
EventRotation("player1.reticule.horizonRotation")
```

The Vector3 event carries `(0, 0, angleInDegrees)`, aligning the reticule's up
direction with projected world-up. It reads the active camera, not the unit or
vehicle pose, so it follows the view in first and third person, including
90-degree banks and inverted flight. It does not change aim, position, size,
mesh, visibility or alpha.

#### Keep rotation and artwork scaling on separate groups

Use an **unscaled pivot** at the reticule position. Keep the existing artwork's
scale and authored rotation inside a child group. Native `EventRotation` rebuilds
the transform from its current basis lengths; repeatedly rotating a group with
unequal X/Y scales can alter its proportions. A nonuniformly scaled ancestor can
also distort the final angle. The event replaces the pivot's rotation rather
than adding to a static `Rotation()` value.

For the existing `player1reticule_group`, retain its position/enable/alpha events
on the outer group, move its `Scale(0.750000,0.800000,0.750000)` into a new child,
and place all its existing reticule artwork inside that child:

```text
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
        // Existing reticule Model3D blocks go here, unchanged.
    }
}
```

This is a layout example, not a replacement containing your artwork. If only one
part should level with the horizon, put only that part inside the rotating pivot.
Do not put a screen-centred reticule's position on both parent and child: that
would translate it twice and make it orbit the wrong point.

Straight up/down has no unique horizon. Inside approximately 0.57 degrees of
vertical the event holds its last reliable direction, resuming beyond about
1.15 degrees to avoid jitter. A new camera/mission starts from zero if already
vertical. Crossing the pole can still reverse world-up by 180 degrees; that is
inherent in following world-up rather than tracking a continuous flight roll.

### Command post strip

A row of command post icons, each showing who owns the post and how far a capture
has got, as in Battlefront III. GameExt publishes one set of events per slot; the
`.hud` lays the slots out. Slot N runs 1 to 16:

| Event | Type | Carries |
|---|---|---|
| `player1.commandPosts.count` | Uint | How many slots are in use |
| `player1.commandPostN.icon` | Uint | The owning team's icon, the texture its `SetTeamIcon` gave it |
| `player1.commandPostN.iconDisable` | Bool | Sent instead when that team has no loaded icon |
| `player1.commandPostN.color` | Color | The owner in your palette: friendly, enemy or neutral, as on the minimap |
| `player1.commandPostN.capture` | Float | How much of the post its side holds, 0 to 1 |
| `player1.commandPostN.captureColor` | Color | The team gaining or holding it |
| `player1.commandPostN.disable` | Bool | The slot is not in use |

`capture` is 1 for a post a team holds, and drains while another team neutralises
it. A neutral post sits at 0 and fills as a team captures it, in that team's
`captureColor`. So one bar, coloured by `captureColor`, shows the whole fight.

Slots follow each post's `HUDIndex`, lowest first, then posts without one in map
order. Posts with `HUDIndexDisplay = 0` are left out: in the stock ODFs that is the
invisible spawn posts and the command vehicles. A post whose object has been
destroyed drops out and the rest move up.

#### The neutral icon

A neutral post belongs to team 0, so it shows team 0's icon. Give team 0 one where
the mission sets the other teams' icons:

```lua
SetTeamIcon(0, "bf3_neutral_icon")
```

Without it, neutral posts send `iconDisable`, so an icon element bound as below hides
while its post is neutral.

#### Laying out the strip

One group per slot, at a fixed spacing inside a row group. The icon enables on
`icon` and hides on `disable`; the capture bar is a `BarBitmap` over or under it:

```text
Group("cp_strip")
{
    EventPosition("player1.example.cpStripPosition")
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
    // cp_slot2 at Position(0.040, 0, 0), cp_slot3 at 0.080, and so on to 16.
}
```

To keep the used slots centred, move the row by the count. For a 0.040 spacing and
a centre at 0.5, the row starts at `0.5 - 0.02 * (count - 1)`:

```text
TransformNumberVector3("player1example_cpstripposition")
{
    NumberVector3(1.00, 0.50, 0.05, 0.00)
    NumberVector3(16.00, 0.20, 0.05, 0.00)
    EventInput("player1.commandPosts.count")
    EventOutput("player1.example.cpStripPosition")
}
```

Put the transform above the strip, as with any transform.

A `Scale` on the row group scales the slot spacing too. It pulls every slot toward
the group's origin, which is slot 1, so a smaller scale shrinks the row to the left;
`"Center"` alignment on the icons does not change that. Scale the second row with it:
its x is `centre - 7.5 * spacing * scale`, 0.32 for the 0.5 centre and 0.040 spacing
above at a 0.6 scale. Or put the `Scale` on each slot group instead, which leaves the
spacing alone.

To fill each emblem from the bottom rather than left to right, add `FillFrom("Bottom")`
to its capture bar; see [Filling upward or downward](#filling-upward-or-downward).

#### Markers in the world

`player1.commandPostN.position`, `.onScreen`, `.offScreen`, `.direction` and
`.distance` place a marker over each slot's post, computed only for slots a `.hud`
binds, and sent when they change. The recipe is in
[HUD.md](../user/HUD.md#markers-in-the-world).

- **Anchor:** the stock objective anchor, the post's collision-sphere centre lifted
  1.3 m along the post's up axis. Stock single-player conquest's `MapAddEntityMarker`
  markers sit at the same point.
- **Placement:** the stock `Target` element's rule. A point in front of the camera and
  inside the 0.9 safe square is on screen at its projection. Anything else is pinned to
  that square's edge along its direction on screen, mirrored behind the camera. Except
  while the player is in a flyer, a point more than about 78 degrees off the view axis
  slides to the left or right edge. Positions are snapped to whole pixels unless
  `[Features] HudSubPixel` is on, which also stops the draw rounding them.
- **Direction:** degrees for `EventRotation` in pixel space, so an arrow drawn
  pointing up turns toward the point, as the stock off-screen arrow does. Put it on a
  unit-scale pivot group.
- **Distance:** from what the player controls, else the camera, to the sphere's centre.
- **Visibility:** `onScreen` and `offScreen` are sent when the state changes, and
  `offScreen` also when the slot goes out of use, so `EventEnable`/`EventDisable` on
  them are enough to show a marker only while its post is in view. An element binds
  one of each (a second line replaces the first), which is why the slot's own
  `disable` is not needed there.
- **Not included:** occlusion. The stock markers dim behind walls with a ray per frame;
  these do not test line of sight.

#### Online

The events come from the HUD update, so they work on multiplayer clients too. Who
owns each post is always right there. The capture progress is only as good as the
client's own copy: a client works out a post's capture only while the post is near
its player, as the game itself does, so there the bar moves as normal. Elsewhere a
client sees each post at rest, full for its owner or empty when neutral, and the
change in owner when it happens. On the host and in single player every post's
progress is live.

## Bar fill direction

`FillFrom` on a `BarBitmap` sets which end of the bar stays put while its value
changes:

| Property | Kept edge | The bar grows |
|---|---|---|
| `FillFrom("Left")` | Left end; the stock behaviour and the default | Rightward |
| `FillFrom("Right")` | Right end | Leftward |
| `FillFrom("Bottom")` | Bottom edge | Upward |
| `FillFrom("Top")` | Top edge | Downward |

`BarBitmap` only, set in the `.hud` file; there is no INI setting. The property only
exists with GameExt: a game without it logs `Error reading parameter` for that line
and the bar fills from the left.

### Filling from the right

`FillFrom("Right")` makes the bar keep its right end and move its left edge with the
value, instead of the stock fixed left end and moving right edge.

The texture is neither stretched nor flipped: every point shows the same pixel the
full bar shows there. So two bars can share one picture, such as a health silhouette
with the missing part drawn in another colour. Feed the second bar the missing health,
`1 - health`, from a `TransformNumberMath`:

```text
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
    EventBitmap("player1.unit.healthTexture")
    // Same BitmapRect, TexCoords, Rotation, position and enable/disable
    // events as the health bar.
}
```

The health bar covers the picture up to the current health and this one covers the
rest; the two meet at the same point. The transform is the first block of the
[low-health recipe](#low-health-danger-intensity-a-nonlinear-response); declare it once
if you use both. It must come before the bar in load order, or the bar cannot find
its event. `ConstantA` comes first because `Subtract` is A minus B, the constant is
unquoted, and the clamp keeps bonus health from sending a negative value.

"Right" is the bar's own far end, the end a normal bar reaches when full, not the
screen's right: give both bars the same `Rotation` and they turn together.
`EventBitmap` works as on any bar, since it only swaps the texture, so the class and
stance icons drive both bars.

- Any `TexCoords` works, including ones that do not start at U 0 or are flipped,
  which the stock fill gets wrong.
- `ScaleTexture`, `BitmapRect` alignment and the flash (`FlashyScale`,
  `FlashyIncFadeOutTime`, `FlashyDecFadeOutTime`) behave as on any bar; the increase
  fade still plays when the bar grows and the decrease fade when it shrinks.

### Filling upward or downward

`FillFrom("Bottom")` keeps the bar's bottom edge and grows it upward; `FillFrom("Top")`
keeps the top and grows it downward. As with `"Right"`, each point shows the pixel the
full bar shows there, so an upright picture stays upright: no `Rotation` and no rotated
texture are needed. That is what lets a team emblem or class icon fill from the bottom,
where rotating the bar would turn the picture on its side.

```text
BarBitmap("player1cpstrip_slot1_fill")
{
    EventValue("player1.commandPost1.capture")
    FillFrom("Bottom")
    Bitmap("bf3_neutral_icon")
    EventBitmap("player1.commandPost1.icon")
    // Same BitmapRect and position as the emblem underneath.
}
```

- `ScaleTexture(0)` squeezes the whole texture into the moving bar, as on a stock bar.
- A vertical bar has no flash: `FlashyScale` and the fade times do nothing on it.
- `Rotation` still applies on top, like any bar.

## TrueWidescreen files

`TrueWidescreen(1)` in a file's `FileInfo` takes that file out of the stock wide-screen
handling. Any number other than 0 turns it on, and `true` and `false` work too; a quoted
value or an empty line is logged once under `[TrueWidescreen]` in `BF2GameExt.log` and the
file keeps the stock layout. It applies on a screen the game treats as wide (height
below 3/4 of the width) with one viewport, both read as the file loads; otherwise the
file draws the stock way.

### What the stock game does at 16:9

| | Stock | TrueWidescreen |
|---|---|---|
| `"Viewport"`/`"Screen"` x and widths | fractions of the real width | fractions of W43 = 4/3 × the height (960 at 720p) |
| `"Pixels"` | screen pixels | pixels of the 4:3 layout |
| Heights | in a band of 8/9 the height, 40 pixels short at top and bottom at 720p | the full height |
| Bitmaps | 32/27 as tall as wide for a square `BitmapRect` | as written |
| Minimap and segmented-bar rings | stretched to make up for the width | as written |
| Text | 8/9 as tall | as written |

### Where each piece goes

An element at the top of the file is a piece. After the file loads, each piece is kept
to the edge nearest where its position puts it in the 4:3 layout: left of W43/3 it stays
where the layout puts it, right of 2/3 × W43 it moves right by the width beyond the
layout (320 pixels at 1280×720), and between them by half that. An element inside a
piece moves with it.

A top-level element whose position is exactly (0, 0), written so or left out, is a
plain container rather than a piece: each element directly inside it is placed as a
piece instead. A `Target` element is never moved: `ElementTarget` places its markers
over their targets on the real screen.

Positions sent later through `EventPosition` are read in the layout too, so a
`TransformNumberVector3` output means the same on every screen. Four kinds of position
follow the world instead, and arrive as fractions of the real screen; for a piece in a
TrueWidescreen file each is turned into the layout fraction that lands on the real point,
whatever edge the piece keeps to:

- `player1.weapon1.reticule.position`, `player1.weapon2.reticule.position`, with
  `[Fixes] ReticleCorrection` taken back off their y
- `player1.weapon1.lockOnPosition`, `player1.weapon2.lockOnPosition`
- `player1.weapon1.target.position`, `player1.weapon2.target.position`
- `player1.commandPost1.position` to `player1.commandPost16.position`

Each opted-in file logs one line to the game's log as it loads on a wide screen, for
example
`[TrueWidescreen] a .hud file laid out for 1280x720 as on 960x720: 17 pieces kept to the
left, 5 centred, 0 to the right (1 plain containers, 0 Target elements)`.

### Limits

- A piece always keeps to its nearest edge; it cannot pick another.
- 512 pieces across all opted-in files; more are logged once and keep the stock layout.
- Other files are not affected, so a TrueWidescreen file and a stock one can be loaded
  together, but their elements line up differently on a wide screen.
- The modtools HUD editor saves a TrueWidescreen file in its 4:3 numbers and keeps the
  line. GameExt keeps the editor off on Steam and GOG.
- A game without BF2GameExt logs `Error reading parameter` for the line and draws the
  file the stock way, which looks off: its numbers are for the 4:3 layout.

[HUD.md](../user/HUD.md#writing-the-numbers) has the conversion from stock 16:9 numbers.

## Transforms

A transform turns an event's value into a new event for elements to bind.
`TransformNumberMath`, `TransformNumberLerp` and `TransformNumberCompare` come with
GameExt; the other four classes are native engine features.

### Choose a transform

| Class | Input | Output | Use it for |
|---|---|---|---|
| `TransformNumberMath` | Two operands: numeric events or constants | Float | Arithmetic, ratios, offsets, clamping, combining live values |
| `TransformNumberLerp` | A numeric event placed in 0..1 or `InputRange`, plus two ends: numeric events or constants | Float | Fades and flashes: easing between two values over time; remapping a range |
| `TransformNumberCompare` | Two operands: numeric events or constants | Float, Bool | Thresholds: warnings that show, hide or fade as a value crosses a line |
| `TransformNumberColor` | One numeric event, with optional input factors | Color | Health/ammo tints and piecewise colour gradients |
| `TransformNumberVector3` | One numeric event, with optional input factors | Vector3 | Driving position, scale or rotation from a number |
| `TransformNumberColorBlend` | Numeric mapping input, plus a Color event and optional numeric blend-weight event | Color | Blending mapped RGB with another colour |
| `TransformNameMesh` | Uint containing a name hash | Model | Selecting a loaded HUD mesh, such as a weapon icon |

`Transform` and `TransformNumberType` are native base classes, **not additional
authorable factory keywords**. `TransformNumberType` is not a numeric conversion
or arithmetic transform.

Trail, timeline, gate and select transforms are not implemented by this work; a
lerp with no times, fed a 0/1 input, already picks between A and B. Do not put
those proposed class names into a production `.hud`.

No transform here reads a bone, projects world geometry, scales with distance
automatically, invents missing gameplay events, or keeps a history. The one with
time in it is `TransformNumberLerp`, which eases toward its latest input at a set
rate and remembers nothing else.
The floating healthbar and horizon-rotation events are separate event producers.
Their bindings are documented in [Floating target bars](#floating-target-bars)
and [Reticule horizon levelling](#reticule-horizon-levelling) above.

### TransformNumberMath

#### Capability

Calculates `A operation B`, optionally clamps the result, and publishes a Float.
Either operand can be a constant or the latest value from a numeric event. All
four pairings work: constant/constant, event/constant, constant/event, event/event.
No INI setting is needed; declaring the transform in your `.hud` is the opt-in.

```text
TransformNumberMath("player1example_ticketlead")
{
    Operation("Subtract")
    EventInputA("player1.team1.reinforcements")
    EventInputB("player1.team2.reinforcements")
    EventOutput("player1.example.ticketLead")
}
```

For this team-1-minus-team-2 example, bind a test Text element using
`EventNumber("player1.example.ticketLead")` and `FloatFormat("%.0f")`. Either
team's updates recalculate the difference. It is not necessarily the local
player's lead; leave it unclamped if negative values should remain visible.

#### Complete parameter list

These eight keys are the complete accepted property set for the new class.

| Parameter | Arguments | Required/default | Accepted values and meaning |
|---|---|---|---|
| `Operation("name")` | Exactly one string | Required; no authored default | `Add`, `Subtract`, `Multiply`, `Divide`, `Min`, `Max`; case-insensitive, no symbols or other aliases |
| `ConstantA(value)` | Exactly one number | Choose this **or** `EventInputA` | Finite numeric literal; zero, negative and fractional values are allowed |
| `EventInputA("event")` | Exactly one string | Choose this **or** `ConstantA` | Existing Int, Uint or Float event; nonempty name, at most 240 bytes |
| `ConstantB(value)` | Exactly one number | Choose this **or** `EventInputB` | Same numeric rules as `ConstantA` |
| `EventInputB("event")` | Exactly one string | Choose this **or** `ConstantB` | Same event rules as `EventInputA` |
| `Clamp(minimum, maximum)` | Exactly two numbers | Optional; no clamp if absent | Both finite; minimum must be ≤ maximum; applies to the final result |
| `EventOutput("event")` | Exactly one string | Required | New Float event; nonempty name, at most 240 bytes; must not already exist |
| `OutputIsAlpha(flag)` | Exactly one number, or `true`/`false` | Optional; off | On: the result is sent every HUD update, not only on change, for a group's `EventAlpha`. `1` or `true` is on, `0` or `false` off; a quoted value is invalid |

A comment after a property on the same line is fine: the munger turns its words
into more arguments, and the reader drops everything from the `//` on. Leave a
space after the `//`. Written `//comment`, it is one hashed word that cannot be
told from an argument, and the transform switches off with a log line saying so.

Every operand must have exactly one source. Repeating a property, even with the
same value, disables that transform. Repeating `ConstantA` as `EventInputA` is
also a duplicate source, not a fallback/default value.
Property order within a block does not matter for ordinary existing inputs;
the ordering of separate producers and consumers still matters.

Numbers are unquoted: `ConstantB(100.00)`, not `ConstantB("100.00")`. `Clamp(0, 1)`
is valid; `Clamp(1, 0)` is invalid. Equal bounds are valid and produce that fixed
value whenever the underlying arithmetic is valid.

These native-transform properties are **not** accepted by Math: `EventInput`,
`InputFactor`, `EventInputFactor`, `WrapInput`, `NumberColor`, `NumberVector3`,
`Alpha`, `BlendMode`. There is also no `Expression`, `Default`, `Else`, `Offset`,
`EventReset` or third operand. Use separate, ordered transforms for extra steps.

#### Operations

| Operation | Formula | Example with A = 8, B = 3 | Typical purpose |
|---|---|---|---|
| `Add` | A + B | 11 | Combined total or fixed offset |
| `Subtract` | A - B | 5 | Team advantage, difference, `1 - fraction` |
| `Multiply` | A × B | 24 | Gain, percentage conversion, squaring a value |
| `Divide` | A / B | Approximately 2.666667 | Ratios and normalisation; not integer division |
| `Min` | Smaller of A and B | 3 | Upper cap against another live value |
| `Max` | Larger of A and B | 8 | Lower floor against another live value |

Order matters for Subtract and Divide. `Min` and `Max` return numbers, not Bool
events or identifiers saying which input won. There is no modulo, power, square
root, rounding or trigonometry operation. Squaring is possible by multiplying a
value by itself; arbitrary exponentiation is not.

#### Numeric types, precision and ranges

- Int: signed 32-bit event value, from -2,147,483,648 to 2,147,483,647.
- Uint: unsigned 32-bit event value, from 0 to 4,294,967,295.
- Float: finite 32-bit floating-point value, approximately ±3.4028235 × 10^38.
- Constants are numeric HUD literals stored as 32-bit floats by the config format.
- Arithmetic uses double intermediates; the output is always a 32-bit Float.
  Whole-number outputs above 16,777,216 can lose integer precision.

Bool, String, Color, Vector3, Model and Texture are not numeric inputs. A
numeric-looking String event is still a String; no automatic parsing occurs.
Math has no intrinsic units: choose compatible counts, fractions, seconds or
distances yourself. A property whose name ends in `Fraction` is not proof that
its runtime type is Float, or that its range is confined to 0..1.

Clamping occurs **after** arithmetic and before Float conversion. For example,
a large but finite double result can be contained by `Clamp(0, 1)`. Clamp does
not repair invalid inputs or division by zero.

#### Update and failure behaviour

- Waits until each event-fed operand has received a valid value. It does not
  query the game for a current value or invent an initial zero.
- Either operand's event recomputes the result from both cached operands.
- If both operands bind the **same** event, both update before calculation.
- First publication is delayed until a HUD update after loading, allowing
  consumers to bind. Constant-only transforms also publish then.
- After activation, publication is synchronous with input events. Unchanged
  Float results are not resent; idle frames do not produce repeated output,
  unless `OutputIsAlpha(1)` asks for the last result every HUD update. Leave it
  off for `EventChanged`, which would then fire every frame.
- Separate input events are **not an atomic pair**. If two related sources
  update in sequence, an intermediate result can use one new and one old value.
  The final value follows the second update. Math does not buffer a whole frame.
- Division by +0 or -0, NaN/infinity and results outside Float range send nothing.
  Consumers keep their last valid value, or their authored default if none was
  published. A later valid input resumes normal calculation.
- An invalid runtime input invalidates that operand until it receives a valid
  replacement. Changing only the other operand does not make it valid again.
- Configuration errors disable that transform for its current instance. Correct
  the source, munge and reload; later input events do not repair configuration.
- A disable/death/weapon-change event does not automatically reset cached operands.
  Keep the appropriate native enable/disable bindings on your consuming artwork.
- State and subscriptions are removed by normal mission HUD teardown. Feedback
  is forbidden; a runtime guard also caps nested synchronous math dispatch at
  32 levels. This is not a limit of 32 transforms in the whole HUD.

Math is currently **source-authored only**. Native HUD-editor export omits these
blocks. Do not overwrite the source transform file with an editor export.

### TransformNumberLerp

#### Capability

Publishes `A + (B - A) × s` as a Float. `s` follows the input, held to 0..1, but
moves at no more than `1 / RiseTime` per second toward a higher input and
`1 / FallTime` per second toward a lower one. A and B are each a constant or the
latest value of a numeric event, and default to 0 and 1, so a lerp with only times
set is its input, eased. No INI setting is needed; declaring it is the opt-in.

A and B do any inverting: `ConstantA(1)` with `ConstantB(0)` turns a rising input
into a falling output with no Math stage. The input is not limited to 0 and 1: any
value between sets `s` part way, so a health fraction comes out eased, and
`InputRange(min, max)` places any other range, such as `InputRange(0, 30)` for a
clip. Each end of the range is a number or a quoted event name.

```text
TransformNumberLerp("player1example_reloaddim")
{
    EventInput("player1.weapon1.state.reloading")
    ConstantA(1.00)
    ConstantB(0.35)
    RiseTime(0.20)
    FallTime(0.20)
    EventOutput("player1.example.reloadDim")
    OutputIsAlpha(1)
}
```

Bound with `EventAlpha` on a group with `PropagateAlpha(1)` around the weapon icon,
this dims the icon to 35% over a fifth of a second as a reload starts and brings it
back as the reload ends.

#### Complete parameter list

These ten keys are the complete accepted property set.

| Parameter | Arguments | Required/default | Accepted values and meaning |
|---|---|---|---|
| `EventInput("event")` | Exactly one string | Required | Existing Int, Uint or Float event; nonempty name, at most 240 bytes. Placed in `InputRange`; values past either end count as that end |
| `InputRange(min, max)` | Exactly two, each a number or a string | Default 0, 1 | The inputs that give `s` = 0 and 1. A string is an existing Int, Uint or Float event, followed as it changes. Two equal numbers are invalid; a reversed range is fine |
| `ConstantA(value)` | Exactly one number | Default 0; or `EventInputA` | Finite numeric literal: the output at 0 |
| `EventInputA("event")` | Exactly one string | Or `ConstantA` | Existing Int, Uint or Float event for the output at 0 |
| `ConstantB(value)` | Exactly one number | Default 1; or `EventInputB` | Finite numeric literal: the output at 1. B may be below A |
| `EventInputB("event")` | Exactly one string | Or `ConstantB` | Existing Int, Uint or Float event for the output at 1 |
| `RiseTime(seconds)` | Exactly one number | Default 0 | Finite, 0 or more: the time for `s` to go from 0 to 1. 0 follows at once |
| `FallTime(seconds)` | Exactly one number | Default 0 | Finite, 0 or more: the time for `s` to go from 1 to 0. 0 follows at once |
| `EventOutput("event")` | Exactly one string | Required | New Float event; nonempty name, at most 240 bytes; must not already exist |
| `OutputIsAlpha(flag)` | Exactly one number, or `true`/`false` | Optional; off | On: the result is sent every HUD update, not only on change, for a group's `EventAlpha`. `1` or `true` is on, `0` or `false` off; a quoted value is invalid |

As with Math, a constant and an event for the same end are a duplicate, as is any
repeated key, and either disables the transform. Trailing comments follow Math's
rule. `Operation`, `Clamp` and the native
`InputFactor`, `EventInputFactor` and `WrapInput` are not accepted. `RiseTime` and
`FallTime` refer to the input: with `ConstantA(1)` and `ConstantB(0)` the output
falls over `RiseTime` while the input rises.

#### Update and failure behaviour

- The first valid input sets `s` at once, so nothing eases in at load. After that,
  `s` moves each HUD update by the time since the last one, capped at 0.1 s, so a
  hitch or a pause cannot jump it. The time is the wall clock between updates on
  every build: Steam and GOG pass the HUD update no time step.
- A direction with a time of 0 moves `s` inside the input's own callback, so a
  change that lasts one update (`land`, `shot`) always reaches the output in full.
- The rate is for a whole swing, so part of one takes that share of the time. A
  swing that reverses part way turns round from where `s` got to.
- A change to A or B republishes at once; only `s` is eased. With an event end,
  nothing is published until that event has sent a valid value.
- A non-finite input holds `s` where it is until a valid one arrives. A non-finite
  end holds the last output.
- With an event for an end of `InputRange`, nothing is sent until it has arrived. A
  later change moves the target, and `s` travels there at the usual rate. Equal ends
  from events hold `s` where it is.
- Publication, the first-update delay, change suppression, output naming, player
  filtering, feedback limits and teardown are the same as Math's, and so are the
  `[HudNumberMath]` log lines. A lerp can take a Math result or another lerp's as
  any of its inputs, and feed either, in declaration order.

The native shell has two handlers, so the lerp's B event binds a third handler the
DLL owns, through the same `Item::ReadEvent`, and unlinks it itself when the item is
destroyed; the unlink is the one `EventHandler`'s destructor performs on every build.
Details are in [HUDSystem.md](HUDSystem.md#transformnumberlerp).

### TransformNumberCompare

#### Capability

Compares A with B and publishes 1 or 0 as a Float, and optionally Bool events as the
result turns on and off. A and B are each a constant or the latest value of a
numeric event, as in Math. It exists for thresholds the stock HUD cannot express:
low ammo, low health, full charge, a lead lost.

```text
TransformNumberCompare("player1example_lowhealth")
{
    Operation("Less")
    EventInputA("player1.healthFraction")
    ConstantB(0.25)
    Hysteresis(0.05)
    EventOutput("player1.example.lowHealth")
    EventOutputTrue("player1.example.lowHealthOn")
    EventOutputFalse("player1.example.lowHealthOff")
}
```

#### Complete parameter list

| Parameter | Arguments | Required/default | Accepted values and meaning |
|---|---|---|---|
| `Operation("name")` | Exactly one string | Required | `Greater`, `GreaterOrEqual`, `Less`, `LessOrEqual`, `Equal`, `NotEqual`: A compared with B; case-insensitive, no symbols |
| `ConstantA(value)` / `EventInputA("event")` | As in Math | One required | The left side |
| `ConstantB(value)` / `EventInputB("event")` | As in Math | One required | The right side |
| `Hysteresis(amount)` | Exactly one number | Default 0 | Finite, 0 or more. Once on, `Greater` stays on while A > B − amount, `Less` while A < B + amount, and the `OrEqual` forms alike. For `Equal`, on while \|A − B\| ≤ amount; `NotEqual` the reverse |
| `EventOutput("event")` | Exactly one string | One output required | New Float event: 1 or 0 |
| `EventOutputTrue("event")` | Exactly one string | One output required | New Bool event, sent as the result turns to 1 |
| `EventOutputFalse("event")` | Exactly one string | One output required | New Bool event, sent as the result turns to 0 |
| `OutputIsAlpha(flag)` | As in Math | Optional; off | Repeats the Float every HUD update; the Bool events never repeat |

#### Update and failure behaviour

- The first result is published once both sides are known: the Float, and the Bool
  event that matches it. After that, each change sends the Float and the other Bool
  event. The Bool events are never sent twice in a row, so `EventEnable` and
  `EventDisable` on them play their fades once per change.
- An invalid side holds the last result until a valid value arrives.
- Comparisons are on the values as sent, in double precision: an Int or Uint side
  compares exactly, and `Equal` on two Floats usually wants a `Hysteresis`.
- Publication, activation, output naming, filtering, feedback limits, comments and
  teardown are the same as Math's.

### Shared native numeric parameters

`TransformNumberColor` and `TransformNumberVector3` accept all five parameters
below in addition to their repeated mapping rows. `TransformNumberColorBlend`
inherits the parser too, but has a factor caveat explained in its own section.
`TransformNameMesh` accepts only `EventInput` and `EventOutput` from this table.
None of this table is an extension of Math's parameter set.

| Parameter | Arguments | Default / requirement | Meaning |
|---|---|---|---|
| `EventInput("event")` | One event-name string | Needed to drive the mapping | Int, Uint or Float for numeric transforms; Uint name hash for NameMesh |
| `EventOutput("event")` | One event-name string | Needed to publish | Creates/gets the class-specific Color, Vector3 or Model event |
| `InputFactor(number)` | One finite numeric literal | 1 | Constant multiplier on the primary numeric input |
| `EventInputFactor("event")` | One event-name string | Optional; cached factor begins at 1 | Int, Uint or Float supplying another multiplier |
| `WrapInput(flag)` | One numeric flag | 0 | Zero clamps input to mapping endpoints; any nonzero value enables native wrapping; author 0 or 1 |

For the ordinary Color and Vector3 transforms:

```text
lookup value = EventInput value × InputFactor × latest EventInputFactor
```

`EventInputFactor` only caches its value. **It does not publish a new result**;
the next primary `EventInput` causes mapping and publication. Unlike Math,
these are not symmetric two-input operators. Use Math Multiply upstream when a
change to either input must immediately recalculate the result.

Mapping inputs need not be fractions: keys can be health amounts, seconds,
degrees or other numbers. Write finite keys in **strictly increasing order**.
Repeated keys are not a supported way to request a hard step. Values between
adjacent rows interpolate linearly; identical values across two rows make a
plateau. Native interpolation does not add temporal easing or smoothing.

With `WrapInput(0)`, input below/above the first/last key holds the endpoint.
Native readers cap each mapping table at 256 rows. Use at least one row; two or
more make a useful interpolated range. Empty native tables are not a safe
configuration, particularly for ColorBlend. These readers do not provide Math's
validation/failure guarantees.

#### Wrapping is not a general modulo operator

`WrapInput(1)` works best for a nonnegative input and a zero-based mapping range
with two distinct endpoint keys. The reference implementation preserves the
exact upper endpoint, wraps values above it, and has unusual negative/nonzero-
origin behaviour. For a 0..1 range, -0.25 becomes +0.25, not the conventional
modulo result 0.75. Avoid using it for general signed arithmetic, a single-key
table, or arbitrary nonzero-origin ranges.

Unlike Math's whitelist, native parsers may only warn about some bad arguments
or accept extra arguments. Use the documented arities and ranges; permissive
parsing is not a supported capability. Do not rely on repeated singleton settings.

### TransformNumberColor

#### Parameters and output

Accepts the five [shared numeric parameters](#shared-native-numeric-parameters),
plus:

| Parameter | Arguments | Values |
|---|---|---|
| `NumberColor(key, red, green, blue)` | Exactly four numbers per mapping row | Finite numeric key; RGB channels authored as integers 0..255 |

The output is **Color**, not a number or vector. Native rows set colour alpha to
255; `NumberColor` does **not** take a fifth alpha argument. The reader converts
channels to bytes, so do not rely on out-of-range values being safely clamped.
Interpolated channel values are rounded to byte colours.

#### Example: health warning tint

```text
TransformNumberColor("player1example_healthtint")
{
    NumberColor(0.00, 255, 30, 20)
    NumberColor(0.25, 255, 30, 20)
    NumberColor(0.50, 255, 190, 40)
    NumberColor(1.00, 0, 150, 255)
    EventInput("player1.healthFraction")
    EventOutput("player1.example.healthTint")
}
```

Bind `EventColor("player1.example.healthTint")` on a test copy of your health
artwork. It stays red below 25%, interpolates towards amber at 50%, then blue at
100%. Above 100% it stays blue because wrapping defaults off. This is the same
mapping mechanism used by your existing health/vehicle/energy colour transforms.

### TransformNumberVector3

#### Parameters and output

Accepts the five [shared numeric parameters](#shared-native-numeric-parameters),
plus:

| Parameter | Arguments | Values |
|---|---|---|
| `NumberVector3(key, x, y, z)` | Exactly four numbers per mapping row | Finite numeric key and three finite vector components; negative values are allowed |

Each component interpolates independently. This creates a Vector3 **from a
scalar**, not vector addition or multiplication. The receiving property defines
the units:

- `EventScale`: dimensionless X/Y/Z scale values.
- `EventRotation`: X/Y/Z rotation in degrees.
- `EventPosition`: position interpreted in the receiving group's authored
  relative-coordinate mode, not automatically world space or viewport fractions.

It supplies the property value, not an automatic offset from the existing
property. Use a parent/child layout when authored placement and animated offsets
or scale need separate control.

#### Example: a shrinking marker

```text
TransformNumberVector3("player1example_healthmarkerscale")
{
    NumberVector3(0.00, 0.70, 0.70, 1.00)
    NumberVector3(1.00, 1.00, 1.00, 1.00)
    EventInput("player1.healthFraction")
    EventOutput("player1.example.healthMarkerScale")
}
```

Bind `EventScale("player1.example.healthMarkerScale")` on an appropriate group
containing the marker. At 50% health, X/Y scale is 0.85. The Z component stays 1.

#### Example: looping rotation driven by time

```text
TransformNumberVector3("player1example_spinner")
{
    NumberVector3(0.00, 0.00, 0.00, 0.00)
    NumberVector3(1.00, 0.00, 0.00, 360.00)
    InputFactor(0.25)
    WrapInput(1)
    EventInput("time")
    EventOutput("player1.example.spinnerRotation")
}
```

For a seconds-valued `time` input, this is one turn every four seconds. Bind
`EventRotation("player1.example.spinnerRotation")` on an **unscaled pivot**;
put artwork sizing in a child. This uses an existing updating clock event; the
transform itself does not create a timer. See
[Reticule horizon levelling](#reticule-horizon-levelling) for the native
nonuniform-scale/rotation caveat and horizon-levelling event.

### TransformNumberColorBlend

#### What it actually blends

First maps the primary numeric input through `NumberColor` rows. Then combines
that mapped RGB colour with the latest Color supplied through `EventBlend`.
It is **not** a generic two-Color-input lerp, and does not switch renderer blend
state. Its output remains a Color for `EventColor`.

#### Complete parameter set

| Parameter | Arguments | Default / accepted values |
|---|---|---|
| `NumberColor(key, r, g, b)` | Four numbers; repeat for mapping rows | Same key/order/RGB/256-row rules as Color; supply a nonempty table |
| `EventInput("event")` | One event-name string | Int, Uint or Float; the primary trigger that calculates and sends |
| `EventOutput("event")` | One event-name string | Color output |
| `EventBlend("event")` | One event-name string | Color only; caches the other colour |
| `BlendMode("mode")` | One string | `"Alpha"` (default) or `"Additive"`; not `Multiply`, `Screen` or `Normal` |
| `Alpha(value)` | One number | Blend weight in 0..1; default internal byte 127, approximately 0.498 |
| `EventAlpha("event")` | One event-name string | Float weight in 0..1, or Int/Uint weight in 0..255 |
| `WrapInput(flag)` | One numeric flag | 0; nonzero enables wrapping; author 0 or 1; native mapping wrap rules apply |
| `InputFactor(number)` | One number | Inherited/parsed; **reference input handler does not apply it** |
| `EventInputFactor("event")` | One numeric event name | Inherited/parsed/cached; **reference input handler does not apply it** |

Use Math upstream if this blend's numeric input needs scaling. Ordinary Color
and Vector3 apply their factors, but the reference ColorBlend override passes
its raw primary input to the colour lookup.

`EventAlpha` here controls **colour blend weight**, not element opacity. `Alpha`
is the initial weight and later alpha events replace it. The native handler
converts float weights by truncating `weight × 255`; integer inputs supply the
byte value directly. Out-of-range weights are not safely clamped—keep the values
in the ranges above or clamp a Math output upstream.

#### Blend equations and timing

Let `M` be a mapped colour channel, `B` the cached `EventBlend` channel, and `a`
the weight byte (0..255). For the reference implementation:

```text
Alpha:    floor(a × M / 256) + floor((255 - a) × B / 256)
Additive: min(255, B + floor(a × M / 256))
```

This integer implementation is close to familiar alpha/additive colour mixing,
but its `/256` arithmetic means endpoints are not mathematically exact `/255`
lerp endpoints. The mapped colour's alpha remains separate; this does not produce
a fading-opacity event.

Only the primary `EventInput` sends output. `EventBlend` and `EventAlpha` update
cached values but do **not** independently republish. Ensure the blend-colour
producer is declared first and actually sends before the first primary trigger.
The reference constructor does not establish a reliable authored base colour
when no blend event has arrived; do not treat an unbound blend as black.

#### Example: soften a health tint against white

Both transforms use the same health source. Declare the base producer first so
its Color event is sent to the blend before that blend handles the health input.

```text
TransformNumberColor("player1example_blendbase")
{
    NumberColor(0.00, 255, 255, 255)
    NumberColor(1.00, 255, 255, 255)
    EventInput("player1.healthFraction")
    EventOutput("player1.example.blendBase")
}

TransformNumberColorBlend("player1example_softhealthtint")
{
    NumberColor(0.00, 255, 30, 20)
    NumberColor(1.00, 0, 150, 255)
    EventBlend("player1.example.blendBase")
    BlendMode("Alpha")
    Alpha(0.50)
    EventInput("player1.healthFraction")
    EventOutput("player1.example.softHealthTint")
}
```

Bind `EventColor("player1.example.softHealthTint")` on the artwork. Use `EventAlpha`
on a group around it if opacity should also vary. An element's own
`BlendMode("Additive")` is a different rendering setting; see
[HUD blend modes](../RE/HUDBlendMode.md) for bitmap/model limitations.

### TransformNameMesh

#### Parameters and event types

| Parameter | Arguments | Meaning |
|---|---|---|
| `NameMesh("sourceName", "meshName")` | Exactly two strings; repeatable | Map the hash of a source name to a loaded model/mesh name |
| `TransformNameMesh("otherTransformName")` | Exactly one string inside the block | Copy mapping entries from an already-defined NameMesh transform; not a nested child scope |
| `EventInput("event")` | One event-name string | Uint name-hash event; commonly `player1.weapon1.change` |
| `EventOutput("event")` | One event-name string | Model event for `EventMesh` |

It does not accept `Operation`, factors, wrapping, colours or vectors. A String
containing a weapon label is not the same as a Uint name hash, and a Math Float
output is not a usable name-hash input even if its value looks like an integer.

Tables are limited to 256 mappings, including inherited entries. Up to 16
inheritance names are recorded for native write-back. Stay within both limits;
avoid duplicate source keys or instance names, and do not rely on inheritance
as an override-priority mechanism. Name-key tables do not require numeric
ascending order like `NumberColor`/`NumberVector3` rows.

#### Example: weapon HUD mesh selection

Your file already uses entries such as this weapon-icon mesh name:

```text
TransformNameMesh("player1example_weaponicons")
{
    NameMesh("hud_model_primary_republic_rifle", "hud_model_primary_republic_rifle")
    NameMesh("hud_model_primary_republic_pistol", "hud_model_primary_republic_pistol")
    EventInput("player1.weapon1.change")
    EventOutput("player1.example.weaponMesh")
}
```

On a `Model3D` element, bind:

```text
EventMesh("player1.example.weaponMesh")
```

The first string must match the name whose hash the source event actually
publishes; do not assume it is the translated weapon display label. The second
must name a model present in the loaded assets. `NameMesh` selects assets; it
does not load arbitrary files from disk or create geometry.

The native handler ignores zero/non-Uint input. On an absent/failed mapping,
the stock reference path can fall back to finding a loaded model by the incoming
hash; if nothing resolves, no new model event is sent. The previous displayed
mesh may therefore remain until your normal visibility bindings hide it.
BF2GameExt's `WeaponIconFix` adjusts cross-table resolution to prevent unrelated
fallback models when mods add mappings; it is not a new `.hud` parameter.

`MeshInfo` is an optional per-mesh layout override on a **Model3D element**, not
a NameMesh parameter and not a required mesh registration step. Likewise,
`InheritMeshInfo` is not this transform's table-inheritance keyword.

### Practical math recipes

These are source blocks to place before their consumers, not complete artwork
definitions. Keep fonts, positions, textures and native visibility rules on the
existing elements. All outputs use the `player1.example.` namespace.

#### Low-health danger intensity: a nonlinear response

```text
TransformNumberMath("player1example_missinghealth")
{
    Operation("Subtract")
    ConstantA(1.00)
    EventInputB("player1.healthFraction")
    Clamp(0.00, 1.00)
    EventOutput("player1.example.missingHealth")
}

TransformNumberMath("player1example_dangerintensity")
{
    Operation("Multiply")
    EventInputA("player1.example.missingHealth")
    EventInputB("player1.example.missingHealth")
    EventOutput("player1.example.dangerIntensity")
    OutputIsAlpha(1)
}
```

Bind `EventAlpha("player1.example.dangerIntensity")`, with `PropagateAlpha(1)`, on
a group around a dedicated warning overlay or a test copy of
`player1health_colourchange`. Opacity is 0 at full
health, 0.25 at half health and 0.5625 at quarter health. The clamp prevents bonus
health above 100% from generating a negative deficit that would square positive.
Keep the normal health-disable behaviour: this is not a persistent death overlay.

#### Relative reinforcement balance

```text
TransformNumberMath("player1example_totaltickets")
{
    Operation("Add")
    EventInputA("player1.team1.reinforcements")
    EventInputB("player1.team2.reinforcements")
    EventOutput("player1.example.totalTickets")
}

TransformNumberMath("player1example_ticketbalance")
{
    Operation("Divide")
    EventInputA("player1.team1.reinforcements")
    EventInputB("player1.example.totalTickets")
    Clamp(0.00, 1.00)
    EventOutput("player1.example.ticketBalance")
}
```

For nonnegative ticket counts, equal teams produce 0.5; 150 versus 50 produces
0.75. Bind `EventValue("player1.example.ticketBalance")` to a new central bar
using your reinforcement artwork. This is team 1's relative share, not necessarily
the local player's share. Both zero holds the last valid result. Different paths
through this graph can briefly publish intermediate values while a source event
propagates; it is not a frame-atomic scoreboard calculation.

To turn that number into a position, append:

```text
TransformNumberVector3("player1example_ticketmarker")
{
    NumberVector3(0.00, 0.35, 0.08, 0.00)
    NumberVector3(1.00, 0.65, 0.08, 0.00)
    EventInput("player1.example.ticketBalance")
    EventOutput("player1.example.ticketMarkerPosition")
}
```

On the receiving marker group, use `Position(0, 0, 0, "Viewport")` to establish
viewport-relative units and `EventPosition("player1.example.ticketMarkerPosition")`.
Use an appropriately positioned parent; do not also add a screen-centre offset
that unintentionally translates those coordinates a second time.

#### Finite-ammo magazine / reserve counter

```text
TransformNumberMath("player1example_reserveammo")
{
    Operation("Subtract")
    EventInputA("player1.weapon1.totalAmmoBullets")
    EventInputB("player1.weapon1.totalClipBullets")
    EventOutput("player1.example.reserveAmmo")
}
```

In test copies of `player1info_weapon1ammo`, use
`EventNumber("player1.weapon1.totalClipBullets")` for the magazine and
`EventNumber("player1.example.reserveAmmo")` with `FloatFormat("%.0f")` for reserve.
For 120 total rounds with 30 loaded this represents 30 / 90.

This is a **conditional example**, not a universal ammo-counter fix. The reference
build includes the clip in total ammo, but suppresses total-ammo publication while
scoped. Math cannot update a source the game stops sending. Infinite ammo also
uses sentinel values that must not be treated as ordinary counts. Test finite
ammo unscoped first; retain weapon disable/infinite-ammo presentation logic.

#### Optional bar-and-text test: remaining heat capacity

For a simple inversion and percentage chain using existing weapon events:

```text
TransformNumberMath("player1example_heatremaining")
{
    Operation("Subtract")
    ConstantA(1.00)
    EventInputB("player1.weapon1.heat")
    Clamp(0.00, 1.00)
    EventOutput("player1.example.heatRemaining")
}

TransformNumberMath("player1example_heatremainingpercent")
{
    Operation("Multiply")
    EventInputA("player1.example.heatRemaining")
    ConstantB(100.00)
    EventOutput("player1.example.heatRemainingPercent")
}
```

In a test copy of `BarSegmented("player1weapon1heatbar")`, change only the value
binding to `EventValue("player1.example.heatRemaining")`. Keep its existing
`EventEnable("player1.weapon1.heat")`, disable, colour, segments, scale and fade
settings. The ring drains as heat rises and refills as the weapon cools; its
existing colour still represents heat, retaining the warning. Fire the weapon
to exercise its existing visibility rules.

A test Text element can use `EventNumber("player1.example.heatRemainingPercent")`
and `FloatFormat("%.0f")` for a rounded 0–100 number. Keep its font, position and
visibility settings, and do not also enable native percentage conversion.
Build the DLL and munge the changed HUD sources normally; the DLL does not edit
your HUD assets. Load both transforms before their consumers.

#### Formula cookbook

| Desired signal | Transform chain |
|---|---|
| Percentage | Multiply `fraction` by 100; consume with `EventNumber`/`FloatFormat("%.0f")` |
| Fixed offset | Add `value` and a constant |
| Affine remap | Multiply by gain, then Add offset |
| Absolute difference | Subtract A-B and B-A separately, then Max of their outputs |
| Average | Add A+B, then Divide by 2 |
| Normalise a range | Subtract lower bound, then Divide by range width; optionally Clamp(0,1) |
| Dynamic upper/lower bound | Min or Max with the other live numeric event |
| Smooth-looking nonlinear intensity | Square/cube through Multiply chains; this changes the response curve, not time smoothing |
| Preserve overhealth | Use the raw health fraction or a deliberate wider clamp, not an automatic 0..1 clamp |

Every row still follows load-order, type and divide-by-zero rules. These are
compositions of existing operations, not extra `Operation("...")` names.

### Useful input events

This is a relevant selection, not the complete event catalogue. Native events
may stop updating when their gameplay/UI context is inactive.

| Event | Registered type | Authoring notes |
|---|---|---|
| `player1.healthFraction` | Float | Soldier health ratio; can exceed 1 with bonus health |
| `player1.healthInVehicleFraction` | Float | Soldier-health channel used while in a vehicle |
| `player1.hero.healthFraction` | Float | Hero-health channel, separate from ordinary soldier health |
| `player1.vehicle.healthFraction` | Float | Vehicle health, not occupant health |
| `player1.weapon1.target.healthFraction` | Float | Current/latching target context; retain target visibility events |
| `player1.energyFraction` | Float | Energy ratio; existing mapping also authors a -1 endpoint |
| `player1.jetFuelFraction` | Float | Jetpack fuel level |
| `player1.jetFuelThreshold` | Float | Authored minimum-border/warning threshold; do not assume a universal constant |
| `player1.weapon1.totalAmmoBullets` | Uint | Total including loaded clip in the reference build; infinite/scoped caveats above |
| `player1.weapon1.totalClipBullets` | Uint | Current clip count; respect weapon/infinite-ammo context |
| `player1.weapon1.totalClipFraction` | Float | Clip fraction, already useful directly with a bar |
| `player1.team1.reinforcements` | Int | Team 1 count; corresponding team2 event also exists |
| `player1.team1.reinforcementsFraction` | Float | Existing team's depletion bar value, not relative share against the other team |
| `player1.lockOnDistance` | Float | Existing distance text uses `FloatFormat("%.0f")` |
| `objectivetimer` | Float | Existing objective timer; keep its mode/disable handling |
| `time` | Float | Existing clock input for continuously driven mappings |
| `player1.weapon1.change` | Uint | Hashed selection name; NameMesh input, not an ammunition count |
| `player1.weapon1.name` | String | Display text; not a Math or NameMesh input |
| `player1.vehicle.hackingTimeFraction` | String | Despite its name, not a numeric bar/Math input in the reference catalogue |

Weapon 2/team 2 use `weapon2`/`team2` where the corresponding family exists.
Indices are 1-based. Do not combine inactive soldier/hero/vehicle caches and
assume that automatically selects whichever unit is currently controlled; that
requires explicit context/selection logic beyond this first Math feature.

## Testing and verification

In-game checks for each feature, in the order above, with the automated tests
behind each. `python tools/run_tests.py` from the repository root compiles and runs
every standalone test named here, the same way CI does.

### Icon checks

Spawn as a class with `HealthTexture` and its variants packed, then stand, crouch and
go prone; remove one variant and check the icon falls back along its chain. Roll up as
a droideka: the ball icon should appear only once fully balled. Enter and leave a
vehicle and a turret, switch weapons in each, then die and respawn as another class.
Repeat on a multiplayer client. `BF2GameExt.log` should show
`[HudClassIcons] publishing` once per mission when a `.hud` binds an event.
`tests/hud_class_icons_tests.cpp` covers the selection rules, and
`tests/hud_class_icons_abi_tests.py "path\to\GameData"` checks every engine read
against all three executables (read-only, needs `pefile`); neither replaces this
in-game pass.

### Target bar checks

After rebuilding the DLL, select a target without firing, look away, reacquire
it during the fade, then switch directly to another target. With the reference
settings, expect immediate appearance, a 0.5-second hold after selection loss,
then a 0.25-second HUD fade. Also test death/respawn, changing weapons, entering
and leaving vehicles, and mission reloads. Multiplayer still needs a live test.

Bind `target.distance` to a text and walk toward and away from a target: it should
count in metres and print `--` with `InfiniteDashes(1)` when nothing is targeted.

For the edges, turn away from a selected target: its bar should slide off the
screen with it rather than park at the edge. Then walk up to a large vehicle and
aim at it: its bar should stay at the top of the screen while selected. For the
rider pair, aim between a speeder bike and its exposed rider: the bar should stay
on one of them, and move to the rider only when you hold your aim on the rider.

`tests/target_bar_selection_tests.cpp` is a standalone C++17 assertion test
for replacement, expiry, generation reuse, channel isolation, prevention of
self-refresh and the rider pair. `tests/target_bar_geometry_tests.cpp` covers the
safe-area pin and the pixel snap, and `tests/target_bar_fade_tests.cpp` the
world-space anchor kept for the death fade. `tests/target_bar_selection_abi_tests.py
"path\to\GameData"` checks native input instructions read-only on all three
supported executables; it requires Python, `pefile` and `capstone`. These checks
do not replace an in-game test.

### Horizon levelling checks

The degree payload and native handler were checked against all three supported
executables. Standalone tests in `tests/hud_horizon_tests.cpp` cover bank, pitch,
yaw, inversion, projection aspect, zoom, invalid data and vertical-view stability,
including 20,000 random projected world-up comparisons. Check the behaviour in
your own in-game layout too, especially its parent scaling and pivot placement.

### Command post strip checks

Load a conquest map with the strip bound and check that every capturable post has a
slot, in `HUDIndex` order, and that invisible posts and command vehicles do not.
Capture a neutral post: its bar should fill in your colour, then the icon and colour
should flip to your team. Neutralise an enemy post: its bar should drain in its
owner's colour, and the icon should turn neutral when it empties. Watch an AI capture
elsewhere on the map in single player; the bar should move there too.

For the markers, bind the recipe from HUD.md and turn on the spot: each icon should
sit over its post, slide to the screen's edge as the post leaves the view, with the
arrow pointing the way to turn, and go to the side edges behind you, the top and
bottom in a flyer. The distance should count down as you walk to a post.

`tests/hud_command_posts_tests.cpp` covers the slot order, the capture value and
colour, and change detection, and `tests/hud_world_markers_tests.cpp` the placement,
edge pinning, arrow angle and distance, with 20,000 random points.
`tests/hud_command_posts_abi_tests.py "path\to\GameData"` checks every offset,
address and guard against all three executables, read-only, including the stock
anchor reads and the 1.3 m lift.

### Bar fill checks

Bind a bar with `FillFrom("Right")` and one with `FillFrom("Bottom")` to something you
can change, such as health. The first should shrink toward its right end and the second
toward its bottom edge, both with the picture upright and each point showing the same
part of it as the full bar. `tests/hud_bar_fill_from_tests.cpp` covers the geometry and
texture alignment of all three modes, and `tests/hud_bar_fill_from_abi_tests.py
"path\to\GameData"` checks the hooks' prologues and offsets on all three executables,
read-only.

### Sub-pixel checks

`HudSubPixel` under `[Features]` is on by default: look for `[HudSubPixel] installed` in
`BF2GameExt.log`. Turn slowly on the spot with command post markers or a floating
target bar in view: they should glide rather than step a pixel at a time, the icon and
its distance text moving together. Menus and still HUD elements should look as before,
or at most slightly softer. `tests/hud_sub_pixel_tests.cpp` runs the stand-in the patch
puts in place of the draw's `floor`, at both x87 precisions, and checks it leaves the
x87 stack balanced; `tests/hud_world_markers_tests.cpp` and
`tests/target_bar_geometry_tests.cpp` cover the unrounded marker and target bar
positions. `tests/hud_sub_pixel_abi_tests.py "path\to\GameData"` checks the draw, both
`floor` sites, the 0.5 each adds and the draw's callers on all three executables,
read-only.

### TrueWidescreen checks

Look for `[TrueWidescreen] installed` in `BF2GameExt.log`. At a 16:9 resolution, load a
map with a `TrueWidescreen(1)` file: the game's log should show one
`[TrueWidescreen] a .hud file laid out ...` line for it, with its counts of pieces per
edge. A square `BitmapRect` should look square, pieces should sit against the edges
they were written near, and the stock files, if any are loaded beside it, should look
as before. Aim at things with the reticule, lock on, and bring up the floating target
bar and command post markers: each should sit on its point, at the screen's edges too.
Load a map at a 4:3 resolution: the file should draw the stock way, as written. On
modtools, move a
piece in the HUD editor and save: the file should keep `TrueWidescreen(1)` and come
back in the same place. `tests/hud_true_widescreen_tests.cpp` covers the layout, edges,
slides and world-following positions on five screen shapes, the parent matrix through
the interface camera, and the stock mapping it replaces.
`tests/hud_true_widescreen_abi_tests.py "path\to\GameData"` checks every guard, the
addresses the loader moves inside them, the vtable slots, the contracts of the
detoured functions and the register each stand-in reads, on all three executables,
read-only.

### Math transform checks

#### In-game stability checklist

1. Try the low-health danger overlay while taking damage and recovering health;
   check the response curve and unchanged layout/visibility. Alternatively, test
   heat inversion while firing, overheating and cooling.
2. Test a chained result, such as the danger-intensity square or optional heat
   percentage; it should refresh without requiring an extra source update.
3. Test reinforcement subtraction while each team loses tickets; either input
   should refresh the result, including negative leads.
4. Switch weapons, die/respawn, enter/exit a vehicle, restart a mission, then load
   a different map and return. Look for stale values, duplicates or crashes.
5. Optional failure tests: temporarily set a constant divisor to zero or supply
   an invalid operation. The bar/text should keep its default/last good value;
   a bad configuration should log an error, not crash the game.

#### Automated coverage and its limits

Math coverage includes all six operations, 20,000 random arithmetic cases,
nonfinite values/overflow, malformed config arguments, both native calling
conventions with mocked engine callbacks, initial publication, two-input updates,
same-source operands, chaining, filtering, invalid configurations, feedback, and
200 simulated mission reloads. Lerp and Compare coverage adds the easing in both directions, input ranges from
numbers and events, every comparison with and without hysteresis, the Bool events,
turning round mid-swing, one-update pulses, clamped and invalid inputs, event and
constant ends, one event on two inputs, the third handler's unlink, invalid
configurations and 200 more reloads. Installer fingerprints and the handler unlink
are checked read-only against all three local executables. These are **not** a
replacement for a DLL build and in-game testing.

The optional x86 MSVC AddressSanitizer executable compiled, but this host's ASan
runtime failed during interceptor initialisation before running tests; that run
is not counted as a sanitizer pass. Ordinary standalone and syntax checks passed.

#### Running the standalone tests

For a stricter `/W4 /WX` build of just the math test, open an x86 Visual Studio
developer command prompt in the ignored `build\hud-number-math` directory (create
it first):

```bat
cl /nologo /std:c++17 /EHsc /W4 /WX /I ..\..\PatcherDLL\src\core /I ..\..\PatcherDLL\src /Fe:hud_number_math_tests.exe /Fo:hud_number_math_tests.obj ..\..\tests\hud_number_math_tests.cpp
hud_number_math_tests.exe
```

The independent PE fingerprint audit requires Python and `pefile`. Run it from
the repository root:

```text
python tests/hud_number_math_abi_tests.py "path\to\GameData"
```

### State and lerp checks

Bind the sprint recipe from [HUD.md](../user/HUD.md#unit-and-weapon-states), then
sprint in first and third person: the reticule should fade out over `RiseTime`, stay
hidden for the whole sprint, fade back over `FallTime`, and turn round if the sprint
is cut short. If it snaps back as soon as a fade finishes, `OutputIsAlpha(1)` is
missing. Board a
vehicle while sprinting; the reticule should come back and stay. Bind a text element
to each state event with `FloatFormat("%.0f")` and jump, fall, roll, jet, hover, get
knocked down, fire, charge, reload, overheat, swing and block a melee weapon, then
repeat on a multiplayer client. A lerp on `land` or `shot` with only a `FallTime`
should flash on every landing or shot. `tests/hud_class_icons_tests.cpp` covers the
state mapping, landings and shots, and `tests/hud_number_math_tests.cpp` the lerp.

## Troubleshooting and limits

| Symptom | Check |
|---|---|
| Class icon never shows | `HealthTexture` set on the class; its texture packed in a level the mission loads; `EventEnable` on the texture event; `[HudClassIcons] publishing` in `BF2GameExt.log` |
| Class icon ignores a stance | The variant's name is the `HealthTexture` name plus `_crouch`, `_prone` or `_ball`, and it is loaded; a missing one falls back silently |
| Rotation changes artwork proportions | Put rotation on a unit-scale pivot and authored sizing on a child |
| Math class does nothing or is unknown | DLL contains this implementation, extension is loaded, installer guards passed, HUD was munged and loaded |
| `[HudNumberMath] ... disabled` | Read the following log reason: missing/duplicate field, unknown operation, invalid clamp/type/name, output collision or unresolved input |
| Editor export loses Math blocks | Source-authoring only for Math; preserve source rather than overwriting it with the export |
| A transform is silent | Source event exists **and sends**; output exists before consumer binds; native table is nonempty |
| Result stays at a default/old value | Waiting for a source, source context inactive, invalid operand, zero divisor, or result unchanged |
| Factor/colour-weight changes do not refresh | Native factor, Blend and Alpha inputs only cache; primary EventInput must fire |
| Scale/position/rotation ignores Math output | Convert Float to Vector3 with NumberVector3; verify the receiving group and units |
| Numeric text prints decimals | Math publishes Float; use a valid float format such as `FloatFormat("%.0f")`, not an integer `%d` format |
| A percentage is multiplied twice | Do not combine Math ×100 with the native text percentage conversion |
| Zero-valued output still enables something | EventEnable is not a numeric gate; use appropriate visibility events or alpha |
| Unexpected flicker/transient value | Different sources/graph branches publish sequentially; inspect update order and stale context |
| Colours unexpectedly mix or overwrite | Multiple writers share an output or element colour binding; use unique events |
| An alpha fades, then snaps back to full | `EventAlpha` lasts one frame: give the transform feeding it `OutputIsAlpha(1)` |
| `EventAlpha` changes nothing | It only exists on groups; give that group `PropagateAlpha(1)` so its alpha reaches the elements inside it |
| `... has more arguments than it takes` in the log | A comment on that property's line has no space after its `//` |
| A comparison flickers on and off | The value is sitting on the line: give it a `Hysteresis` |
| A lerp fades the wrong way | `RiseTime` and `FallTime` follow the input, not the output: with A = 1 and B = 0 the output falls over `RiseTime` |
| A lerp never sends | Its input has not sent a valid value yet, or an event end has not arrived; nothing is sent before both |
| A state event stays at 0 | The unit states are for soldiers on foot only; a droideka, vehicle, turret or remote reads 0 |
| A moving marker or bar steps a pixel at a time | The game draws every element on a whole pixel. `[Features] HudSubPixel` in `BF2GameExt.ini` draws them at their exact positions; it is on by default, but each player's own setting, so check it is not 0 |

## Evidence and related references

- [Math and lerp implementation](../../PatcherDLL/src/render/hud_number_math.cpp): the exact property whitelists, name limits, activation, callbacks, the lerp's third handler and failure handling.
- [Math and lerp numeric core](../../PatcherDLL/src/render/hud_number_math_core.hpp): six operations, the lerp's easing, type conversion, clamp, numeric range and change suppression.
- [Math and lerp tests](../../tests/hud_number_math_tests.cpp): parser, arithmetic, easing and mocked native event/lifetime tests.
- [State events](../../PatcherDLL/src/render/hud_class_icons.cpp) and [their rules](../../PatcherDLL/src/render/hud_class_icons_core.hpp): which soldier and weapon states set each flag, landings and shots.
- [HUD system RE](../RE/HUDSystem.md): factory registration, type catalogue, load order, NameMesh inheritance and build-specific adapter addresses.
- [HUD event bindings RE](../RE/HUDEventBindings.md): health ranges/context and consumer behaviour.
- [HUD blend mode RE](../RE/HUDBlendMode.md): renderer blend state and Model3D/MeshInfo distinctions.

Additional reference-build functions checked for this document: numeric input
handling `0061D3F0`/`0061E3B0`, colour/vector interpolation `0061D1D0`/`0061E260`,
factor/wrap reader `0061DF70`, factor callback `0061DF30`, wrap calculation
`0061DE70`, ColorBlend reader/input `0061DB00`/`0061D940`, its constructor
`0061D770`, and NameMesh reader/input `0061CD50`/`0061C8E0`. These are Phantom
addresses for evidence, not addresses mod authors should call.
