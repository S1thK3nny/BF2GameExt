# In-Game Movie System

Reverse-engineering notes for `ScriptCB_PlayInGameMovie` and the `GameMovie` /
`RedMovie` layer behind it. All addresses are unrelocated (imagebase `0x400000`).

## Why custom in-game movies never worked

The long-standing modding complaint is that a custom in-game movie has to
*overwrite* the stock `ingame.mvs`; a movie with any other filename simply never
plays. That is not a path problem, it is an argument problem.

The dev build (Phantom `0x006466d0`) reads both Lua arguments:

```c
seg  = luaL_checklstring(L, 2);
file = luaL_checklstring(L, 1);
StartInGameMoviePlay(file, seg);
```

Every shipping build reads **only argument 2** and substitutes the filename from
a hardcoded language table:

```c
switch (GetLanguage()) {
default: file = "ingame.mvs";   break;   //  0..2, 5, 6
case 3:  file = "ingamefr.mvs"; break;   //  French
case 4:  file = "ingamegr.mvs";          //  German
}
seg = luaL_checklstring(L, 2);
StartInGameMoviePlay(file, seg);
```

So `ScriptCB_PlayInGameMovie("anything.mvs", "seg")` has always opened
`ingame.mvs`. The first argument is dead on modtools, Steam and GOG alike.

| | modtools | Steam | GOG |
|---|---|---|---|
| `ScriptCB_PlayInGameMovie` | `0x004653E0` | `0x00585790` | `0x00586520` |
| registration table entry | `0x00AC7830` | `0x007E7058` | `0x007E8058` |
| language jump table | `0x00465490` | `0x00585834` | `0x005865C4` |
| `"ingame.mvs"` | `0x00A2EE58` | `0x007A5A54` | `0x007A68D4` |

Modtools additionally guards the whole body behind a leading `if (f()) return;`
at `0x007ED650`, which the retail builds dropped. Phantom has the same guard in
the same position and names it `RedMissionLog::Enabled()`.

## Playback chain

```
ScriptCB_PlayInGameMovie(file, segment)
  state != 0 -> PblHash(segment) pushed onto g_InGameMovieQueue, return
  state == 0 -> StartInGameMoviePlay(file, segment)
                  sFilename = "Movies\" + file        // fixed 256-byte global
                  sIdName   = segment                 // fixed 256-byte global
                  state = 1, frame = 0
UpdatInGameMovie()      // once per frame
  state 1, frame 0 -> GameMovie::Create + GameMovie::Open(sFilename, sIdName, ...)
                      state = 2
  state 2, frame 4 -> GameMovie::Play(sIdName, "", 0, 0, w, h, false)
                      drain g_InGameMovieQueue through RedMovie::Properties::FindByHashID
                      state = 3
  state 3          -> GameMovie::Update / IsPlaying; state 0 when finished
```

`StartInGameMoviePlay` is `__cdecl(file, segment)` on modtools and LTCG
`ECX = file, EDX = segment` on the retail builds.

| | modtools | Steam | GOG |
|---|---|---|---|
| `StartInGameMoviePlay` | `0x0044AF40` | `0x00534BD0` | `0x00535940` |
| `UpdatInGameMovie` | `0x0044B2A0` | `0x00534D40` | `0x00535AB0` |
| `GameMovie::Open` | `0x0044A620` | `0x00534590` | `0x00535300` |
| `sFilename[256]` | `0x00B30290` | `0x01E56288` | `0x01E57738` |
| `sIdName[256]` | `0x00B30390` | `0x01E56170` | `0x01E57630` |
| player state | `0x00B30280` | `0x01E5616C` | `0x01E5761C` |
| player frame | `0x00B30284` | `0x01E56270` | `0x01E57620` |
| `sUseMovies` | `0x00AC68F4` | `0x007E666D` | `0x007E766D` |

Both fixed buffers are 256 bytes on all three builds - confirmed by the `0x100`
stride on Phantom and modtools, and by the next referenced global sitting exactly
`0x100` above `sFilename` on Steam (`0x01E56388`) and GOG (`0x01E57838`).
`sUseMovies` is only ever written by `ParseCommandLine`.

## Path resolution

`GameMovie::Open` already understands a `dc:` prefix on every build:

```c
useDLC = (_strnicmp(name, "dc:", 3) == 0);
if (useDLC) name += 3;
LoadUtil::MakeFullName(name, FILE_TYPE_NONE, out, 0x80, useDLC);
```

`MakeFullName` (see `readdatafile_path_resolution`) with `FILE_TYPE_NONE` builds

```
useDLC ? "<GetContentDirectory()>\Data\" : "data\"     +  "_lvl_pc\"  +  name
```

so an in-game movie ends up at `data\_lvl_pc\Movies\<file>`.

The prefix can never fire on the in-game path, though, because
`StartInGameMoviePlay` pastes the literal `"Movies\"` in front of the name first
- `"Movies\dc:foo.mvs"` puts the prefix at offset 7, where nothing looks for it.
`FILE_TYPE_MOVIE` is a different thing entirely: it appends `.bik` and is used
for full-motion video, not for `.mvs` containers.

Note `MakeFullName`'s output is capped at `0x80` here, so the whole resolved
path - including the absolute addon directory `GetContentDirectory` returns on a
real install - has to fit in 128 characters. That cap is the engine's, and the
shell's own `dc:` movies have always lived with it.

**The `dc:` form does not fit on a default Steam install.** `GetContentDirectory`
returns an absolute path, so the install directory eats most of the budget:

```
130  C:\Program Files (x86)\Steam\steamapps\common\Star Wars Battlefront II Classic\GameData\AddOn\mymod\Data\_lvl_pc\Movies\ingame.mvs
 61  data\_lvl_pc\..\..\AddOn\mymod\Data\_lvl_pc\Movies\ingame.mvs
```

Past 127 characters `MakeFullName` truncates in place and the open fails. The
second form is the same file reached by climbing out of `data\_lvl_pc\` instead,
the trick `SetLoadDisplayLevel` already uses, and it takes the cap out of play.
This is why BF2GameExt emits the relative climb rather than `dc:`.

## What "Unable to open movie file" actually means

`GameMovie::Open` (Phantom `0x005d2540`) logs that warning at severity 2 when
`RedMovie::Open` returns false. Two details matter when reading it:

**It prints the unresolved name.** The two `%s` arguments pushed at `0x005d26f2`
are `ESI` and `EDI`, which hold `param_1` and `param_2` as they arrived - not the
`MakeFullName` output. So `Unable to open movie file dc:Movies\ingame.mvs:seg`
says nothing about which directory was searched, and a truncated path looks
identical to a missing one.

**The segment is not validated there.** The chain is

```
RedMovie::Open          0x00413a75 -> 0x008dd130
RedMoviePlayer::Open    0x004160e0 -> 0x008db9f0
RedMoviePlayerBase::Open           0x00407081
```

and only two things in it can produce the warning:

| Failure | Cause |
|---|---|
| `PblFile::Open(name)` false | file is not at the resolved path |
| `OpenHeader` false | no `Info` chunk parsed, i.e. not a munged `.mvs` |

`RedMoviePlayerBase::OpenHeader` (`0x0092c000`) takes the segment hash as
`param_3` and **never reads it** - it walks the chunk tree for `Info` (`0x0fb40705`),
fixes up segment start positions, then returns true if `mInfo` is non-null after
selecting segment 0. A wrong segment name therefore cannot cause this message; it
fails later and silently, in `GameMovie::Play` via
`RedMovie::Properties::FindByHashID`.

`LoadUtil::MakeFullName` has no null guard on the DLC branch either - with no
addon mounted it formats `"%s\\Data\\"` from a NULL `GetContentDirectory()`,
yielding `(null)\Data\_lvl_pc\Movies\...` and the same warning rather than a
crash.

## Movie properties are not a gate

A `.mvs` is played by *segment* name, looked up through
`RedMovie::Properties::FindByHashID`. Those `Properties` come from the `mcfg`
chunk of a loaded lvl - `GameMovieOpenConfig` pulls one out of
`Shell\eng\shell.lvl`, and a mission's own `<name>_movies.mcfg` comes in through
its `.req`, exactly as the stock missions pull in `ingame_movies`.

The `Movie("ingame")` line inside a `MovieProperties` block sets
`Properties::mMovieID`, and `GetMovieID` has exactly one caller -
`Properties::Read`, propagating it through `Inherit()`. Nothing else in the
executable ever reads the field, so it does **not** have to match the filename
and does not gate playback.

## The fix

`PatcherDLL/src/shell/ingame_movie_path.cpp` detours the Lua callback, snapshots
both arguments, lets the original run, and then rewrites `sFilename` in place -
`"Movies\<name>"` for the base game, or
`"..\..\<addon>\Data\_lvl_pc\Movies\<name>"` when the movie belongs to the addon.
Nothing reads that buffer until `UpdatInGameMovie` ticks on a later frame, so the
edit always lands before `GameMovie::Open` sees it.

`"dc:Movies\<name>"` is emitted only as a fallback, for an addon directory that
is not under the working directory and so cannot be named by climbing. A real
install never has one; the path-length arithmetic above is why the climb is
preferred.

Because the engine's own warning identifies neither the directory it searched nor
which failure it hit, the hook resolves the path itself and logs the result: the
full on-disk name on success, and on failure a finished sentence naming the cause
- file absent, no addon mounted, or a resolved path over the 127-character limit
with the measured length. It never refuses to rewrite, so an unresolvable request
still reaches `GameMovie::Open` and the script's intent stays visible in the log.

Rewriting the global was chosen over reimplementing the callback because the
callback also stops all sound, stops rumble, and pushes onto the segment queue -
four more per-build addresses for no gain - and over hooking
`StartInGameMoviePlay`, which is a register-convention LTCG leaf on the retail
builds.

The three stock filenames are passed through untouched so the French and German
substitutions keep working.
