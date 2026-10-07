<!-- GENERATED FILE. Do not edit by hand.
     Produced by generate_ini.py from ini_registry.hpp,
     controller_support.cpp and version.h. Re-run:  python generate_ini.py -->

# Controller Bindings

Gamepad binding reference for BF2GameExt v1.1.0. Enable the pad itself with `[Controller] Enabled=1`. Aim assist is separate and is **off by default** - turn it on with `[AimAssist] Enabled=1`. See [CONFIGURATION.md](CONFIGURATION.md#controller) for both and for the aim assist tuning values.

> **Stick feel is not set here.** Pad bindings decide *which* button does *what*, nothing more. If the stick drifts, feels twitchy or too slow, or moves you when you are not touching it, that is sensitivity and deadzone - set those in the game's own **Options -> Controls** screen. Rebinding will not fix it.

## Where pad bindings live

The game gives every action two keys, shared by keyboard and pad. BF2GameExt adds a third slot for the pad, so pad buttons never take a keyboard key away. Rebind them on the game's own **Options -> Controls** screen: select an action and press a pad button or move a stick. The list shows pad bindings after the keyboard keys, for example `SPACE, NUMPAD 0, PAD A`. Holding Escape clears the action, pad included, and **Restore Defaults** resets the mode on screen, pad included.

Each profile keeps its pad bindings in `SaveGames\<profile>.padbinds`, next to the game's `<profile>.profile`. A profile without one starts from the default layout below, and the file is written the first time the profile is used with BF2GameExt.

> **After updating from an older BF2GameExt**, your profile may still carry pad buttons in its normal key slots. Press **Restore Defaults** once per mode on the controls screen to get the stock keys back.

## Editing the file

The file can also be edited by hand while the game is closed. Each line in a mode section is one input, then the actions it fires:

```ini
[Unit]
A=Jump              ; one button, one action
B=Crouch,Roll       ; one button, two actions at once
LY-=MoveAxis        ; a stick axis driving a full movement axis
DPadUp=MoveNeg      ; a button driving one half of an axis
Pad2.A=Jump         ; a second pad
```

An input or action missing from a section is unbound. Modes do not inherit, so `[Unit]` and `[Hero]` are set separately. The controls screen binds one pad input per action; by hand an action can have several.

Full axes (`MoveAxis`, `TurnAxis`, `StrafeAxis`, `PitchAxis`) expect a stick axis on the left side. The half-axis actions (`MovePos`/`MoveNeg` and friends) exist so you can drive movement from a digital button such as a d-pad direction.

## Modes

| Section | Applies to |
|---------|------------|
| `[Unit]` | On foot, the default for infantry |
| `[Vehicle]` | Ground vehicles and walkers |
| `[Flyer]` | Flyers, adds `Roll` |
| `[Hero]` | Heroes and villains |
| `[Turret]` | Mounted and emplaced turrets |

## Raw input names

Valid on the left of the `=`.

The four face buttons have two spellings for the same button. `A`/`B`/`X`/`Y` are the Xbox labels. `FaceDown`/`FaceRight`/`FaceLeft`/`FaceUp` name the button by **where it sits on the pad in your hands**, which is the unambiguous form: a DualShock prints different symbols in those positions, and a Nintendo pad swaps A with B and X with Y, so `B` means a different physical button depending on the pad. Either spelling works and both always drive the same button.

The file is written with `A`/`B`/`X`/`Y`, `LT` and `RT`; the other spellings are read the same way.

| Name | Control |
|------|---------|
| `A` | Face button, bottom. Same button as `FaceDown` |
| `B` | Face button, right. Same button as `FaceRight` |
| `X` | Face button, left. Same button as `FaceLeft` |
| `Y` | Face button, top. Same button as `FaceUp` |
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
| `FaceDown` | Face button, bottom. Same button as `A` |
| `FaceRight` | Face button, right. Same button as `B` |
| `FaceLeft` | Face button, left. Same button as `X` |
| `FaceUp` | Face button, top. Same button as `Y` |

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
| `Chat` | Open chat |
| `TeamChat` | Open team chat |
| `CommSpotted` | Communication: spotted |
| `CommMedic` | Communication: medic |
| `CommRepair` | Communication: repair |
| `CommAmmo` | Communication: ammo |
| `CommPickup` | Communication: pickup |
| `CommBackup` | Communication: backup |
| `CommAttack` | Communication: attack |
| `CommDefend` | Communication: defend |
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

## Default layout

What a profile without a `.padbinds` file starts with, and what Restore Defaults puts back. Blank means unbound.

### Unit

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

### Vehicle

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

### Flyer

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

### Hero

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

### Turret

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
