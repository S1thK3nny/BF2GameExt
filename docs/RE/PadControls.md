# Pad controls: what the console left in the PC exe

What the PC executable still does with a pad, what it hides, and what is broken, for bringing pad play
closer to the console versions. There is no Xbox executable to compare against; this is what the PC
build contains. Researched 2026-10-11 by a read-only pass in Ghidra; nothing is built, and each address
is to be re-read before it is used. P = Phantom, M = modtools, S = Steam, G = GOG; S and G addresses
move with the exe. GameExt's own pad support is described in [CONTROLLER.md](../user/CONTROLLER.md)
and the menus in [GuiInputSystem.md](GuiInputSystem.md).

## The pad path in PlayerController::Update

`PlayerController::Update` (P `0x0071CAA0`, M `0x0059B460`, S `0x0061A2B0`, G `0x0061B320`):

- **Control mode:** 2 for a flyer in flight (the in-flight test is inferred), 3 with a melee weapon in
  slot 0, 4 for a `MountedTurret` or `EntityBuildingArmed`, 0 for a soldier (the droideka counts as
  one, inferred), 1 otherwise.
- **Mouse or pad:** `NetGame::IsMouseMode` (P `0x00696F50`, M `0x00707290`) reads bits 30-31 of
  `ProcessedInputs`, which `StandardInputProcess` sets when a mouse axis changes and clears when a pad
  axis changes.
- **Clamps:** turn and pitch to ±1 on a pad, ±16 for a mouse-driven soldier; move and strafe to ±1,
  any value that is not 0 raised to at least 0.3, and both pairs kept inside the unit circle.
- **The turn ramp, pad only:** while turn² + pitch² is at least 0.9 (the stick at about 95% or more),
  `mTurnBuildup` and `mPitchBuildup` grow by the frame time up to 1; otherwise both drop to 0. They are
  at Controllable +0xF4 and +0xF8 on every build (S block at `0x0061A611`).

## Who uses the ramp

In pad mode only:

- `EntitySoldier::GetOmega` (P `0x0056F3A0`, M `0x00529880`) blends from `MaxPitchSpeed` toward
  `MaxTurnSpeed` by the ramp. Stock soldiers have `MaxTurnSpeed` 5.0 (`com_inf_default.odf`) and the
  class default `MaxPitchSpeed` 1.0 (constructor P `0x0053DF90`; no stock ODF sets it), so on a pad
  they start turning at 1 radian a second and reach 5 only after a full second at 95% tilt or more.
- `EntityHover::Update` (P `0x00549740`), `EntityWalker::Update` (P `0x0059CDE0`) and
  `MountedTurret::Update` (P `0x00673B00`) do the same with their own rates.
- `EntityFlyer::Update` (P `0x0052EB00`) applies the ODF keys `PitchBuildupMultiplier` and
  `TurnBuildupMultiplier` (20 stock flyers set them).
- The droideka ignores it.

## Sensitivity and dead zone

**Sensitivity** is linear: `AdjustValueForSensitivity` (P `0x005C52E0`, M `0x00743380`) scales by
(slider + 1) x 0.2, saturating near the edge of the stick. It is applied in `StandardInputProcess`
(M `0x007455F0`, S `0x004153C0`, G `0x00415380`). Invert applies only to the full pitch axis.

**The dead zone is set in the wrong unit.** The profile byte +0x572 holds 1 to 10 (5 with no profile),
through `ScriptCB_GetDeadZone` / `ScriptCB_SetDeadZone` (P `0x0047B880` / `0x0047BA80`). It indexes the
table at P `0x00A665BC` (0, .002, ..., .01, .05, .1, .15, .2, .3), goes to `RedJoystick::SetDeadZone`
(P `0x00850160`) and then `EnumObjectsCallback` (P `0x0084FD50`), which sets `DIPROP_DEADZONE` on each
axis to the value times 16384. DirectInput reads that property in ten-thousandths, so every step is
1.64 times too big, and a per-axis zone is cross-shaped: it snaps diagonals onto the axes. Setting 5
gives about 1.6%, setting 9 about 33%. S: `0x00415C90` / `0x006C5B00` / `0x006C53A0`, table `0x00788140`;
G: `0x00415C50` / `0x006C6B90` / `0x006C6430`, table `0x007890F0`.

## The console aim help, left unplugged

`Controllable::UpdateTargetLockedObjTracking` (P `0x004F0160`, M `0x005E1120`) has no callers; its
constants (P `0x00A8DA30`) match those of GameExt's aim assist port. Its outputs, `mTurnAdjusted` and
`mPitchAdjusted` (+0x120 / +0x124), are still read: `GetTurnAdjusted` (P `0x004EE600`, M `0x005DEDE0`,
8 call sites in the soldier, droideka and turret updates) uses them whenever they are below 1.5, and
`ClearControllable` resets them to 2.0. While turning the player toward a target, the original also
forced `mTurnBuildup` to 1, so it turned at full speed.

Lock-on runs through a per-player `LockOnManager` in each entity's update (a press locks, a long hold
clears), with ranges from the ODF's `TargetLockMaxDistance` and `TargetLockLose`. Nothing in lock-on
or reticule targeting checks mouse against pad.

## Options the PC hides

- Profile fields (M offsets): `bAutoAim` +0x32 (`mbTargetAutoTracking` in P), `bAimAssist` +0x5AC,
  `bStickyReticule` +0x5AD, `bFlyerRolling` +0x5AE.
- `ScriptCB_SetDifficulty(d, true)` (P `0x0065E950`) turns the first three on for Easy and Medium and
  off for Hard; the PC's `ifs_login` and `ifs_difficulty` call it.
- The PC's `ifs_opt_general.lua` builds auto aim, aim assist, sticky reticule and rumble rows, then
  hides them on PC.
- Nothing in gameplay reads aim assist or sticky reticule (a byte search).
- The host's Auto Aim flag, `netAutoAim` (P `0x00B3956B`, M `0x00BE14CD`, S `0x01E62F12`,
  G `0x01E643C3`), is set from the host's `iAutoAim`, sent in the launch packet, and read by no
  gameplay code.

## Leftovers

`SetFunctionForAnalog` / `GetFunctionForAnalog` (P `0x005C5DB0` / `0x005C5740`) are empty on PC; on the
consoles they held stick layouts such as `Move_Turn`, `FreeLook` and `Throttle_Roll`.
`PopupText::Update` (P `0x00725C40`) still calls the Lua `SetFlyerControllerConfig`, the console's flyer
presets, whose PC branch does nothing. Sprint already behaves like the console's: a tap starts it and
it carries on while the stick is held forward (inferred).

## What GameExt could do

Ranked by value:

1. **A stick front end.** In GameExt's existing `StandardInputProcess` hook, before the original runs:
   a radial inner dead zone, an outer dead zone and a rescale on each pad's stick pairs (pad rows
   only, raw inputs below 0x40, so keyboard and mouse are untouched); a look curve after binding, on
   turn and pitch, in pad mode only (menus read the same raw values, so the curve stays off them).
   Replace `EnumObjectsCallback` (a DirectInput callback, so its convention is fixed) to set the
   DirectInput dead zone to 0, or to the corrected value in a native mode, and read the game's own
   dead-zone slider as the radial size. INI under `[Controller]`, for example `DeadZoneMode`,
   `LookDeadZone`, `MoveDeadZone`, `OuterDeadZone`, `LookCurve`. It only changes what the pad reports,
   so it should be safe online (inferred).
2. **Aim assist the console's way.** In the existing `PlayerController::Update` hook, write
   `mTurnAdjusted` / `mPitchAdjusted` (reset to 2.0 each frame) instead of the turn control, set
   `mTurnBuildup` to 1 while turning toward a target, and let the three hidden options drive
   tracking, the slowdown toward a target and the slowdown near one, with the rows shown when a pad is
   connected. Online, require `netAutoAim` and a local player. Retail builds may have inlined
   `GetTurnAdjusted`, so check its +0x120 readers on S and G first. It would also fix two likely
   faults in the current port (inferred): it leaves `mTurnBuildup` at 0, so its turn toward a target
   runs at a fifth of the console's speed for a stock soldier, and it assumes a 4:3 field of view.
3. **A tunable turn ramp:** set `mTurnBuildup` for local pad players with a threshold (stock 0.95),
   a ramp time (stock 1.0 s) and a starting speed (stock 0).
4. **An XInput reader for Xbox-type pads,** since both triggers share one DirectInput axis and cancel
   out when pulled together. The work is in matching the right DirectInput device with several
   connected.

## Open

- Online, who owns a client's turning? The host recomputes it from the inputs it receives; that
  decides whether items 2 and 3 may run on clients.
- Do `GetTurnAdjusted`'s readers survive on S and G?
- Does the PC localisation still carry the auto aim, aim assist and sticky reticule labels?
- What does the flyer mode's test (a value below 0.0001) measure?
- In play: the dead zone at 5 and at 9, the strength of the current aim assist's turn, and the sprint
  behaviour above.
