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
| `GameExt_TestVersion(major, minor, patch)` | `true` if the running BF2GameExt is that version or newer, as Shader Patch's `SP_TestVersion` is for SP. `minor` and `patch` can be left out and count as 0. New in 1.2.0, so it is `nil` on older versions and without BF2GameExt. |

```lua
if GameExt then
    print("BF2GameExt " .. GameExt.version .. " on " .. GameExt.build)
    SetFogEnable(1)
end
```

**Checking the version (`GameExt_TestVersion`)** - Check that it exists before calling it.
It is `nil` without BF2GameExt and before 1.2.0, so the whole check comes out false
there, which is right for anything that needs 1.2.0 or later:

```lua
useGameExtHud = GameExt_TestVersion and GameExt_TestVersion(1, 2, 0)
```

Versions older than 1.2.0 do not have it, so it cannot tell them apart. To use
something that arrived earlier, check that the function exists instead, as in
`if SetInstanceProperty then`. Anything but numbers answers false and logs a warning.

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
| `GetCharacterWeapon(charIndex, channel)` | Returns the ODF name of the weapon currently held in the given channel (0 = primary, 1 = secondary). Returns nil if the slot is empty or the channel is anything else. | 1.0.0 |
| `SetCharacterWeapon(charIndex, odfName [, channel])` | Replaces the active weapon in a channel (0 = primary, 1 = secondary) with another already-loaded weapon ODF. Pass the full ODF name, exactly as the file is named; partial names are not matched. Builds a real Weapon through the engine's own factory and destroys the old one , ammo, animation stance, and aimer all come out correct. Singleplayer only; slots using `WeaponShareAmmo`/`WeaponShareEnergy` are refused, as are weapons the unit's animation bank has no animmap for (e.g. giving a jedi-bank unit a rifle). The old weapon is kept in that case. Works in first person. Melee-family weapons (sabers, saber throw) are untested and unsupported. Returns 1 on success, nil on failure. | 1.0.0 |
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

## Flyer Wingmen

*A prototype: modtools only for now, in single player and on the host.*

The player's [flyer wingmen](FEATURES.md#ai-systems) as a mission script shapes them. Whatever a script sets holds for the rest of the mission.

A **unit** is a flyer or other world object, given the way the game's own functions take one: an entity pointer (`GetEntityPtr`, `GetCharacterVehicle`, an event's object), a world object's name, or a character index (its vehicle; for an attack's target, its soldier when it has no vehicle). A vehicle spawner's flyer carries the spawner's name, so naming a spawner names whichever flyer it sends out, the next one included.

| Function | Description | Since |
|----------|-------------|-------|
| `CallWingmen([count])` | Calls wingmen up now if the player flies, and at every takeoff after, even after the player broke a flight off or picked one by hand. `count` sets how many are called up on their own (0 to 8; a negative number: the class's own number again). Returns how many fly with the player. | 1.2.0 |
| `DismissWingmen()` | Lets the flight go and forgets the flyers named to it. None is called up on its own again until `CallWingmen` or the squad key. Returns how many were let go. | 1.2.0 |
| `AddWingman(unit)` | Names a flyer to the player's flight: it joins as soon as it is a friendly AI flyer in the air, from anywhere, before any called up on its own and whatever its class's keys say. Up to 8 named. Returns true once named. | 1.2.0 |
| `RemoveWingman(unit)` | Unnames it and lets it go. It is never called up on its own again. | 1.2.0 |
| `SetWingmenOrder(order [, target])` | `"formup"`, `"engage"` or `"cover"`: the flight's order now, and the one flights start with from then on. `"attack"`: the flight in the air goes for `target`, or without one the player's lock or what is under the reticule, until it is down or 3 km away, then back to the order before. Puts nothing on the HUD. Returns true if given. | 1.2.0 |
| `GetWingmenOrder()` | `"formup"`, `"engage"`, `"attack"` or `"cover"`; nil with no flight. | 1.2.0 |
| `SetWingmenLocked(locked)` | A locked flight calls no one up on its own, not even on the squad key, and lets none go for straying or arriving late. Names, removals and losses still count. | 1.2.0 |
| `SetWingmenFormation([name])` | The flight flies the map's formation shape `name`, or `"vee"`; with no name, its class's own choice again. Returns false if the map has no such shape. | 1.2.0 |
| `SetWingmenLand([land])` | `true`: wingmen land beside the player; `false`: they break off when it lands; with nothing, the class and the INI decide again. | 1.2.0 |
| `GetWingmenCount()` | How many fly with the player. | 1.2.0 |
| `GetWingman(i)` | The `i`th wingman, 1 first, in slot order: its pilot's character index and its flyer. nil past the last. | 1.2.0 |
| `IsWingman(unit)` | Whether it flies in the player's flight. | 1.2.0 |
| `GetWingmanLeader(unit)` | The character index of whoever it flies as a wingman of: the player, or an AI squadron leader's pilot. nil if it flies as no one's. | 1.2.0 |
| `GetWingmanLostReason()` | Inside `OnWingmanLost`, why: `"killed"`, `"pilot"` (someone else flies it), `"landed"`, `"noroom"` (nowhere to land beside the player under a roof), `"team"`, `"far"`, `"late"`, `"removed"`, `"dismissed"`, `"breakoff"` (the player held the squad key), `"formation"` (the new formation holds fewer) or `"leader"` (the player stopped leading). nil anywhere else. | 1.2.0 |

The events are engine events like `OnCharacterExitVehicle`, with the same `Name`, `Team` and `Class` filters on the character and a `Release` function each:

| Event | `fn` receives | Since |
|-------|---------------|-------|
| `OnWingmanJoin(fn)` | A flyer joins the player's flight: its pilot's character index and its flyer. | 1.2.0 |
| `OnWingmanLost(fn)` | One leaves it: its pilot's character index and its flyer, nil once destroyed. `GetWingmanLostReason()` says why. | 1.2.0 |
| `OnWingmenOrder(fn)` | An order is given, by a key or a script, or an attack ends: the player's character index and the attack's target (nil for the other orders). `GetWingmenOrder()` says which. | 1.2.0 |
| `OnWingmanLanded(fn)` | A wingman lands beside the player: its pilot's character index and its flyer. | 1.2.0 |

They fire once the flyer's own update is done, so a callback may call any of the functions above.

```lua
-- Two named wingmen from their spawners for the whole mission, engaging from takeoff.
AddWingman("red2_spawn")
AddWingman("red3_spawn")
SetWingmenLocked(true)
SetWingmenOrder("engage")

OnWingmanLost(function(charIndex, flyer)
   if GetWingmanLostReason() == "killed" then
      ShowMessageText("level.mymap.wingman_down")
   end
end)
```

## AI Goals

*Modtools only for now, with `[LimitIncreases] AIGoalLimit` above 20 (64 by default). Single player and the host.*

AI goals belong to a team: the game puts each AI unit in one of its team's goals by weight and keeps it there until it respawns. These give a unit a goal of its own. A goal is what the game's own `AddAIGoal` returns. Up to 64 goals may be set at once, not 20.

| Function | Description | Since |
|----------|-------------|-------|
| `SetUnitGoal(charIndex, goal)` | Puts the AI unit in `goal`, a goal of its own team, now and again after every respawn, and it takes the goal's order at once. The goal is kept for the units scripts put in it: no one else joins it by weight, whatever its weight. Returns true if done. | 1.2.0 |
| `ClearUnitGoal(charIndex)` | Back to its team's goals by weight. Returns true if it had a goal of its own. | 1.2.0 |
| `GetUnitGoal(charIndex)` | The goal the unit is in now, as `AddAIGoal` gave it, or nil. | 1.2.0 |

Deleting a goal (`DeleteAIGoal`, `ClearAIGoals`) frees its units to the weighted rule. A new mission starts with none.

```lua
-- Character 5, an attacker, holds cp3 whatever its team's goals say, across respawns.
local hold = AddAIGoal(ATT, "Defend", 1, "cp3")
SetUnitGoal(5, hold)
```

## GameExt Class Properties

The stock `SetClassProperty(className, property, value)` also sets the properties BF2GameExt adds to entity classes, as if the line were in the class's ODF. See [From a Mission Script](ODF_PROPERTIES.md#from-a-mission-script).

```lua
-- The commander's mission: the player's officer orders every friendly near it
-- with the comm keys, not its squad alone.
SetClassProperty("rep_inf_ep3_officer", "CommandAnyFriendly", 1)
```

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
| `SetFogEnable(enable)` | Toggles the D3D fog render state. Takes `1`/`0` or `true`/`false`. | 1.0.0 |
| `SetFogRange(start, end)` | Sets near/far fog distances. | 1.0.0 |

## Camera

A mission script can take the player's view: a shot from a fixed spot, a chase camera
on any unit, a flight along a ZeroEditor path, a look toward something, a zoom and a
shake, then a blend back to the player's own camera.

The camera only changes what this machine draws. Nothing reaches the simulation or the
network, and the player's aim and controls are untouched. It works while the player has
a unit in the world. Before they spawn, and while they are dead, the game draws no
player camera for it to take over.

| Function | Description | Since |
|----------|-------------|-------|
| `CameraShot(where [, opts])` | Puts the view at `where`: a matrix (`GetPathPoint`, `GetEntityMatrix`, `CreateMatrix`, `GetCameraMatrix`) or a unit, looking the way it faces, or a point `{x, y, z}`. | 1.2.0 |
| `CameraFollow(unit [, opts])` | A chase camera on any unit, trailing it a little. | 1.2.0 |
| `CameraPath(path, seconds [, opts])` | Flies the camera along a ZeroEditor path in `seconds`, passing through every node. | 1.2.0 |
| `CameraLookAt(target [, opts])` | With a scripted view on screen, aims it at `target`; `nil` goes back to the view's own aim. Without one, turns the player's own view toward `target` for a moment, then back. | 1.2.0 |
| `CameraFOV([fov [, seconds]])` | Changes the field of view of whatever is on screen over `seconds`, in the game's own measure: 48 is its default. With no `fov`, the game has its own back. | 1.2.0 |
| `CameraShake(amount [, seconds])` | Shakes the view as an explosion would: a grenade's is 0.5 and a detpack's 2. Lasts 1 second unless told otherwise. | 1.2.0 |
| `CameraRelease([seconds])` | Blends back to the player's own camera over `seconds` (1 unless told; 0 cuts). | 1.2.0 |
| `GetCameraMatrix()` | The view on screen as a matrix, facing the way the camera looks. | 1.2.0 |
| `GetCameraPosition()` | The view's position, as `x, y, z`. | 1.2.0 |
| `IsCameraScripted()` | True while a script holds the view, a release included. | 1.2.0 |

A **unit** is given the way the stock functions give one: an entity pointer
(`GetEntityPtr`, `GetCharacterUnit`, `GetCharacterVehicle`, an event's object), a
world object's name, or a character index, which follows whatever that character
is in. A pointer has to be a named object or one a character is in now.

**Options** every view takes, in the `opts` table:

| Option | Meaning |
|--------|---------|
| `lookAt` | A unit or point to keep the camera aimed at, moving or not. `lookAtOffset = {right, up, forward}` moves the aim point on a unit (1 m up unless told). |
| `fov` | The field of view while it holds the view, in the game's measure. |
| `blend` | Seconds to blend in from the view on screen. 0, the default, cuts. |
| `time` | Seconds before it lets go by itself. Without it, the view holds until `CameraRelease` or another camera command. |
| `release` | Seconds that letting go blends back over (1 unless told). |

And what each command adds:

| Command | Option | Meaning |
|---------|--------|---------|
| `CameraShot` | `offset` | `{right, up, forward}` metres from `where` in its own frame; for a point, in world `x, y, z`. |
| `CameraFollow` | `offset` | `{right, up, forward}` from the unit, `{0, 4, -12}` unless told: 4 m up and 12 m behind. |
| | `aim` | The point on the unit it looks at, `{0, 1, 0}` unless told. |
| | `lag` | Seconds the camera trails its spot by, 0.3 unless told; 0 sticks to it. |
| | `world` | `true` keeps the offset in world axes, so the camera does not swing round with the unit. |
| `CameraPath` | `aim` | `"path"`, the default, looks the way it flies; `"nodes"` turns to each node's rotation as set in ZeroEditor. `lookAt` overrides both. |
| | `ease` | `"inout"`, the default, `"in"`, `"out"` or `"linear"`. |
| | `loop` | `true` flies it round and round, back to the first node at the end. |
| | `release` | Lets go at the end, over that many seconds. Without it, the last view holds. |
| `CameraLookAt` | `time`, `blend`, `release` | Without a scripted view: how long the look holds (2 seconds), turns there (0.5) and turns back (0.5). |
| | `offset` | Moves the aim point on a unit, as `lookAtOffset` does. |

Things to know:

- A new camera command replaces the one before it, blending from the view on screen.
- While a shot, a follow or a path holds the view, the player is shown in third person,
  so their arms and cockpit do not hang in the shot. Their own first-person setting comes
  back once the release has finished. A cut from first person waits a frame or two for
  the switch.
- If the unit a camera follows or looks at is destroyed or deleted, the camera stays
  where it last was.
- Explosions near the player and `CameraShake` shake a scripted view too. The player's
  own shakes, from firing, landing or being hit, do not.
- A scope's zoom gives way to a shot and comes back after.
- The HUD stays on screen.
- Each command returns `true`, or `nil` with a warning in the game's log when an argument
  is wrong, such as a path the map does not have.

```lua
-- A flyby: the camera rides "cam_dropship" in 8 seconds, watching the lead
-- gunship, then hands the view back over 1.5 seconds.
CameraPath("cam_dropship", 8, { lookAt = "laat_lead", release = 1.5 })

-- A fixed shot of the landing pad from a ZeroEditor path node, with a slow zoom.
CameraShot(GetPathPoint("cam_spots", 2), { blend = 1, fov = 30, time = 5 })

-- Ride along behind the player's wingman, 20 m back and 6 m up, for 6 seconds.
CameraFollow("red2_spawn", { offset = { 0, 6, -20 }, lag = 0.5, time = 6, release = 1 })

-- Make the player glance at the shield generator when it blows.
CameraLookAt("shield_gen", { time = 1.5 })
CameraShake(1.5, 2)
```

`GetCameraMatrix()` keeps a view to come back to: store it in a script variable and
hand it to `CameraShot` later.

## Coroutines

The game's Lua has coroutines (`coroutine.create`, `resume`, `yield`, `wrap`), but the
game's own functions did not work inside one: they read their arguments from the main
script's stack whatever called them, so inside a coroutine they got the wrong arguments
and their results went missing. With BF2GameExt they work inside coroutines as anywhere
else, so a sequence can be written top to bottom with waits in it. Since 1.2.0; check
`GameExt_TestVersion(1, 2, 0)` before relying on it.

A coroutine pauses with `coroutine.yield` and goes on when something resumes it. The
game has no wait of its own, so a small helper resumes it from a timer:

```lua
-- Sequence(body) runs body as a coroutine; Wait(seconds) inside it pauses it.
local sequenceWaits = 0
local function Sequence(body)
   local co = coroutine.create(body)
   local function step()
      local ok, seconds = coroutine.resume(co)
      if not ok then
         print("Sequence: " .. tostring(seconds))
      elseif seconds then
         sequenceWaits = sequenceWaits + 1
         local t = CreateTimer("sequence_wait" .. sequenceWaits)
         SetTimerValue(t, seconds)
         StartTimer(t)
         OnTimerElapse(function(timer) DestroyTimer(timer) step() end, t)
      end
   end
   step()
end

local function Wait(seconds)
   coroutine.yield(seconds)
end

-- The first time a human player spawns, a short scene.
local played = false
OnCharacterSpawn(function(player)
   if played or not IsCharacterHuman(player) then return end
   played = true
   Sequence(function()
      Wait(3)
      CameraFollow(player, { offset = { 0, 3, -8 }, blend = 1 })
      Wait(4)
      local unit = GetCharacterUnit(player)   -- a stock function, inside the coroutine
      if unit then
         CameraShot(GetEntityMatrix(unit), { offset = { 0, 5, 15 }, lookAt = player, blend = 1 })
      end
      Wait(4)
      CameraRelease(1)
   end)
end)
```

Things to know:

- Only a coroutine can wait. An event callback such as `OnCharacterSpawn`'s function is
  not one, so start a `Sequence` from it rather than calling `Wait` in it directly.
- An event callback that a game function fires from inside a coroutine (a `KillObject`
  firing `OnObjectKill`, say) runs at once, as it always does, and cannot wait either.
- An error inside a coroutine stops that coroutine only: `coroutine.resume` returns
  `false` and the message, which `Sequence` above prints.
- In multiplayer, timers and event callbacks run on the host, so a sequence that waits on
  them runs there. A script that relies on this needs BF2GameExt on the machine that runs
  it.

## HTTP

Make HTTP requests directly from Lua. Useful for telemetry, live configuration, or external API integration in either singleplayer or multiplayer missions.

The synchronous calls pause the game until the server answers. A request gives up and returns nil when the server does not connect or respond within 5 seconds.

| Function | Description | Since |
|----------|-------------|-------|
| `HttpGet(url)` | Synchronous GET. Returns response body as a string, or nil on failure. | 1.0.0 |
| `HttpPut(url, body)` | Synchronous PUT. Returns response body. | 1.0.0 |
| `HttpPost(url, body)` | Synchronous POST. Returns response body. | 1.0.0 |
| `HttpGetAsync(url)` | Fire-and-forget GET on a background thread. | 1.0.0 |
| `HttpPutAsync(url, body)` | Fire-and-forget PUT. | 1.0.0 |
| `HttpPostAsync(url, body)` | Fire-and-forget POST. | 1.0.0 |

## Diagnostics

| Function | Description | Since |
|----------|-------------|-------|
| `GameExtContentCensus()` | Writes the content budget report to `BF2GameExt.log` straight away: what the map loaded against the limits the engine imposes, starting with effect classes out of 256. Works on every build, including retail, which has no debug console. | 1.2.0 |

`ContentCensus()`, the name it shipped under in 1.0.0, still works. To run the same
report on a timer instead, set `[Diagnostic] ContentCensus` in `BF2GameExt.ini`; see
[Configuration](CONFIGURATION.md).
