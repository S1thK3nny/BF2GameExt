# HUD authoring: events and transforms

This is the authoring reference for HUD event bindings, the new
`TransformNumberMath` and the four native transform classes. It covers parameters,
accepted event types, value ranges, defaults, update behaviour, practical examples
and testing, including floating target bars, reticule horizon levelling, and the
class, stance, vehicle and weapon icons.

`TransformNumberMath` requires a BF2GameExt DLL built from the revision that adds
it. Its Modtools, Steam and GOG adapters have automated/static checks; in-game
validation is a separate step. The other four classes are native engine features.
Native details below were checked against the open Phantom/reference executable
and the repository's reverse-engineering notes. Reference-build quirks are called
out explicitly rather than assumed to be fixes supplied by GameExt.

## Contents

- [Choose a transform](#choose-a-transform)
- [Placement, names and event wiring](#placement-names-and-event-wiring)
- [TransformNumberMath](#transformnumbermath)
- [Shared native numeric parameters](#shared-native-numeric-parameters)
- [TransformNumberColor](#transformnumbercolor)
- [TransformNumberVector3](#transformnumbervector3)
- [TransformNumberColorBlend](#transformnumbercolorblend)
- [TransformNameMesh](#transformnamemesh)
- [Practical math recipes](#practical-math-recipes)
- [Useful input events](#useful-input-events)
- [Reticule horizon levelling](#reticule-horizon-levelling)
- [Floating target bars](#floating-target-bars)
- [Class, stance and vehicle icons](#class-stance-and-vehicle-icons)
- [Bars that fill from the right](#bars-that-fill-from-the-right)
- [Testing and verification](#testing-and-verification)
- [Troubleshooting and limits](#troubleshooting-and-limits)
- [Evidence and related references](#evidence-and-related-references)

## Choose a transform

| Class | Input | Output | Use it for |
|---|---|---|---|
| `TransformNumberMath` | Two operands: numeric events or constants | Float | Arithmetic, ratios, offsets, clamping, combining live values |
| `TransformNumberColor` | One numeric event, with optional input factors | Color | Health/ammo tints and piecewise colour gradients |
| `TransformNumberVector3` | One numeric event, with optional input factors | Vector3 | Driving position, scale or rotation from a number |
| `TransformNumberColorBlend` | Numeric mapping input, plus a Color event and optional numeric blend-weight event | Color | Blending mapped RGB with another colour |
| `TransformNameMesh` | Uint containing a name hash | Model | Selecting a loaded HUD mesh, such as a weapon icon |

`Transform` and `TransformNumberType` are native base classes, **not additional
authorable factory keywords**. `TransformNumberType` is not a numeric conversion
or arithmetic transform.

Smooth/trail, timeline, compare, gate and select transforms are not implemented
by this work. Do not put those proposed class names into a production `.hud`.

## Placement, names and event wiring

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
| Float | `EventValue` on a bar; `EventNumber` on text; `EventAlpha` for opacity |
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

## TransformNumberMath

### Capability

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

### Complete parameter list

These seven keys are the complete accepted property set for the new class.

| Parameter | Arguments | Required/default | Accepted values and meaning |
|---|---|---|---|
| `Operation("name")` | Exactly one string | Required; no authored default | `Add`, `Subtract`, `Multiply`, `Divide`, `Min`, `Max`; case-insensitive, no symbols or other aliases |
| `ConstantA(value)` | Exactly one number | Choose this **or** `EventInputA` | Finite numeric literal; zero, negative and fractional values are allowed |
| `EventInputA("event")` | Exactly one string | Choose this **or** `ConstantA` | Existing Int, Uint or Float event; nonempty name, at most 240 bytes |
| `ConstantB(value)` | Exactly one number | Choose this **or** `EventInputB` | Same numeric rules as `ConstantA` |
| `EventInputB("event")` | Exactly one string | Choose this **or** `ConstantB` | Same event rules as `EventInputA` |
| `Clamp(minimum, maximum)` | Exactly two numbers | Optional; no clamp if absent | Both finite; minimum must be ≤ maximum; applies to the final result |
| `EventOutput("event")` | Exactly one string | Required | New Float event; nonempty name, at most 240 bytes; must not already exist |

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

### Operations

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

### Numeric types, precision and ranges

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

### Update and failure behaviour

- Waits until each event-fed operand has received a valid value. It does not
  query the game for a current value or invent an initial zero.
- Either operand's event recomputes the result from both cached operands.
- If both operands bind the **same** event, both update before calculation.
- First publication is delayed until a HUD update after loading, allowing
  consumers to bind. Constant-only transforms also publish then.
- After activation, publication is synchronous with input events. Unchanged
  Float results are not resent; idle frames do not produce repeated output.
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

## Shared native numeric parameters

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

### Wrapping is not a general modulo operator

`WrapInput(1)` works best for a nonnegative input and a zero-based mapping range
with two distinct endpoint keys. The reference implementation preserves the
exact upper endpoint, wraps values above it, and has unusual negative/nonzero-
origin behaviour. For a 0..1 range, -0.25 becomes +0.25, not the conventional
modulo result 0.75. Avoid using it for general signed arithmetic, a single-key
table, or arbitrary nonzero-origin ranges.

Unlike Math's whitelist, native parsers may only warn about some bad arguments
or accept extra arguments. Use the documented arities and ranges; permissive
parsing is not a supported capability. Do not rely on repeated singleton settings.

## TransformNumberColor

### Parameters and output

Accepts the five [shared numeric parameters](#shared-native-numeric-parameters),
plus:

| Parameter | Arguments | Values |
|---|---|---|
| `NumberColor(key, red, green, blue)` | Exactly four numbers per mapping row | Finite numeric key; RGB channels authored as integers 0..255 |

The output is **Color**, not a number or vector. Native rows set colour alpha to
255; `NumberColor` does **not** take a fifth alpha argument. The reader converts
channels to bytes, so do not rely on out-of-range values being safely clamped.
Interpolated channel values are rounded to byte colours.

### Example: health warning tint

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

## TransformNumberVector3

### Parameters and output

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

### Example: a shrinking marker

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

### Example: looping rotation driven by time

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

## TransformNumberColorBlend

### What it actually blends

First maps the primary numeric input through `NumberColor` rows. Then combines
that mapped RGB colour with the latest Color supplied through `EventBlend`.
It is **not** a generic two-Color-input lerp, and does not switch renderer blend
state. Its output remains a Color for `EventColor`.

### Complete parameter set

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

### Blend equations and timing

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

### Example: soften a health tint against white

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

Bind `EventColor("player1.example.softHealthTint")` on the artwork. Use a separate
element `EventAlpha` if opacity should also vary. An element's own
`BlendMode("Additive")` is a different rendering setting; see
[HUD blend modes](../RE/HUDBlendMode.md) for bitmap/model limitations.

## TransformNameMesh

### Parameters and event types

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

### Example: weapon HUD mesh selection

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

## Practical math recipes

These are source blocks to place before their consumers, not complete artwork
definitions. Keep fonts, positions, textures and native visibility rules on the
existing elements. All outputs use the `player1.example.` namespace.

### Low-health danger intensity: a nonlinear response

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
}
```

Bind `EventAlpha("player1.example.dangerIntensity")` on a dedicated warning
overlay or a test copy of `player1health_colourchange`. Opacity is 0 at full
health, 0.25 at half health and 0.5625 at quarter health. The clamp prevents bonus
health above 100% from generating a negative deficit that would square positive.
Keep the normal health-disable behaviour: this is not a persistent death overlay.

### Relative reinforcement balance

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

### Finite-ammo magazine / reserve counter

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

### Optional bar-and-text test: remaining heat capacity

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

### Formula cookbook

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

## Useful input events

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

## Reticule horizon levelling

Bind this on the reticule's rotation pivot:

```text
EventRotation("player1.reticule.horizonRotation")
```

The Vector3 event carries `(0, 0, angleInDegrees)`, aligning the reticule's up
direction with projected world-up. It reads the active camera, not the unit or
vehicle pose, so it follows the view in first and third person, including
90-degree banks and inverted flight. It does not change aim, position, size,
mesh, visibility or alpha. There is no INI setting; the binding is the opt-in.
Supported on Modtools, Steam and GOG, for the first local viewport (`player1`).

### Keep rotation and artwork scaling on separate groups

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

## Floating target bars

`player1.weapon1.target.position` and `player1.weapon2.target.position` publish
Vector3 viewport-relative target anchors. Bind with `EventPosition`, not
`EventEnable`. On death or removal, the bar retains its last live world-space
anchor for the existing HUD fade instead of dropping onto the corpse or sticking
to your view. Its screen position is recalculated as the camera moves. Living
targets still move normally during fade-out, and a newly selected target gets its
own position.

While the game has the target selected, its anchor is kept inside the screen's
safe area, so a big vehicle up close, whose top is above the screen, still shows
its bar. Once the selection is lost, during the hold and the fade, the bar follows
the target's true position and leaves the screen with it, clipped by the edge. A
target wholly behind the camera hides it.

A vehicle and its exposed rider trade the game's selection back and forth when the
crosshair is between them. The bar stays on whichever of the two was selected
first, and moves to the other only once that has held the selection for 0.3
seconds, so aiming at the rider on purpose still shows the rider. Any other target
replaces them at once. This changes only what the HUD shows; aim assist, lock-on
and firing see the game's own selection.

Support is inherently on, but inert unless the HUD binds the event. Sizing,
artwork offsets, labels and fading remain authored in the `.hud`.

Selection retention replaces the old hit latch: no hit is required. A naturally
selected target accepted by the native HUD refreshes the hold; after selection
is lost, that target remains for 0.5 seconds by default. A different target
replaces it immediately. The retained target cannot refresh its own timer, and
retention performs no new line-of-sight/range/frustum check. The game's existing
selection rules still decide acquisition; this does not port SWBFIII autoaim.
Weapon channels retain targets independently, only while their position event
has a listener. Weapon/controlled-object changes reset retention.

`[Features] TargetBarLatchSeconds=0.5` controls the hold, not the fade. Existing
INI overrides (including an old `2.5`) still apply; change them to `0.5` for
reference timing. Zero keeps the explicit no-timeout option.

### What can be authored entirely in the HUD

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

Last-world-anchor death fading remains. Immediate zero-health hiding is distinct
from the preserved native death fade and is not implemented.

## Class, stance and vehicle icons

These events carry the health icon of whatever you are playing, so a HUD can follow
the spawned class, its stance and any vehicle without Lua. They come from the HUD
update, which runs on every machine, so they also work on multiplayer clients, where
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
changes. Nothing is published unless a `.hud` binds at least one of them, and there
is no INI setting. Only the first local viewport (`player1`) has these events.

### Setting up the classes

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

### Weapon icons

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

### Limits

- Works on modtools, Steam and GOG. GameExt 1.1.0 and earlier do not have these
  events.
- The stance event is a plain number for text or `TransformNumberMath`; change the
  picture with the texture events.
- A texture a script loads after the icon was chosen is picked up at the next
  stance, class or vehicle change.
- Setting `HealthTexture` on a soldier changes nothing else in the stock game: only
  the class loader and the ODF parser read it.
- A game without GameExt ignores these bindings: the HUD loader logs "HUD Element
  unable to find event" and carries on, so one `.hud` serves both. A mission script
  can keep an older Lua icon swap for those games by checking
  `type(GameExt) == "table"` first; the check is on the type because `GameExt` can
  fall back to a plain `true`.

## Bars that fill from the right

`FillFrom("Right")` on a `BarBitmap` makes the bar keep its right end and move its
left edge with the value, instead of the stock fixed left end and moving right edge.
`FillFrom("Left")` is the stock behaviour and the default. The property only exists
with GameExt: a game without it logs `Error reading parameter` for that line and the
bar fills from the left.

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
- `BarBitmap` only, set in the `.hud` file; there is no INI setting.

## Testing and verification

### Selection retention checks

After rebuilding the DLL, select a target without firing, look away, reacquire
it during the fade, then switch directly to another target. With the reference
settings, expect immediate appearance, a 0.5-second hold after selection loss,
then a 0.25-second HUD fade. Also test death/respawn, changing weapons, entering
and leaving vehicles, and mission reloads. Multiplayer still needs a live test.

For the edges, turn away from a selected target: its bar should slide off the
screen with it rather than park at the edge. Then walk up to a large vehicle and
aim at it: its bar should stay at the top of the screen while selected. For the
rider pair, aim between a speeder bike and its exposed rider: the bar should stay
on one of them, and move to the rider only when you hold your aim on the rider.

`tests/target_bar_selection_tests.cpp` is a standalone C++17 assertion test
for replacement, expiry, generation reuse, channel isolation, prevention of
self-refresh and the rider pair. `tests/target_bar_selection_abi_tests.py
"path\to\GameData"` checks native input instructions read-only on all three
supported executables; it requires Python, `pefile` and `capstone`. These checks
do not replace an in-game test.

### Class icon checks

Spawn as a class with `HealthTexture` and its variants packed, then stand, crouch and
go prone; remove one variant and check the icon falls back along its chain. Roll up as
a droideka: the ball icon should appear only once fully balled. Enter and leave a
vehicle and a turret, switch weapons in each, then die and respawn as another class. Repeat on a
multiplayer client. `BF2GameExt.log` should show `[HudClassIcons] publishing` once
per mission when a `.hud` binds an event. `tests/hud_class_icons_tests.cpp` covers
the selection rules, and `tests/hud_class_icons_abi_tests.py "path\to\GameData"`
checks every engine read against all three executables (read-only, needs `pefile`);
neither replaces this in-game pass.

### In-game math stability checklist

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

### Automated coverage and its limits

Math coverage includes all six operations, 20,000 random arithmetic cases,
nonfinite values/overflow, malformed config arguments, both native calling
conventions with mocked engine callbacks, initial publication, two-input updates,
same-source operands, chaining, filtering, invalid configurations, feedback, and
200 simulated mission reloads. Installer fingerprints are checked read-only
against all three local executables. These are **not** a replacement for a DLL
build and in-game testing.

The optional x86 MSVC AddressSanitizer executable compiled, but this host's ASan
runtime failed during interceptor initialisation before running tests; that run
is not counted as a sanitizer pass. Ordinary standalone and syntax checks passed.

For horizon levelling, the degree payload and native handler were checked against
all three supported executables. Standalone tests cover bank, pitch, yaw,
inversion, projection aspect, zoom, invalid data and vertical-view stability,
including 20,000 random projected world-up comparisons. Check the behaviour in
your own in-game layout too, especially its parent scaling and pivot placement.

### Running the standalone math tests

`python tools/run_tests.py` from the repository root compiles and runs every
standalone test, this one included, the same way CI does. For a stricter
`/W4 /WX` build of just this test, open an x86 Visual Studio developer command
prompt in the ignored `build\hud-number-math` directory (create it first):

```bat
cl /nologo /std:c++17 /EHsc /W4 /WX /I ..\..\PatcherDLL\src\core /I ..\..\PatcherDLL\src /Fe:hud_number_math_tests.exe /Fo:hud_number_math_tests.obj ..\..\tests\hud_number_math_tests.cpp
hud_number_math_tests.exe
```

The independent PE fingerprint audit requires Python and `pefile`. Run it from
the repository root:

```text
python tests/hud_number_math_abi_tests.py "path\to\GameData"
```

## Troubleshooting and limits

| Symptom | Check |
|---|---|
| Math class does nothing or is unknown | DLL contains this implementation, extension is loaded, installer guards passed, HUD was munged and loaded |
| `[HudNumberMath] ... disabled` | Read the following log reason: missing/duplicate field, unknown operation, invalid clamp/type/name, output collision or unresolved input |
| A transform is silent | Source event exists **and sends**; output exists before consumer binds; native table is nonempty |
| Result stays at a default/old value | Waiting for a source, source context inactive, invalid operand, zero divisor, or result unchanged |
| Factor/colour-weight changes do not refresh | Native factor, Blend and Alpha inputs only cache; primary EventInput must fire |
| Scale/position/rotation ignores Math output | Convert Float to Vector3 with NumberVector3; verify the receiving group and units |
| Numeric text prints decimals | Math publishes Float; use a valid float format such as `FloatFormat("%.0f")`, not an integer `%d` format |
| A percentage is multiplied twice | Do not combine Math ×100 with the native text percentage conversion |
| Zero-valued output still enables something | EventEnable is not a numeric gate; use appropriate visibility events or alpha |
| Unexpected flicker/transient value | Different sources/graph branches publish sequentially; inspect update order and stale context |
| Colours unexpectedly mix or overwrite | Multiple writers share an output or element colour binding; use unique events |
| Rotation changes artwork proportions | Put rotation on a unit-scale pivot and authored sizing on a child |
| Class icon never shows | `HealthTexture` set on the class; its texture packed in a level the mission loads; `EventEnable` on the texture event; `[HudClassIcons] publishing` in `BF2GameExt.log` |
| Class icon ignores a stance | The variant's name is the `HealthTexture` name plus `_crouch`, `_prone` or `_ball`, and it is loaded; a missing one falls back silently |
| Editor export loses Math blocks | Source-authoring only for Math; preserve source rather than overwriting it with the export |

No transform here reads a bone, projects world geometry, scales with distance
automatically, invents missing gameplay events, or supplies smoothing/history.
The floating healthbar and horizon-rotation events are separate event producers.
Their bindings are documented in [Floating target bars](#floating-target-bars)
and [Reticule horizon levelling](#reticule-horizon-levelling) above.

## Evidence and related references

- [Math implementation](../../PatcherDLL/src/render/hud_number_math.cpp): the exact property whitelist, name limits, activation, callbacks and failure handling.
- [Math numeric core](../../PatcherDLL/src/render/hud_number_math_core.hpp): six operations, type conversion, clamp, numeric range and change suppression.
- [Math tests](../../tests/hud_number_math_tests.cpp): parser, arithmetic and mocked native event/lifetime tests.
- [HUD system RE](../RE/HUDSystem.md): factory registration, type catalogue, load order, NameMesh inheritance and build-specific adapter addresses.
- [HUD event bindings RE](../RE/HUDEventBindings.md): health ranges/context and consumer behaviour.
- [HUD blend mode RE](../RE/HUDBlendMode.md): renderer blend state and Model3D/MeshInfo distinctions.

Additional reference-build functions checked for this document: numeric input
handling `0061D3F0`/`0061E3B0`, colour/vector interpolation `0061D1D0`/`0061E260`,
factor/wrap reader `0061DF70`, factor callback `0061DF30`, wrap calculation
`0061DE70`, ColorBlend reader/input `0061DB00`/`0061D940`, its constructor
`0061D770`, and NameMesh reader/input `0061CD50`/`0061C8E0`. These are Phantom
addresses for evidence, not addresses mod authors should call.
