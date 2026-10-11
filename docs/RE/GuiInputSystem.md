# GUI input: how menus get their input

Menus do not read the keyboard or the pad directly. Everything funnels through one `GUIInputs`
object (`RawControllerInputs::m_pGuiInputs`, modtools `+0x25C8`): `float mGUIInputs[26]`,
`mProcessedGUIInputs[26]` at `+104`, `mLastGUIInputs[26]` at `+208`, `MouseCoords` at `+312`.

## The shell was built for a pad

All 120 `ifs_*` screens implement `Input_Accept`, `Input_Back`, `Input_GeneralUp/Down/Left/Right`,
`Input_Start`, `Input_L/RTrigger` and `Input_Misc`/`Misc2`, and the handlers already take a
joystick index (`metagame_ai.lua:46`, `ifs_meta_main:Input_Accept(iJoystick,1)`). Only 2 files use
`fnTestHotSpot` for mouse hit-testing, against 57 with explicit directional handlers and the rest
inheriting `gShellScreen_fnDefaultInputUp/Down`. The console controller API is intact too
(`ScriptCB_ReadAllControllers`, `ScriptCB_GetVKeyboardCharacter` for pad text entry).

Some PC overrides are mouse-first, though (`assets/Shell/scripts/PC/*.lua` in the modtools):

- `PC/ifs_login.lua` only loads a profile when `CurButton == "_accept"`, which only the mouse sets
  (its own comment: "the only way to load a profile on the pc"). Pad Up/Down moves `SelectedIdx`
  correctly and Accept then does nothing. Its `gButtonMode` is never assigned, so it is always nil.
- The top tab row is an `ifelem_tabmanager` (`gPCMainTabsLayout` in `pctabs_options.lua`). Tabs
  live in `this._Tabs[tag]`, not in `this.buttons`, so the button link graph never reaches them.

The stashed fix injects a Lua chunk into the shell state on return from `ShellLoop::Init`
(modtools `0x00738E60`, Steam `0x00635D70`, GOG `0x00636E10`) through `lua_dobuffer` (modtools
`0x007B78B0`, Steam `0x0069B700`, GOG `0x0069C790`). That return is the only moment the state is
new and every screen exists; re-entering the shell makes a new state, so the chunk re-applies
itself. It wraps `ifs_login.Input_Accept` and gives every screen an
`Input_LTrigger2`/`Input_RTrigger2` that cycles the top tabs.

`eGUIINPUT_TYPE`:

```
-1 NONE, 0 Accept, 1 Back, 2 Start, 3 Select, 4 Misc1, 5 Misc2, 6 Up, 7 Down, 8 Left, 9 Right,
10 LeftTrigger, 11 RightTrigger, 12/13 LeftTrigger2/RightTrigger2, 14/15 LeftTrigger3/RightTrigger3,
16 yAxis, 17 xAxis, 18-25 FreeEye*/FindPlayer, 26 MAX
```

## The feed tables

Both are static data with 8-byte entries `{eGUIINPUT mKeyA, mKeyB}`:

| Table | Maps | modtools | Steam | GOG |
|---|---|---|---|---|
| `s_defUIBindings[0x4C]` | raw input id -> GUI | `0x00ADC7C0` | `0x007EB000` | `0x007EC000` |
| `sKeyboardGuiBindings[0x90]` | BindID -> GUI | `0x00ADCA20` | | |
| `RedKeyTo_BindID[0x90]` | BindID -> DIK scancode | `0x00ADBEE8` | | |

`RawControllerInputs::ReadLatest` (modtools `0x007461D0`) ends with, for each raw input,
`GUIInputs::Set(mKeyA, value); Set(mKeyB, value)`, then `GUIInputs::Process`. On modtools that is
two loops: the mouse rows `0x40..0x4B` once, then the joystick rows `0x00..0x3F` once per connected
device. The Steam table is confirmed by the `ReadLatest` xrefs at `0x004152C3`, `0x00415318` and
`0x00415349`.

**PC ships the pad rows almost empty.** Rows `0x00..0x03` (the first four buttons) are all
`Accept` and everything else through `0x41` is `NONE`, so a pad can never back out of a screen.
Only the mouse rows are filled (`0x42` LMB=Accept, `0x43` RMB=Misc1, `0x44` MMB=LeftTrigger,
`0x4A/0x4B` wheel=Up/Down). The keyboard rows are fully populated, which is why the keyboard can
navigate.

- `GUIInputs::Set`: `xAxis` and `yAxis` are a plain assign, so bind only ONE raw input to each;
  everything else keeps the larger magnitude, so several raw inputs may feed one GUI input.
- `GUIInputs::Process`: ids 6, 7, 8, 9, 16 and 17 pass through every frame (repeatable), everything
  else only on a press edge. The Lua global `gbWantMousedownEvents` makes everything repeatable.

## The consumer

`GuiManager::CollectInputs` (called from `GuiManager::UpdateAll`) turns the GUI inputs into events
at a `0.125` threshold: Accept 0, Back 1, Misc1 2, Misc2 3, Select 4, Start 5, LeftTrigger 10,
RightTrigger 11, Up `0x1C/0x18`, Down `0x1E/0x1A`, Left `0x1F/0x1B`, Right `0x1D/0x19`, mouse move
`0x22`. Analog navigation already exists: `yAxis >= 0.5` is Up, `<= -0.5` Down, and the same for
`xAxis`, with the engine's own auto-repeat (0.65 s first, 0.15 s after; left/right repeat gated on
`g_InputRepeatEnable`, set by `ScriptCB_SetInputRepeat`).

`GuiManager::HandleEvents` (Steam `0x00528FB0`) drains the event queue and builds the Lua method
name at runtime, `_snprintf(buf, 0x7F, "%s%s", "Input_", name_table[id*2])`, then
`CallLuaFunctionOfScope`. The name table (Steam `0x007E66C4`) carries the full console set.

## The fix

Fill the pad rows of `s_defUIBindings` after checking the whole 608-byte table matches stock:
`0x00` A->Accept, `0x01` B->Back, `0x02` X->Misc1, `0x03` Y->Misc2, `0x04/0x05` LB/RB->
LeftTrigger2/RightTrigger2 (unused by keyboard and mouse), `0x06` View->Select, `0x07` Start->Start,
`0x20..0x23` hat Up/Right/Down/Left, `0x30` X axis->xAxis, `0x33` Y axis->yAxis (DirectInput Y is
negative-up, so the negative half-axis means up).

## The Lua pad layer

`controller/menu_navigation_shell.inc` runs once in the shell's Lua state (after `ShellLoop::Init`)
and once in game (after `ReadDataFile("ingame.lvl")`). PC screens act on whatever the mouse is
over (`this.CurButton`, `gMouseListBox` + `Layout.CursorIdx`, `gMouseOverImage`), so the pad layer
keeps its own focus per screen and shows it with the same calls mouse hover uses. Activating
something sets `CurButton` and calls the screen's own `Input_Accept`, so mods keep their click
rules. Rules that matter when touching it:

- The mouse wheel sends Up/Down too. Up/Down over a hovered list only go to the list when
  `GameExt_PadDir()` reports no d-pad or stick held.
- The right mouse button is Misc1. Pad X/Y shortcuts check `GameExt_PadButton(n)`.
- Screens built later (the Galactic Conquest purchase screens are built when a game starts)
  must be looked up when used, not when the script runs.
- A screen opened from inside an Accept handler can call its own `Input_Accept` while A is
  still down (1.3's battle mode screen does). Only the outermost Accept counts as the pad's.

## Spawn screen

`SpawnDisplay::UpdateInput` (modtools `0x0068C6E0`, Steam `0x0042B0C0`, GOG `0x0042B080`) only
knows "Accept while the mouse is over a hotspot". A pad press becomes exactly that for one call:
the mouse is moved into the target's hotspot (a point found with `RedHotSpot::IsPointInside`) and
Accept is held, so the engine's own class, team and post logic runs.

Command posts come from the map's own list, the one `SpawnDisplay::FindPost` walks. Map objects
hang off a list (modtools `0x00AD8274`, Steam `0x007EB9FC`, GOG `0x007EC9CC`): the map is
`node - 0x24C68` and its player index sits at `node - 0x24B00`. Post records are a static array
of 16 entries of `0x30` bytes (modtools `0x00B47148`, Steam `0x01F76410`, GOG `0x01F778C0`),
the first field a post pointer; `+0x2C` of that is the CommandPost, valid while its `+0x204`
matches the record's `+0x30`, team in the low 4 bits (signed) of `+0x234`. Post `i`'s hotspot is
`map + 0x1ECA8 + i * 0x100`, shown when bit 8 of the dword before it is set. The selected post
is `SpawnDisplay + 0x2094` (modtools) / `+0x2058` (Steam, GOG).

## Galactic Conquest

The GC screens are console code with PC branches. On PC every GC screen's `Input_Accept` only
acts when `CurButton == "_accept"`; anything else is treated as a mouse click (the purchase
screens step toward the cursor, the summary ignores it). The console stick planet picker,
`ifs_freeform_main:UpdateNextPlanet()`, is still there but `ifs_freeform_fleet` only calls it
when `gPlatformStr ~= "PC"`; it expects `ScriptCB_ReadLeftstick` to report up as positive.
Menu-list GC screens (battle mode, pause menu, cheats, scenario pick, new/load) use
`CurButton` for the highlighted entry instead.

## The reverted 2021 controller build

The Steam update of February 2021 (reverted a day later) added native pad support by compiling
the console pad code into the PC exe: one XInput pad only, XInput buttons converted to the PS2
pad bitmask (LT/RT become L1/R1, LB/RB L2/R2), and a "pad in use" flag set by whichever device
was used last, which is what its `ScriptCB_IsJoyUsed` reports. Its shell scripts show a
pad button icon (atlas `gamepad_xbox360`) next to buttons that carry `xicon = "A"` and toggle
them all when that flag changes, give options screens X/Y shortcuts, let the spawn screen cycle
command posts with LB/RB, and swap HUD prompts to pad wording. It ships as SteamStub-wrapped,
so it has to be unpacked before it can be read. The pad support here took the spawn screen,
the shortcuts and the Galactic Conquest handling from it.
