# Troubleshooting

Start here before opening an issue. If you end up filing one anyway, the
[bug report form](https://github.com/S1thK3nny/BF2GameExt/issues/new?template=bug_report.yml)
asks for the files described below.

## Where everything lives

All of these sit next to the game executable, in `GameData`:

| File | What it is |
|------|-----------|
| `dinput8.dll` | The proxy. This is what the game loads first. |
| `BF2GameExt.dll` | The extension itself. |
| `BF2GameExt.ini` | Your settings. Optional; without it every key uses its built-in default. |
| `BF2GameExt.log` | Which patches applied. **Rewritten from scratch every launch.** |
| `BF2GameExt_crash.log` | Crash reports. **Appended forever**, so the newest is at the bottom. |

## Is it even loaded?

Cheapest first:

1. **Is `BF2GameExt.log` there, and is its timestamp from your last launch?**
   It is recreated on every start, so a missing file or an old timestamp means
   the DLL never ran.
2. **Right-click `BF2GameExt.dll`, Properties, Details.** The file version
   should match the release you installed.

If you want to see it working in game rather than on disk, any of these are
stock behaviour that BF2GameExt changes:

- **Get into a hover or a walker and press the change view button.** Stock, it
  does nothing at all on those two classes. With the extension it toggles first
  and third person.
- **Kill a droideka.** Stock, the death animation is cut off after one frame.
  With the extension it plays through.
- **Plug in a gamepad and fire.** Stock has no rumble at all. With the
  extension the pad vibrates on weapon fire and damage, assuming
  `[Controller] Enabled=1` and `Rumble=1`.

If you write mission scripts, `print(GameExt.version)` returns the version
string and a nil `GameExt` means the Lua half never initialised.

## Antivirus quarantined dinput8.dll

Expect this one. Proxy DLLs get flagged constantly.

A proxy takes the name of a system library, forwards the real exports onward,
and loads another DLL into the process. DLL-hijacking malware does exactly
that, and ours is unsigned on top of it, so scanners match the shape rather
than any specific content. The giveaway is that the detection is a generic
heuristic name rather than a named malware family.

What to do:

- Restore the file from quarantine and add an exclusion for your `GameData`
  folder.
- If you would rather verify first, check the zip's hash against the release
  page, and note that the whole source tree is public and builds to the same
  thing.
- The `ExePatcher` install method gets flagged for a different reason (it
  writes to a copy of the game executable) and is not a safer alternative.

If your scanner deleted the file silently, the symptom is "nothing happens" and
`dinput8.dll` is simply gone. Check the folder before assuming a bug.

## Nothing happens, and there is no BF2GameExt.log

In rough order of likelihood:

- **The files are in the wrong folder.** They go next to the game executable,
  which means inside `GameData`, not the folder above it that contains
  `GameData`. This trips up nearly everyone.
- **Antivirus ate `dinput8.dll`.** See above.
- **`[General] Enabled=0`** in your INI.
- **You are on the Aspyr Classic Collection.** It is a different executable and
  is not supported. Only the Modtools, Steam and GOG builds work.
- **An overlay or launcher is loading its own `dinput8.dll`** ahead of ours.

## ReShade or another dinput8 proxy

Only one file in a folder can be called `dinput8.dll`, so two proxies cannot
both use the name. BF2GameExt handles this by chain-loading:

> On startup it scans its own folder for `dinput8_*.dll`. If it finds one, it
> forwards DirectInput through that instead of the system copy. Otherwise it
> falls back to `system32\dinput8.dll`.

So to run both, **rename the other proxy**:

```
GameData/
  dinput8.dll            <- BF2GameExt's proxy
  dinput8_reshade.dll    <- ReShade, renamed
  BF2GameExt.dll
```

Two caveats:

- The scan takes the **first** `dinput8_*.dll` it finds. Chaining three proxies
  this way is not supported; only one will be picked up.
- Load order matters for overlays. If ReShade misbehaves when renamed, try it
  the other way round, letting ReShade own `dinput8.dll` and loading
  `BF2GameExt.dll` through the exe patcher method instead.

## The game crashes

`BF2GameExt_crash.log` is written automatically whenever the game faults. There
is no setting to turn it on, and it needs no debugger. It records the faulting
address as `module+offset` plus an unrelocated address, which is the form that
maps onto the disassembly, along with registers and a stack scan.

When reporting a crash:

- Attach the **whole report**, not just the first line. The stack scan below
  the registers is often the only thing that identifies the culprit.
- The file is appended to across sessions, so **take the last report**, and say
  roughly when the crash happened if the file has several.
- Only the first 16 faults per session are logged, so a crash loop will not
  produce an endless file.

Before filing, try reproducing with a default `BF2GameExt.ini`. If it stops,
bisect your settings and tell us which key did it. That turns a hard bug into
an easy one.

## A specific feature does nothing

- **Check the build.** Not everything works everywhere.
  [FEATURES.md](FEATURES.md) marks each entry, and the compatibility table in
  the [README](../../README.md) summarises it. The grappling hook is Modtools
  only.
- **Check the toggle.** Every optional feature has an INI key, listed in
  [CONFIGURATION.md](CONFIGURATION.md).
- **Prone needs its asset.** It requires `GameData/data/_lvl_pc/prone.lvl`,
  which ships in the release zip. Prone is on by default and self-disables on
  any mission where that file is missing, so if it silently does nothing, check
  that the file survived the install.
- **A mod may have opted out.** Any map script can set `GameExt.disable = true`
  in its `ScriptPreInit` to suppress the intrusive features. If a feature works
  on stock maps but not on one particular mod, that is why, and it is
  deliberate on the mod's part.
- **Read `BF2GameExt.log`.** It lists which patches applied and which were
  skipped, which usually answers this immediately.

## Gamepad problems

- The pad needs `[Controller] Enabled=1`.
- Bindings are per mode and do not inherit. Rebinding jump under
  `[Controller.Unit]` does not change it for `[Controller.Hero]`.
- In the shipped INI every default is commented out with a leading `;`.
  Uncomment a line to override it.
- Aim assist is singleplayer only, and controller only.

Full reference: [CONTROLLER.md](CONTROLLER.md).

## Uninstalling

Setting `Enabled=0` under `[General]` in `BF2GameExt.ini` switches everything off and the game runs fully stock with the
files still in place. That is also the fastest way to rule BF2GameExt out when
you are not sure whether it is the cause of a problem, and it is reversible in
one edit.

To remove it properly, delete these from `GameData`:

| File | |
|------|--|
| `dinput8.dll` | |
| `BF2GameExt.dll` | |
| `BF2GameExt.ini` | |
| `data/_lvl_pc/prone.lvl` | optional, see below |
| `BF2GameExt.log`, `BF2GameExt_crash.log` | only if they exist |

`prone.lvl` is inert on its own. Nothing loads it unless BF2GameExt is running,
so it is harmless to leave in the data folder if you would rather not touch it.

Nothing is written to the registry and no game files are modified, so the game
reverts exactly to stock either way.

If you used the exe patcher instead, delete the patched executable copy and go
back to launching the original.
