<!-- GENERATED FILE. Do not edit by hand.
     Produced by generate_ini.py from ini_registry.hpp,
     controller_support.cpp and version.h. Re-run:  python generate_ini.py -->

# Controller Bindings

Gamepad binding reference for BF2GameExt v1.0.0. Enable the pad itself with `[Controller] Enabled=1`. Aim assist is separate and is **off by default** - turn it on with `[AimAssist] Enabled=1`. See [CONFIGURATION.md](CONFIGURATION.md#controller) for both and for the aim assist tuning values.

## How a binding works

Each binding is one line in a `[Controller.<Mode>]` section. The **key** is a raw input (a physical button or axis) and the **value** is a comma-separated list of **actions** to fire:

```ini
[Controller.Unit]
A=Jump              ; one button, one action
LB=Crouch,Zoom      ; one button, two actions at once
LY-=MoveAxis        ; a stick axis driving a full movement axis
DPadUp=MoveNeg      ; a button driving one half of an axis
Back=               ; empty value unbinds it
```

In the shipped INI every default line is commented out with a leading `;`. Uncomment a line to override that binding; anything you leave commented keeps its default. Bindings are per mode and do not inherit, so rebinding jump for `Controller.Unit` does not change it for `Controller.Hero`.

Full axes (`MoveAxis`, `TurnAxis`, `StrafeAxis`, `PitchAxis`) expect a stick axis on the left side. The half-axis actions (`MovePos`/`MoveNeg` and friends) exist so you can drive movement from a digital button such as a d-pad direction.

## Modes

| Section | Applies to |
|---------|------------|
| `[Controller.Unit]` | On foot, the default for infantry |
| `[Controller.Vehicle]` | Ground vehicles and walkers |
| `[Controller.Flyer]` | Flyers, adds `Roll` |
| `[Controller.Hero]` | Heroes and villains |
| `[Controller.Turret]` | Mounted and emplaced turrets |

## Raw input names

Valid on the left of the `=`.

| Name | Control |
|------|---------|
| `A` | Face button, bottom |
| `B` | Face button, right |
| `X` | Face button, left |
| `Y` | Face button, top |
| `LB` | Left shoulder bumper |
| `RB` | Right shoulder bumper |
| `Back` | Back / Select |
| `Start` | Start / Menu |
| `L3` | Left stick click |
| `R3` | Right stick click |
| `DPadUp` | D-pad up |
| `DPadRight` | D-pad right |
| `DPadDown` | D-pad down |
| `DPadLeft` | D-pad left |
| `LX+` | Left stick right |
| `LX-` | Left stick left |
| `LY+` | Left stick down |
| `LY-` | Left stick up |
| `ZPos` | DirectInput Z axis, positive. Usually the left trigger, see the note below |
| `ZNeg` | DirectInput Z axis, negative. Usually the right trigger, see the note below |
| `RX+` | Right stick right |
| `RX-` | Right stick left |
| `RY+` | Right stick down |
| `RY-` | Right stick up |
| `RZPos` | DirectInput RZ axis, positive. Varies by controller |
| `RZNeg` | DirectInput RZ axis, negative. Varies by controller |
| `RT` | Right trigger. Alias for `ZNeg` |
| `LT` | Left trigger. Alias for `ZPos` |

> **Triggers.** Both triggers sit on the single DirectInput Z axis on most pads, with the left trigger reading positive and the right negative. `LT` and `RT` are aliases for `ZPos` and `ZNeg`, so binding both a trigger alias and its Z axis name to different actions will not do what you want. Which physical control lands on `RZPos`/`RZNeg` varies between controllers and drivers, so those two are worth testing rather than assuming.

## Action names

Valid on the right of the `=`, comma-separated.

| Name | Effect |
|------|--------|
| `PrimaryFire` | Fire the primary weapon |
| `SecondaryFire` | Fire the secondary weapon |
| `Sprint` | Sprint |
| `Jump` | Jump |
| `Crouch` | Crouch. Double-tap triggers prone when the Prone feature is enabled |
| `Zoom` | Zoom / scope |
| `View` | Toggle first and third person |
| `Reload` | Reload |
| `Use` | Use / enter vehicle |
| `SquadCommand` | Issue a squad command |
| `AcceptHero` | Accept a hero spawn offer |
| `DeclineHero` | Decline a hero spawn offer |
| `LockTarget` | Lock onto a target |
| `PrimaryNext` | Next primary weapon |
| `PrimaryPrev` | Previous primary weapon |
| `SecondaryNext` | Next secondary weapon |
| `SecondaryPrev` | Previous secondary weapon |
| `PlayerList` | Show the player list |
| `Map` | Show the map |
| `Roll` | Roll (flyers) |
| `StrafeAxis` | Full strafe axis. Bind to a stick axis, not a button |
| `MoveAxis` | Full forward and back axis. Bind to a stick axis, not a button |
| `TurnAxis` | Full turn / yaw axis. Bind to a stick axis, not a button |
| `PitchAxis` | Full pitch axis. Bind to a stick axis, not a button |
| `StrafePos` | Strafe right. Half-axis, for binding a button to an axis |
| `StrafeNeg` | Strafe left. Half-axis, for binding a button to an axis |
| `MovePos` | Move backward. Half-axis, for binding a button to an axis |
| `MoveNeg` | Move forward. Half-axis, for binding a button to an axis |
| `TurnPos` | Turn right. Half-axis, for binding a button to an axis |
| `TurnNeg` | Turn left. Half-axis, for binding a button to an axis |
| `PitchPos` | Pitch down. Half-axis, for binding a button to an axis |
| `PitchNeg` | Pitch up. Half-axis, for binding a button to an axis |
| `None` | Explicitly unbound |

## Default bindings

Blank means unbound by default.

### Controller.Unit

| Input | Actions |
|-------|---------|
| `A` | `Jump` |
| `B` | `Crouch,Roll` |
| `X` | `Reload` |
| `Y` | `Use` |
| `LB` | `SecondaryNext` |
| `RB` | `PrimaryNext` |
| `Back` | `PlayerList` |
| `Start` | `View` |
| `L3` | `Sprint` |
| `R3` | `Zoom` |
| `DPadUp` | `SquadCommand` |
| `DPadRight` | `AcceptHero` |
| `DPadDown` | `DeclineHero` |
| `DPadLeft` | `LockTarget` |
| `RT` | `PrimaryFire` |
| `LT` | `SecondaryFire` |
| `LX+` | `StrafeAxis` |
| `LY-` | `MoveAxis` |
| `RX+` | `TurnAxis` |
| `RY-` | `PitchAxis` |

### Controller.Vehicle

| Input | Actions |
|-------|---------|
| `A` | `Jump` |
| `B` | `Crouch,Roll` |
| `X` | `LockTarget` |
| `Y` | `Use` |
| `LB` | `SecondaryNext` |
| `RB` | `PrimaryNext` |
| `Back` | `Map` |
| `Start` | `View` |
| `L3` | `Sprint` |
| `R3` | `Zoom` |
| `DPadUp` | `SquadCommand` |
| `DPadRight` | `AcceptHero` |
| `DPadDown` | `Reload` |
| `DPadLeft` | `DeclineHero` |
| `RT` | `PrimaryFire` |
| `LT` | `SecondaryFire` |
| `LX+` | `StrafeAxis` |
| `LY-` | `MoveAxis` |
| `RX+` | `TurnAxis` |
| `RY-` | `PitchAxis` |

### Controller.Flyer

| Input | Actions |
|-------|---------|
| `A` | `Jump` |
| `B` | `Crouch` |
| `X` | `LockTarget` |
| `Y` | `Use` |
| `LB` | `StrafeNeg` |
| `RB` | `StrafePos` |
| `Back` | `Map` |
| `Start` | `View` |
| `L3` | `Sprint` |
| `R3` | `Zoom` |
| `DPadUp` | `SquadCommand` |
| `DPadRight` | `PrimaryNext,AcceptHero` |
| `DPadDown` | `Reload` |
| `DPadLeft` | `PrimaryPrev,DeclineHero` |
| `RT` | `PrimaryFire` |
| `LT` | `SecondaryFire` |
| `LX+` | `StrafeAxis` |
| `LY-` | `MoveAxis` |
| `RX+` | `TurnAxis` |
| `RY-` | `PitchAxis` |

### Controller.Hero

| Input | Actions |
|-------|---------|
| `A` | `Jump` |
| `B` | `Crouch,Roll` |
| `X` | `LockTarget` |
| `Y` | `Use` |
| `LB` | `SecondaryNext` |
| `RB` | `PrimaryNext` |
| `Back` | `Map` |
| `Start` | `View` |
| `L3` | `Sprint` |
| `R3` | `Zoom` |
| `DPadUp` | `SquadCommand` |
| `DPadRight` | `AcceptHero` |
| `DPadDown` | `Reload` |
| `DPadLeft` | `DeclineHero` |
| `RT` | `PrimaryFire` |
| `LT` | `SecondaryFire` |
| `LX+` | `StrafeAxis` |
| `LY-` | `MoveAxis` |
| `RX+` | `TurnAxis` |
| `RY-` | `PitchAxis` |

### Controller.Turret

| Input | Actions |
|-------|---------|
| `A` | - |
| `B` | - |
| `X` | `LockTarget` |
| `Y` | `Use` |
| `LB` | - |
| `RB` | - |
| `Back` | `Map` |
| `Start` | `View` |
| `L3` | - |
| `R3` | `Zoom` |
| `DPadUp` | `SquadCommand` |
| `DPadRight` | `PrimaryNext,AcceptHero` |
| `DPadDown` | `Reload` |
| `DPadLeft` | `PrimaryPrev,DeclineHero` |
| `RT` | `PrimaryFire` |
| `LT` | `SecondaryFire` |
| `LX+` | `StrafeAxis` |
| `LY-` | `MoveAxis` |
| `RX+` | `TurnAxis` |
| `RY-` | `PitchAxis` |
