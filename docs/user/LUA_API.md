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

| Function | Description | Since |
|----------|-------------|-------|
| `GetCharacterWeapon(charIndex, channel)` | Returns the ODF name of the weapon currently held in the given channel (0 = primary, 1 = secondary, ...). Returns nil if the slot is empty. | 1.0.0 |
| `SetCharacterWeapon(charIndex, odfName [, channel])` | Replaces the active weapon in a channel (0 = primary, 1 = secondary) with another already-loaded weapon ODF. Builds a real Weapon through the engine's own factory and destroys the old one , ammo, animation stance, and aimer all come out correct. Singleplayer only; slots using `WeaponShareAmmo`/`WeaponShareEnergy` are refused, as are weapons the unit's animation bank has no animmap for (e.g. giving a jedi-bank unit a rifle). The old weapon is kept in that case. Works in first person. Melee-family weapons (sabers, saber throw) are untested and unsupported. Returns 1 on success, nil on failure. | 1.0.0 |
| `GetWeaponAmmo(charIndex [, channel])` | Returns four numbers: `curClip, numClips, maxClips, roundsPerClip` for the active weapon in the channel (default 0). Ammo is tracked in **clips**, with `curClip` being a fractional 0.0-1.0 of one loaded clip. | 1.0.0 |
| `SetWeaponAmmo(charIndex, curClip [, numClips [, channel]])` | Writes `curClip` (fractional 0.0-1.0) and optionally `numClips` (spare clips) on the active weapon. Pass nil for `numClips` to leave it untouched. | 1.0.0 |

## Spawn Menu

| Function | Description | Since |
|----------|-------------|-------|
| `RemoveUnitClass(team, className)` | Removes a unit class from a team's spawn menu at runtime. Compact-shifts the team's class arrays to preserve order. | 1.0.0 |

## World Objects

| Function | Description | Since |
|----------|-------------|-------|
| `SetInstanceProperty(name, property, value)` | Writes an `[InstanceProperties]` key on the world object of that name, the way the world layer would have at load time. `name` is the name the object was given in ZeroEditor. Returns how many objects were written, or nil if the name matched nothing. | 1.1.0 |

Works on any named world object. Vehicle spawners are the reason it exists: stock
`SetProperty` cannot reach them at all, so before this there was no way to change
one after the map loaded.

```lua
SetInstanceProperty("cp2_vspawn1", "ClassAllDEF", "cis_fly_droidfighter")
SetInstanceProperty("cp2_vspawn1", "SpawnTime", 15)
```

A spawner's vehicle carries the spawner's name. While both exist, the spawner is the
one written; use `SetProperty` for the vehicle.

A spawner takes the same keys its ODF does: `SpawnTime`, `DecayTime`, `ControlZone`,
`Team`, and the per-side class keys. Beyond the keys in the stock template,
`ClassAlliance`, `ClassEmpire`, `ClassRepublic`, `ClassCIS` and `ClassHistorical`
also work, and are the only way to set the vehicle for teams 3 through 7.

Things to know:

- The vehicle class you name has to already be loaded in the level. Naming one that
  is not leaves the spawner with nothing to spawn.
- A new class only takes effect once the vehicle already parked at the spawner is
  gone.
- `SpawnTime` applies from the next respawn onward, not to a countdown already
  running.
- `SpawnCount` is refused at runtime and logged. It resizes a memory pool that live
  vehicles are allocated out of, so it stays a load-time key.

In multiplayer, vehicle spawning only runs on the host, so call it there.

## Event Callbacks

The engine fires `CharacterEnterVehicle` but never wired up the exit side.
`CharacterExitVehicle` fills that gap, and it is a real engine event rather than a
lookalike: the globals below are generated by the engine's own event manager, so
they take the same arguments, filter the same way and return the same kind of
handle as `OnCharacterDeath` and the rest of the stock `On*` family.

| Function | Description | Since |
|----------|-------------|-------|
| `OnCharacterExitVehicle(fn)` | Fires whenever a character exits a vehicle. | 1.1.0 |
| `OnCharacterExitVehicleName(fn, name)` | Only for characters whose entity name matches. | 1.1.0 |
| `OnCharacterExitVehicleTeam(fn, team)` | Only for characters on that team index. | 1.1.0 |
| `OnCharacterExitVehicleClass(fn, className)` | Only for characters of that ODF class. | 1.1.0 |
| `ReleaseCharacterExitVehicle(handle)` | Unregister a callback. | 1.0.0 |

```lua
local h = OnCharacterExitVehicle(
  function(charIndex, vehicle)
    print("unit " .. charIndex .. " got out")
  end
)

ReleaseCharacterExitVehicle(h)
h = nil
```

`fn` receives the character index (the same index every `GetCharacter*` function
takes) and the vehicle entity as light userdata. As with the stock `On*` events,
the filtered variants match on the *character*, not on the vehicle, and callbacks
only fire on the host in multiplayer.

## Loading Screen

| Function | Description | Since |
|----------|-------------|-------|
| `SetLoadDisplayLevel(path)` | Redirects the loading screen to a custom load lvl. Call from script root or ScriptPreInit. | 1.0.0 |

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

## Match Info

| Function | Description | Since |
|----------|-------------|-------|
| `GetMissionName()` | Returns the mission-script name the match was launched from, e.g. `"cor1c_con"`. Returns nil in the shell. | 1.1.0 |

The name is set before `ScriptPreInit` runs and does not change for the rest of
the match, so a script can branch on its own map or mode without having the name
hardcoded into it. Stock Lua only offers `GetWorldFilename()`, which returns the
world file, not the script and only after the world has been loaded.

```lua
local mission = GetMissionName()          -- "cor1l_con"
local mode    = string.sub(mission, -4)   -- "_con"
```

It returns nil in the shell. The engine never clears the name when a match ends,
so the raw value would keep naming the last map played; BF2GameExt tracks whether
a mission is actually live and reports nil when one is not.

## Rendering

| Function | Description | Since |
|----------|-------------|-------|
| `SetFogEnable(0/1)` | Toggles the D3D fog render state. | 1.0.0 |
| `SetFogRange(start, end)` | Sets near/far fog distances. | 1.0.0 |

## HTTP

Make HTTP requests directly from Lua. Useful for telemetry, live configuration, or external API integration in either singleplayer or multiplayer missions.

| Function | Description | Since |
|----------|-------------|-------|
| `HttpGet(url)` | Synchronous GET. Returns response body as a string, or nil on failure. | 1.0.0 |
| `HttpPut(url, body)` | Synchronous PUT. Returns response body. | 1.0.0 |
| `HttpPost(url, body)` | Synchronous POST. Returns response body. | 1.0.0 |
| `HttpGetAsync(url)` | Fire-and-forget GET on a background thread. | 1.0.0 |
| `HttpPutAsync(url, body)` | Fire-and-forget PUT. | 1.0.0 |
| `HttpPostAsync(url, body)` | Fire-and-forget POST. | 1.0.0 |
