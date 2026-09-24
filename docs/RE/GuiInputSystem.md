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
