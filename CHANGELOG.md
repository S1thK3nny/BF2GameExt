# Changelog

What changed in each release of BF2GameExt. Version numbers follow the rules in
`version.h`: a major release can break existing scripts, ODFs or INI files, a minor
release only adds, and a patch release only fixes.

Scripts can check the running version through `GameExt.version`.

## Unreleased

### Added

- **Command post strip** - `player1.commandPostN.icon`, `.color`, `.capture`,
  `.captureColor` and `.disable` for slots 1 to 16, plus `player1.commandPosts.count`,
  so a `.hud` can show every command post's owner and capture progress in a row.
  Slots follow `HUDIndex`; neutral posts use team 0's `SetTeamIcon`. Works on
  multiplayer clients; inert unless a `.hud` binds it.
  See [HUD authoring](docs/user/HUD.md#command-post-strip).
- **`FillFrom` for HUD bars** - A `BarBitmap` can keep its right end and grow or
  shrink at its left (`"Right"`), or fill vertically from its bottom or top (`"Bottom"`,
  `"Top"`), showing the same part of its texture the full bar shows there, so an
  upright icon fills without being rotated. `"Right"` keeps the bar flash; the vertical
  modes have none. Inert unless a `.hud` uses it.
  See [HUD authoring](docs/user/HUD.md#fillfrom).
- **Weapon icon textures** - `player1.weapon1.iconTexture` and
  `player1.weapon2.iconTexture` carry the stock `IconTexture` of the weapon in each
  slot, following the same weapon as the stock weapon events, with Disable twins.
  Inert unless a `.hud` binds them.
  See [HUD authoring](docs/user/HUD.md#weapon-icons).
- **Class, stance and vehicle health icons** - HUD events carrying the health icon of
  what the local player controls, taken from the class's stock `HealthTexture`:
  `player1.unit.healthTexture` follows the stance through `<name>_crouch`,
  `<name>_prone` and `<name>_ball` (prone falls back to crouch, then standing),
  `player1.unit.stance` carries the stance itself, and `player1.vehicle.healthTexture`
  carries the entered vehicle or turret's icon. The texture events have Disable twins.
  Published from the HUD update, so they work on multiplayer clients; inert unless a
  `.hud` binds them. Modtools, Steam and GOG.
  See [HUD authoring](docs/user/HUD.md#class-stance-and-vehicle-icons).
- **`TransformNumberMath`** - A HUD transform that adds, subtracts, multiplies,
  divides or takes the minimum or maximum of two events or constants, optionally
  clamped, and publishes the result as a new event, for values the stock transforms
  cannot make, such as missing health or a ticket lead. Inert unless a `.hud`
  declares one. See [HUD authoring](docs/user/HUD.md#transformnumbermath).
- **`EnableStrafe`** - Flyer ODF property that makes the strafe controls slide the
  flyer sideways at `StrafeSpeed` instead of rolling it, leaning by `StrafeRollAngle`,
  and lets it come out of reverse as quickly as it brakes into it.
  Off by default and inherited through `ClassParent`.
  See [ODF properties](docs/user/ODF_PROPERTIES.md#vehicle-classes).
- **`DisableProne` / `DisableCrouch`** - Soldier ODF properties that take prone or
  crouch away from a unit, for the AI as well as the player. Off by default and
  inherited through `ClassParent`.
  See [ODF properties](docs/user/ODF_PROPERTIES.md#soldier-classes).
- **`player1.reticule.horizonRotation`** - A camera-driven HUD rotation event for
  world-up reticules, including banked and inverted views. Bind `EventRotation`
  on an unscaled pivot with artwork sizing in a child group; position and native
  reticule events are unchanged. Holds the last reliable angle near vertical.
  Always available, independent of target bars, with no INI setting.
- **`player1.weaponN.target.position`** - A new HUD event carrying the current target's
  position on screen. Bind it with `EventPosition` on the group that holds the target
  health bar and the bar floats on the unit. The last target the game selected is held
  for 0.5 seconds after the selection is lost, instead of fading the moment the reticle
  slips off; a new selection replaces it at once. A vehicle and its exposed rider no
  longer flicker between each other: the bar stays on the one selected first until the
  other holds the selection for 0.3 seconds. It does nothing unless a `.hud` file binds
  the event, so stock HUDs are unchanged. Support is inherently on; only
  `[Features] TargetBarLatchSeconds` remains configurable. Placement projects one
  world-bounds top-centre point (no animated-bone wobble) and snaps it to framebuffer
  pixels. While selected, the anchor stays inside the screen's safe area, so a big
  vehicle up close keeps its bar; otherwise the bar leaves the screen with its target.
  Bar sizes, scales, labels and offsets stay in the `.hud`.
- **Foley regions** - A `foleyfx <group>` region now changes the footstep, landing
  and impact sounds on the ground inside it to that foley group. In stock BF2 these
  regions did nothing. See [Features](docs/user/FEATURES.md).
- **Foley sounds from several sound files** - Soldiers no longer lose their footstep
  and impact sounds when a mission script loads world sound files from more than one
  map. Missions that load a single one are unchanged.
- A [HUD properties](docs/user/HUD_PROPERTIES.md) reference listing every event,
  property and transform parameter BF2GameExt adds to `.hud` files, with the version
  each first appeared in.

### Fixed

- Carriers (vehicle pads with `SetCarrierClass`):
  - A pad no longer stops spawning forever when its vehicle is destroyed while still being carried.
  - A carrier whose landing fails (slope, water) now drops its vehicle instead of flying off with it.
  - Every carrier after the first from the same pad now descends with its cargo bay closed.
  - Extra vehicles on multi-cargo carriers now get their team back when dropped.
  - Carriers no longer look skewed while climbing away.
  - Carrier turrets keep cooling down and reloading while searching for a target.
  - A carrier bringing a tall vehicle such as an AT-AT now lands and drops it instead of
    hovering over the pad.
  - Shooting down a carrier that is still carrying an AT-AT or another command walker no
    longer crashes the game.
  - Carriers no longer set their vehicle down short of the pad on maps with a low flight
    ceiling.
  - Hosting a multiplayer match on a map with carriers no longer crashes the game.
  - A carrier shot down in the air now explodes on the spot instead of spinning all the
    way to the ground.
- Floating target bars keep their last screen position when a target dies or is
  removed, allowing the existing HUD fade to finish without dropping onto the
  corpse. Living targets still track normally; new targets reset the cached anchor.
- A map rotation no longer crashes when a map with `foleyfx` regions follows another
  map that has them.
- The game no longer crashes, usually on the first spawn, on a map whose terrain
  paints a foliage layer its props do not define when a map that did define that
  layer was played earlier in the session.
- A multiplayer client no longer crashes when a landed or crashed aircraft whose ODF
  has no `ExplosionDestruct` is removed.
- A multiplayer client on Steam or GOG no longer crashes when a grappling hook is
  fired from a weapon that is not a grappling hook weapon.

## 1.1.0

### Added

- **`GetMissionName()`** - Returns the mission-script name the match was launched
  from, such as `"cor1l_con"`. Stock Lua exposed the world file but never the
  script, so a mission could not tell which map or mode it was running as without
  the name being hardcoded. See the
  [Lua API](docs/user/LUA_API.md#match-info).
- **`SetInstanceProperty(name, property, value)`** - Changes a world object's
  instance properties after the map has loaded. Works on vehicle spawners, which no
  stock Lua function could reach, so a script can now change which vehicle a
  spawner produces or how quickly it respawns. See the
  [Lua API](docs/user/LUA_API.md#world-objects).
- **`@GameExt` ODF suffix** - A property written as `Name@GameExt` replaces the
  plain `Name` line only when BF2GameExt is installed, so one ODF serves both stock
  and extended players. See [ODF Properties](docs/user/ODF_PROPERTIES.md).
- **`HeldOrdnanceEffectBone`** - A cannon weapon can show its projectile trail at a
  soldier bone while preparing to fire, then carry it into flight. INI:
  `[Fixes] HeldOrdnanceEffect`.
- **`ExtendedBladeBase`** - The visible lightsaber blade extends 8% behind its base
  instead of 4%, matching the Classic Collection. Combat reach is unchanged. INI:
  `[Lightsaber] ExtendedBladeBase`.
- A `Since` column in the Lua API and ODF property references, naming the version
  each entry first appeared in.

### Changed

- **`OnCharacterExitVehicle` now runs on the engine's own event manager.** The
  callback names, arguments and handles are unchanged, but registration, filtering,
  release and multiplayer behaviour are the engine's rather than a parallel
  implementation, so the event behaves exactly like the stock `On*` family. Lifts
  the old 64-callback ceiling and fixes callbacks surviving into the next mission.
  See [the RE notes](docs/RE/OnEventSystem.md).
- **Combo animation limits are now raised by default.** Combo animation names go
  from 30 to 90, animation banks from 16 to 64, bank/weapon maps from 30 to 90 and
  animation references from 256 to 768, and the extra animations actually play.
  `[LimitIncreases] ComboAnimIncrease` was previously off and marked unsafe.
- **The tentacle limit is now raised by default**, from 4 to 9, keeping the
  original offline and multiplayer timing. `[LimitIncreases] TentacleLimit` was
  previously off.
- Starting the game on the original 2006 executable now names that as the problem,
  instead of a generic failure. That executable is still not supported.
- **Going prone is now a committed move.** It plays the weapon handling sound,
  and your input is held for the length of the getdown animation instead of
  letting you walk out of it halfway. On the modtools executable it also plays
  the first-person hands-down transition; the retail executables do not contain
  that animation path at all, so first person there is unchanged.
- **AI now uses the prone positions the levels already mark out.** Map layouts give
  every AI cover, patrol and sniper position a list of stances the AI may take there,
  prone among them, but the game only ever read stand and crouch out of that list.
  Positions marked prone are now used as marked, including in the stock maps. Needs
  prone enabled; with it off, those positions behave as before.

### Fixed

- **The `OnCharacterExitVehicle*` filter arguments were documented in the wrong
  order.** The 1.0.0 Lua API reference listed `OnCharacterExitVehicleName(name, fn)`;
  the functions have always taken `(fn, name)`, matching the stock `On*` events, and
  a script written from the old docs silently registered nothing.

- A map could die at the very end of loading, after the loading screen, with a
  stack overflow. The engine's world splitter recursed forever on objects it
  cannot tell apart: objects at exactly the same spot, objects stacked at one
  point, or an object with a broken position. The split now stops once it has
  gone deeper than any real map needs, and that group of objects is left as it
  is.
- An effect attached with `AttachEffect` stayed on screen after the object that
  carried it was deleted. The effect is now removed along with the object.
- **`SetCharacterWeapon` did nothing on Steam and GOG.** It asked the game whether the
  new weapon had animations for that unit in a way those two builds do not answer, so
  every swap was refused as if the animations were missing. It now works on all three
  builds. The weapon name must be the full ODF name; shortened names are no longer
  accepted on any build.
- Tentacles beyond the fourth lost their pose whenever another unit was drawn.
- Above 60 FPS, the top of a cape detached from the body and jittered while the
  wearer moved. This was caused by BF2GameExt's own cloth collision fix.
- A crash shortly after a loading screen that loads its own sounds.
- Random crashes while writing to the log on the modtools executable, when a sound
  thread and the game logged at the same moment.
- With prone enabled, AI soldiers dropped prone at roughly half the cover positions
  they took, including positions meant to be used standing.
- Animated projected textures on `light` class objects showed the same frame for
  every step of the animation. Credit to Sleepy, who found and fixed this one.
- Droidekas turned on the spot to face whatever killed them while their death
  animation played.
- The game could crash on startup, most often with the modtools exe, and keep
  crashing until the PC was restarted. It now starts with a slightly smaller sound
  memory pool when the full one does not fit, and says so in the log. If sound
  cannot start at all, the game runs without sound instead of crashing.
- While looking through a scope, shots left the barrel instead of the centre of the
  scope, so they visibly angled in towards the target.

## 1.0.0

First release.
