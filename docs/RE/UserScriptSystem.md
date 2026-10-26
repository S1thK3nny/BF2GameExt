# User Script System (design)

Status: **planned, nothing implemented.** Backlog entry in [ROADMAP.md](../../ROADMAP.md#lua-api).

A user script is a munged `.lvl` holding Lua that runs on its own, without being part of a
map: in the shell (menus) or in every mission. GameExt loads them from each mod's own folder,
with no numbering, no fixed ID and no in-game registration.

## Why not the existing loaders

**v1.3 patch** (`game_interface.lua`, `user_script_10.lua`):
- Scripts are `data\_LVL_PC\user_script_<N>.lvl`. It probes 0 to 10, then counts up from 11
  and **stops at the first missing number**, so 11, 12, 14 never loads 14.
- The number is the identity. The lvl name AND the `scr_` name inside it must both be
  `user_script_<N>`, because the loader calls `ScriptCB_DoFile("user_script_" .. i)`. Every
  author ships an "11", two of them cannot coexist, and one cannot be renamed without a re-munge.
- Everything goes into the stock data folder.

**Remaster 1.5** (`script_manager.lua`):
- Scripts are per mod, `addon\<ID>\scripts\<ID>_game_script.lvl` (also `_interface_`,
  `_option_`, `_theme_`), which is the right idea.
- Discovery brute-forces every 3-character base36 ID: up to 46,656 `ScriptCB_IsFileExist`
  calls, and only when the user presses a button in its options screen.
- The result is saved in the profile and smuggled into the mission through
  `ScriptCB_SaveMetagameState`. Game scripts only run if that database survives.
- Every mod needs a unique 3-letter ID, and installing anything needs a manual rescan.

GameExt can list directories from C++ and keeps memory across Lua states, so none of that
machinery is needed.

## Folder layout

Two roots, same inner layout:

```
GameData\addon\<Mod>\userscripts\shell\*.lvl     a map/era mod that also ships scripts
GameData\addon\<Mod>\userscripts\ingame\*.lvl

GameData\userscripts\<Mod>\shell\*.lvl           a mod that is only scripts
GameData\userscripts\<Mod>\ingame\*.lvl
```

- `shell\` runs in the shell Lua state, `ingame\` in every mission's state.
- `<Mod>` and the lvl file names are free. Nothing is numbered or registered.
- `GameData\userscripts\` sits next to `addon\` and `data\`. The engine's addon scan never sees
  it, so a script-only mod needs no `addme`. Nothing goes into `data\_LVL_PC`.
- Uninstall is deleting the mod's folder. A script-only mod that grows into an addon moves its
  `shell\` / `ingame\` folders into `addon\<Mod>\userscripts\` unchanged.

## Discovery

- `FindFirstFileA` over `addon\*\userscripts\<phase>\*.lvl` and `userscripts\*\<phase>\*.lvl`.
  The working directory is always `GameData`.
- Rescanned on **every** shell start and mission load. Dropping a file in works without a restart.
- Order: mods from both roots sorted together by folder name (case-insensitive; on a tie the
  addon goes first), then files within a mod by name. Prefixes (`10_base.lvl`, `50_tweaks.lvl`)
  give an explicit order and gaps do not matter.
- Disabled by deleting or renaming the file (`.lvl.off`). INI: `[Features] UserScripts=1`
  master switch, optionally a `Disable=` list so a user can turn off one script without
  touching mod files.

## Running a lvl

GameExt reads the lvl file itself and walks its `ucfb` chunks for every `scr_`
(`NAME` = script name, `BODY` = compiled Lua 5.0 chunk starting `\x1bLuaP`, see the BF2
bytecode notes). The main script's `BODY` is run directly with `lua_dobuffer`, chunk name
`@<Mod>/<phase>/<file>.lvl`, so errors name the real file.

The engine's script table is **not** used to find it, so neither the lvl name nor the
internal script name matters. Two old 1.3 `user_script_11.lvl` files renamed to
`snipers_only.lvl` and `server_name.lvl` both run, without a re-munge.

The engine's `ReadDataFile` is called on the lvl only when it also carries assets
(textures, sounds, sides). A scripts-only lvl never goes through the engine loader.

When it does, `ReadDataFile` also registers the lvl's `scr_` chunks in the engine's script
table (see [Engine script table](#engine-script-table)). The table is global, a lookup
returns the **oldest** entry with that name, and it is only cleared between states. So a
user lvl's `main` or `utils` left there would answer every later `ScriptCB_DoFile("main")`
for the rest of the mission, including one from a map that loads its own `main` afterwards.
GameExt therefore records the list tail before the read, and afterwards unlinks every entry
appended after it and frees their bodies, the same two steps the engine uses
(`FUN_00488160` unlink, `FUN_00487e30` free body). The scripts still run from GameExt's own copy.

Each script runs in its own protected call. A failure logs
`[UserScripts] <Mod>\<phase>\<file>.lvl failed: <err>` and the next script still runs.

### Picking the main script

First rule that gives exactly one answer wins:

1. **A `scr_` named `main`.** The documented rule, and the only one new authors need.
   Always wins, even when the lvl also holds old 1.3 scripts.
2. **The lvl holds exactly one `scr_`.** It runs whatever its name. Old 1.3 files and quick
   one-file scripts.
3. **Exactly one `scr_` that no other script in the lvl refers to.** Found by scanning each
   `BODY`'s string constants, nested functions included. A helper's name appears there because
   the main calls `ScriptCB_DoFile("helper")`. Exists only for old 1.3 lvls with helpers; logs a
   hint to rename the entry script to `main`. Can be dropped if strictness is preferred, at the
   cost of those lvls needing a re-munge.
4. **Otherwise nothing from the file runs** and the log says:
   `<file> has 3 scripts (a, b, c) and none is clearly the main one; name the entry script "main".`

"Named like the lvl file" is deliberately not a rule: it would bring back the file name
dependency, and an ordering prefix would silently break it.

Rules 2 and 3 are documented only under "Using v1.3 scripts".

### Helpers

The main loads helpers with plain `ScriptCB_DoFile("helper")`, so 1.3 scripts work unchanged.
GameExt hooks `ScriptCB_DoFile` and resolves the name:

1. While a user script is loading: its **own lvl first**. Two mods can each have a `helper`.
2. Otherwise GameExt walks the engine's script list itself, silently. If the name is there,
   call the original, so stock and mission scripts behave exactly as before.
3. If the engine does not have it: the user-script lvls loaded in this Lua state, only when
   exactly one has that name; otherwise log the clash.
4. Still nothing: call the original, which reports the miss the way it always has.

The walk in step 2 is needed because the original is not a safe probe: on modtools a miss logs
`Script file %s not found` at error severity before returning. Retail has that log compiled
out and returns silently.

Rule 3 covers helpers loaded late (inside a wrapped `ScriptPostLoad`). The tutorial still says
to load helpers at the top of `main`.

The engine frees a script's body after running it once, so a second `ScriptCB_DoFile` of a stock
script finds an empty entry. GameExt keeps its own copies until the state ends, so a user
script's helper can be loaded again.

## When scripts run

Both hook points already exist on modtools, Steam and GOG (`menu_navigation.cpp`,
`prone_lvl_load.cpp`), as does `lua_dobuffer`.

| Phase | Hook | State at that moment |
|---|---|---|
| shell | `ShellLoop::Init` return | Fresh state, every `ifs_*` screen exists |
| ingame | Lua `ReadDataFile("ingame.lvl")` return | After `game_interface`; mission has defined `ScriptPostLoad` but not yet called `AddUnitClass` / `SetupTeams` |

What the `ReadDataFile` callback does for `ingame.lvl` (modtools `Lua_Callbacks::ReadDataFile`
`0x0046A790`), in order:

1. `LoadUtil::ReadDataFile` reads the whole lvl.
2. `FirstPerson::Init`, then the `gDemoBuild` / `gPCBetaBuild` globals.
3. `LuaScript::LoadScript("game_interface")`, synchronously. `game_interface` is a script inside
   `ingame.lvl`, so it cannot run before step 1.
4. `MissionPlayList::PushToLua`, then the callback returns. **GameExt user scripts run here.**

v1.3's user scripts run at the top of `game_interface` (step 3), before its `globals`,
`interface_util` and `ifelem_*` loads. Anything a 1.3 script redefines that `game_interface`
also defines is overwritten moments later. Running after it lets user scripts wrap those
functions. If a script ever needs to set a global that `game_interface` reads while it
initialises, an `ingame_early\` phase could hook `LuaScript::LoadScript` for the name
`game_interface`, which is 1.3's spot. Not planned until something needs it.

Neither spot reaches the mission's `ScriptPreInit`, which runs before `ScriptInit` reads
`ingame.lvl`.

## Lua API

```lua
-- Set only while a user script loads; scripts keep a local copy.
GameExt.UserScript = {
    mod   = "MyMod",
    root  = "userscripts",                      -- or "addon"
    dir   = "..\\..\\userscripts\\MyMod\\",     -- for ReadDataFile of the mod's own assets
    phase = "ingame",                           -- or "shell"
    file  = "foo.lvl",
}

-- Everything that loaded in this Lua state, in load order.
for i, s in ipairs(GameExt.GetUserScripts()) do
    print(s.mod, s.root, s.phase, s.file)
end
```

No register or unregister call. Being in the folder is enough.

Where each lives:
- `GameExt.GetUserScripts` is a normal table member, registered with `set_func` next to
  `TestVersion` / `IsPatchActive` in `register_lua_functions` (`lua/lua_funcs.cpp`). That runs
  from the `init_state` hook in every Lua state, shell included. The function body and the list
  it returns live in `lua/user_scripts.cpp`; the list is rebuilt each time a phase runs, since
  every shell entry and mission is a fresh state.
- `GameExt.UserScript` is set by the loader in `lua/user_scripts.cpp` right before each `main`
  runs and set back to `nil` after it. Helpers load inside that call, so they see it too.
- `_G.GameExt` can be the plain `true` fallback when the table could not be built, so
  `lua_funcs.cpp` exports a `gameext_push_table(L)` that only succeeds when the table exists,
  and the loader uses that instead of indexing `_G.GameExt` itself.
- User docs: `docs/user/LUA_API.md`, next to `TestVersion` / `IsPatchActive`.

## What an author writes

```
GameData\userscripts\SnipersOnly\ingame\snipers_only.lvl
```
```
ucft
{
    REQN
    {
        "script"
        "main"
        "snipers_util"
    }
}
```
```lua
-- main.lua
local me = GameExt.UserScript
ScriptCB_DoFile("snipers_util")          -- from this lvl, even if another mod has one

if ScriptCB_InMultiplayer() then return end

local origAddUnitClass = AddUnitClass
AddUnitClass = function(team, class, ...)
    if class and string.find(string.lower(class), "sniper") then
        return origAddUnitClass(team, class, unpack(arg))
    end
end

local origPostLoad = ScriptPostLoad
ScriptPostLoad = function(...)
    if origPostLoad then origPostLoad(unpack(arg)) end
    ReadDataFile(me.dir .. "ingame\\sniper_sides.lvl")
end
```

Author rules for the tutorial:
- Wrap, never replace: keep the original and call it. Other user scripts, v1.3 and GameExt
  may wrap the same function.
- Copy `GameExt.UserScript` into a local at the top; it changes when the next script loads.
- Locals do not cross between scripts; share through one global table
  (`SnipersOnly = SnipersOnly or {}`).
- Lua 5.0.2: `arg` / `unpack(arg)`, `table.getn`, `string.gfind`.
- An error at load is contained. An error inside a wrapper later belongs to the mission.
- Multiplayer: ingame scripts run separately on the host and every client, and gameplay events
  only fire on the host. Rule changes should check `ScriptCB_InMultiplayer()`, or every player
  needs the same script.

## Coexistence

- v1.3 and Remaster loaders stay untouched and keep running their own scripts.
- Their layouts are not adopted automatically, which would double-run scripts when those
  patches are installed. A migration note covers moving a 1.3 or 1.5 script over.

## Engine script table

Verified on modtools in Ghidra and on Steam and GOG from the exe bytes; same code on all three.

| | modtools | Steam | GOG |
|---|---|---|---|
| `Lua_Callbacks::ScriptCB_DoFile` (lua_CFunction) | `0x004582A0` | `0x0057DCC0` | `0x0057EA40` |
| `LuaScript::LoadScript(name)` | `0x004884B0` cdecl | `0x005A1B90` name in ECX | `0x005A2B30` name in ECX |
| find by hash | `0x004881A0` thiscall(list, hash) | `0x005A1B00` stdcall(hash) | `0x005A2AA0` stdcall(hash) |
| list object | `0x00ACA790` | `0x007EBB7C` | `0x007ECB4C` |

- `ScriptCB_DoFile` accepts exactly one string argument and calls `LoadScript`; it always
  returns 0 results.
- `LoadScript` hashes the name (`PblHash`), finds the entry, runs `lua_dobuffer(L, body, size,
  name)`, then **frees the body** (`entry+0x18`, size `entry+0x14`, from `RunTimeHeap`), so every
  script runs once per registration. That is why v1.3 calls `ReadDataFile` again before each
  `ScriptCB_DoFile`. A miss on modtools logs `Script file %s not found` (error severity) and
  returns 0; retail has no such string and returns 0 silently.
- The list is an intrusive doubly linked list. The list object's `+4` is the first node, `+8`
  the last, `+0x10` the count; the object itself is the end sentinel (its `+0xC` is 0). Node /
  entry (0x1C bytes): `+4` next, `+8` prev, `+0xC` self, `+0x10` name hash, `+0x14` body size,
  `+0x18` body.
- `ReadDataFileChunk` registers each `scr_` (`FUN_00488610`: parse `NAME` / `BODY`, then
  **append at the tail**, `FUN_00488360`). Lookup walks from the head, so with duplicate names
  the **first registered wins** and later ones are unreachable. This is also why loading a
  custom `dc:ingame.lvl` before `ingame.lvl` lets a mod's own `game_interface` replace the stock one.
- Entries are removed one by name only by `AddMissionCommon` (via `FUN_004885D0`) and all at once
  by `PostStateCleanup` (`FUN_00488300`). Nothing else touches the list.

## Custom ingame lvls

The callback's `ingame.lvl` branch is an exact `_stricmp` on the argument, so
`ReadDataFile("dc:ingame.lvl")` or an `..\..\addon\...\ingame.lvl` path runs no
`game_interface`. GameExt's hook uses the same test, so it fires exactly when the engine runs
`game_interface`.

A sweep of the stock mission scripts plus a large set of mod sources (758 plain `ingame.lvl`
reads): all 114
scripts that read a custom ingame lvl also read the plain one, always **after** the custom one,
and no script reads the plain one twice. So user scripts see the mod's custom ingame content
too. The loader still runs at most once per Lua state, in case a script does read it twice.

## Work breakdown

| Step | Size |
|---|---|
| Directory scan, sort, INI switch | small |
| lvl reader: `ucfb` walk, `scr_` NAME/BODY, asset-chunk detection | small |
| Main-script rules 1, 2, 4 and running through `lua_dobuffer` with protected calls | small |
| `GameExt.UserScript`, `GameExt.GetUserScripts` | small |
| Call from the existing shell and ingame hooks (share them with menu nav / prone) | small |
| `ScriptCB_DoFile` hook (addresses known, see above), per-lvl resolution, silent list walk | small to medium |
| Unlink and free the user `scr_` entries an asset lvl registers | small |
| Rule 3: Lua 5.0 proto walker over the BF2 bytecode layout | medium |
| Docs: FEATURES.md, LUA_API.md, tutorial with example `.req` and layout, 1.3/1.5 migration | medium |
| Test on modtools, Steam, GOG | medium |

Suggested order: the scripts-only path with rules 1, 2 and 4 first (usable on its own), then
the `ScriptCB_DoFile` hook, then assets plus the duplicate-name question, then rule 3.
