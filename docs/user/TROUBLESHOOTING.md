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

## Nothing happens, and there is no BF2GameExt.log

In rough order of likelihood:

- **The files are in the wrong folder.** They go next to the game executable,
  which means inside `GameData`, not the folder above it that contains
  `GameData`. This trips up nearly everyone.
- **`[General] Enabled=0`** in your INI.
- **You are on the Aspyr Classic Collection.** It is a different executable and
  is not supported. Only the Modtools, Steam and GOG builds work.
- **Your executable has already been patched by something else.** See below.
- **An overlay or launcher is loading its own `dinput8.dll`** ahead of ours.
- **`dinput8.dll` is not in the folder any more.** Rare, but a scanner can
  remove it. See [Antivirus flagged dinput8.dll](#antivirus-flagged-dinput8dll).

## Your executable has already been patched

**BF2GameExt needs the clean, unmodified game executable.**

It patches the exe in memory at launch and expects the original bytes to be
there. If another patcher has already rewritten them on disk, those patches do
not apply. The usual result is that the extension loads and writes its log, but
half the features do nothing.

`BF2GameExt.log` is how you confirm it. Lines reading

```
Skipping patch set (site mismatch @ ..., expected ...)
```

mean the bytes at that address were not what the game shipped with. One or two
can just be a feature that is not mapped for your build; a long run of them
means the executable itself is not stock.

The one that comes up in practice is **MemExt**, the earlier project this one
grew out of. An exe patched with MemExt is not a valid base for BF2GameExt.
The same goes for no-CD executables, older hex-edited copies, and anything else
that shipped as "a patched exe" rather than as files you drop in next to it.

To fix it, put the original executable back:

- **Steam** - right-click the game, Properties, Installed Files, Verify
  integrity of game files.
- **GOG** - reinstall, or restore the exe from your installer.
- **Modtools** - copy `BF2_modtools.exe` back from a fresh mod tools install.

Then install BF2GameExt again. Nothing it ships modifies the executable on
disk, so once the original is back you can leave it alone permanently.

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

## Particle density is costing frame rate

`[Particles] ParticleDensity=2` switches off the engine's distance thinning
completely, so a distant emitter spawns as many particles as one at your feet.
On a busy map that costs frame time, and it is the first thing to suspect if the
game got slower after you changed this key.

Level 1 is the middle setting and exists for exactly that reason. It gives full
density near and mid-range but leaves the far field thinned, which is where the
cost concentrates. If 2 drops your frame rate, try 1 before going back to 0.

`ParticleFixes` is a separate key and is not a density control. It repairs how
the engine batches and draws particles; `ParticleDensity` is the one that
decides how many there are.

## VoiceLimit did nothing

`[LimitIncreases] VoiceLimit` is 0 by default, which keeps the stock 32; a
count from 33 to 119 raises it. The key is Modtools only, and on Steam and GOG
the installer no-ops and the key is ignored.

Both mixing paths are raised, so on Modtools the count should climb either way.
The two paths get there differently, and only one of them needs anything from
your sound card:

- **EAX**, meaning 5.1 or 7.1 or another audio mode that picks DirectSound
  hardware. The extra voices are hardware buffers, so DirectSound has to have
  some to give out. Native Windows Vista and later report zero hardware 3D
  buffers, which is why this path in practice wants a wrapper such as DSOAL,
  Creative ALchemy or IndirectSound.
- **Software mixing**, which needs nothing external. The engine's own mixer
  ships with 32 inputs and is widened to match the voice count.

If the software path could not be widened - any one of its sites failing its byte
check - it is left at 32 on purpose, because a voice that cannot get a mixer
input takes a pool slot and then produces no sound at all, which is worse than
stock. The log says so when that happens.

The engine's own `BFront2.log` tells you whether the patch itself applied: on
success it carries a line tagged `[VoiceLimit]` naming the count. Every site is
verified against its expected bytes before anything is written, and a single
mismatch switches the whole feature off and logs which site failed, so it cannot
half-apply. That line only means the patch went in. For the ceiling actually in
force at runtime, and for how many sounds are being dropped for want of a voice,
set `[Diagnostic] SoundDiagnostic=1`.

## Branch regions do not resolve

Check the names first. The region has to be called `entitypathbranch <id>`, and
the path node has to ask for the same `<id>`:

```
region:     entitypathbranch dropzone1
path node:  BranchRegion("dropzone1")
```

The old code took the id from the separating space onward, so a region named
`entitypathbranch dropzone1` registered itself as ` dropzone1`. That never
resolved in any shipping build, so a path node written to match it -
`BranchRegion(" dropzone1")` - did not work before the fix either, and is
deliberately not supported now. Take the space out if you have one.

Then confirm `[Fixes] BranchRegionFix` has not been set to 0; it is on by
default. If the names are right and the fix is on, set
`[Diagnostic] BranchRegionDebug=1`. It writes every step of resolution to
`BF2GameExt.log` tagged `[BranchDbg]` - which factories are registered, what
each region registered itself as, and what each lookup asked for. The debug key
is Modtools only.

On Steam and GOG the engine's missing-region warning was stripped out, so a
branch region that never resolves is completely silent there and getting no
warning tells you nothing. The fix is applied on all three builds, but it has
only been verified in play on Modtools.

## Gamepad problems

- The pad needs `[Controller] Enabled=1`.
- Bindings are per mode and do not inherit. Rebinding jump under
  `[Controller.Unit]` does not change it for `[Controller.Hero]`.
- In the shipped INI every default is commented out with a leading `;`.
  Uncomment a line to override it.
- Aim assist is off by default. It needs `[AimAssist] Enabled=1` of its own,
  separately from `[Controller] Enabled=1`, and it is singleplayer only and
  controller only.

Full reference: [CONTROLLER.md](CONTROLLER.md).

## Antivirus flagged dinput8.dll

This has not come up in practice, but it is possible, so it is worth knowing
about. A proxy DLL takes the name of a system library, forwards the real exports
onward, and loads another DLL into the process, which is a shape some scanners
flag generically. If yours does, the detection will be a generic heuristic name
rather than a named malware family, and the symptom is "nothing happens" with
`dinput8.dll` gone from `GameData`.

If it happens, restore the file and add an exclusion for your `GameData` folder.
To satisfy yourself first, check the zip's hash against the SHA-256 on the
release page and download only from the sources under
[Where to get it](../../README.md#where-to-get-it). The source tree is public
too, so you can read or rebuild it yourself, though a rebuild will not
reproduce the published hash byte for byte because the compiler and the zip both
stamp timestamps into their output.

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
