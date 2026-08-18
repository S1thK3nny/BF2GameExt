# Lua API

All functions below are registered globally and callable from any mission, ScriptInit, or shell script. To check for what builds they are available on, see the [compatibility table](../../README.md#compatibility).

## Detection

Set alongside the function registrations, so scripts can degrade gracefully when the extension isn't installed (`GameExt` is simply `nil` in vanilla).

| Field | Description |
|-------|-------------|
| `GameExt` | A table when BF2GameExt is loaded; `nil` otherwise. |
| `GameExt.version` | Version string, e.g. `"1.0.0"`. |
| `GameExt.build` | Which executable is being patched: `"modtools"`, `"steam"`, `"gog"` or `"unknown"`. |
| `GameExt.disable` | Write-only opt-out. Set it truthy to disable BF2GameExt's intrusive gameplay features for a mission (see below). |

```lua
if GameExt then
    print("BF2GameExt " .. GameExt.version .. " on " .. GameExt.build)
    SetFogEnable(1)
end
```

**Opting out (`GameExt.disable`)** - A mod that wants the extension's fixes and Lua API but *not* its gameplay-altering features can set `GameExt.disable = true`. Set it in your mission's `ScriptPreInit` or `ScriptInit`, but before the `ReadDataFile("ingame.lvl")` call, so it takes effect for that mission:

```lua
function ScriptPreInit()
    if GameExt then GameExt.disable = true end   -- e.g. suppress prone
end
```

Currently this gates the **Prone** feature (the only feature that modifies mod content, it grafts a pose into the soldier animation banks). It leaves bug-fix patches and the Lua API untouched. Future intrusive features will honor the same switch.

## Character & Weapon Queries

| Function | Description |
|----------|-------------|
| `GetCharacterWeapon(charIndex, channel)` | Returns the ODF name of the weapon currently held in the given channel (0 = primary, 1 = secondary, ...). Returns nil if the slot is empty. |
| `SetCharacterWeapon(charIndex, odfName [, channel])` | Replaces the active weapon in a channel (0 = primary, 1 = secondary) with another already-loaded weapon ODF. Builds a real Weapon through the engine's own factory and destroys the old one , ammo, animation stance, and aimer all come out correct. Singleplayer only; slots using `WeaponShareAmmo`/`WeaponShareEnergy` are refused, as are weapons the unit's animation bank has no animmap for (e.g. giving a jedi-bank unit a rifle). The old weapon is kept in that case. Works in first person. Melee-family weapons (sabers, saber throw) are untested and unsupported. Returns 1 on success, nil on failure. |
| `GetWeaponAmmo(charIndex [, channel])` | Returns four numbers: `curClip, numClips, maxClips, roundsPerClip` for the active weapon in the channel (default 0). Ammo is tracked in **clips**, with `curClip` being a fractional 0.0-1.0 of one loaded clip. |
| `SetWeaponAmmo(charIndex, curClip [, numClips [, channel]])` | Writes `curClip` (fractional 0.0-1.0) and optionally `numClips` (spare clips) on the active weapon. Pass nil for `numClips` to leave it untouched. |

## Spawn Menu

| Function | Description |
|----------|-------------|
| `RemoveUnitClass(team, className)` | Removes a unit class from a team's spawn menu at runtime. Compact-shifts the team's class arrays to preserve order. |

## Event Callbacks

Register Lua callbacks that fire when soldiers dismount vehicles. All registration functions return a handle that can be passed to `ReleaseCharacterExitVehicle` to unsubscribe.

| Function | Description |
|----------|-------------|
| `OnCharacterExitVehicle(fn)` | Fires on every character exiting any vehicle. |
| `OnCharacterExitVehicleName(name, fn)` | Filtered to vehicles with the given entity name. |
| `OnCharacterExitVehicleTeam(team, fn)` | Filtered to a specific team index. |
| `OnCharacterExitVehicleClass(className, fn)` | Filtered to a specific vehicle ODF class. |
| `ReleaseCharacterExitVehicle(handle)` | Unregister a previously-registered callback. |

## Loading Screen

| Function | Description |
|----------|-------------|
| `SetLoadDisplayLevel(path)` | Redirects the loading screen to a custom load lvl. Call from script root or ScriptPreInit. |

`SetLoadDisplayLevel` resolves its path the same way `ReadDataFile` does, minus
the sublevel suffix. The trailing `.lvl` is optional.

| Form | Resolves to |
|------|-------------|
| `SetLoadDisplayLevel("LOAD\\load.lvl")` | `data\_lvl_pc\LOAD\load.lvl` |
| `SetLoadDisplayLevel("dc:LOAD\\load.lvl")` | `<addon dir>\Data\_lvl_pc\LOAD\load.lvl`, e.g. `addon\VTR\Data\_lvl_pc\LOAD\load.lvl` |
| `SetLoadDisplayLevel("..\\..\\addon\\VTR\\data\\_LVL_PC\\LOAD\\load")` | raw path relative to `data\_lvl_pc\` (the original form, still supported) |

## In-Game Movies

Stock `ScriptCB_PlayInGameMovie(file, segment)` ignores `file` argument. Here it works,
and also takes the `dc:` addon prefix.

| Form | Resolves to |
|------|-------------|
| `ScriptCB_PlayInGameMovie("ingame.mvs", "hotmon01")` | stock ingame.mvs
| `ScriptCB_PlayInGameMovie("mymovie.mvs", "seg")` | `data\_lvl_pc\Movies\mymovie.mvs` |
| `ScriptCB_PlayInGameMovie("dc:mymovie.mvs", "seg")` | `<addon dir>\Data\_lvl_pc\Movies\mymovie.mvs` |

A bare name that is missing from the base game but present in the active addon
resolves to the addon copy on its own, so `dc:` is only needed when both exist.

Every call is logged to `BF2GameExt.log` with the file it actually opened. If the
movie will not play, that line names the exact path that was tried and why it
failed. The engine's own `Unable to open movie file` warning quotes the name your
script passed, not the path it looked in, so it cannot tell you that on its own.

## Rendering

| Function | Description |
|----------|-------------|
| `SetFogEnable(0/1)` | Toggles the D3D fog render state. |
| `SetFogRange(start, end)` | Sets near/far fog distances. |

## HTTP

Make HTTP requests directly from Lua. Useful for telemetry, live configuration, or external API integration in either singleplayer or multiplayer missions.

| Function | Description |
|----------|-------------|
| `HttpGet(url)` | Synchronous GET. Returns response body as a string, or nil on failure. |
| `HttpPut(url, body)` | Synchronous PUT. Returns response body. |
| `HttpPost(url, body)` | Synchronous POST. Returns response body. |
| `HttpGetAsync(url)` | Fire-and-forget GET on a background thread. |
| `HttpPutAsync(url, body)` | Fire-and-forget PUT. |
| `HttpPostAsync(url, body)` | Fire-and-forget POST. |
