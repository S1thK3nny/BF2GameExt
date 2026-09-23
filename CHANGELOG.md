# Changelog

What changed in each release of BF2GameExt. Version numbers follow the rules in
`version.h`: a major release can break existing scripts, ODFs or INI files, a minor
release only adds, and a patch release only fixes.

Scripts can check the running version through `GameExt.version`.

## Unreleased

### Added

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
  health bar and the bar floats on the unit. Hitting an enemy also keeps the bar on them
  for a short hold instead of fading the moment the reticle slips off; aiming at anyone
  else drops it, and an enemy behind cover hides it until they reappear. It does nothing
  unless a `.hud` file binds the event, so stock HUDs are unchanged. Support is
  inherently on; only `[Features] TargetBarLatchSeconds` remains configurable.
  Placement projects one
  world-bounds top-centre point (no animated-bone wobble), pins it inside the
  screen using built-in edge reservations, and snaps the anchor to
  framebuffer pixels. Bar sizes, scales, labels and offsets stay in the `.hud`.

### Fixed

- Floating target bars keep their last screen position when a target dies or is
  removed, allowing the existing HUD fade to finish without dropping onto the
  corpse. Living targets still track normally; new targets reset the cached anchor.

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
