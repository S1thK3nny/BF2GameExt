# Build reference: 2006 Steam retail (`PC Final LTCG`)

The original 2006 Steam release of SWBF2, recovered from Steam2 depot `6061_1`. A fourth build
lineage alongside the two modtools builds and the 2017 Steam/GOG recompiles.

Source: `E:\Steam2Browser\steam2info\extracted\6061_1`
DRM analysis and unpacking: [SteamStubDRM.md](SteamStubDRM.md)

## Identity

| field | value |
| --- | --- |
| file | `GameData\BattlefrontII.exe` |
| md5 | `c1fca4cf1dcc3fab753a1bc5d2fe7803` |
| size | 4,960,256 bytes |
| PE timestamp | 2006-01-31 19:25:56 |
| linker | 7.10 (MSVC 2003) |
| build config | `e:\Battlefront2\main\Battlefront2\Build\PC Final LTCG\` |
| imagebase | 0x400000 |
| `.text` | va 0x1000, vs 0x38B0AB |
| entry point (unpacked) | 0x00352049 |
| patch level | v1.1 |

`testapp.exe` in the same folder is a byte-identical copy - same md5. Not a separate binary.

Version is v1.1: retail shipped 2005-11-01 so a 2006-01-31 build is post-release, and the depot's
install script records `Revision 10101` next to `Update1_1.txt`.

> `Version     : 1.00` in the dedicated-server banner is a dead literal present in **every** build,
> Steam and GOG included. Never identify a build from it.

## Build lineage comparison

| build | config | timestamp | linker | RTTI |
| --- | --- | --- | --- | --- |
| **this one** | `PC Final LTCG` | 2006-01-31 | 7.10 | no |
| `BF2_modtools.exe` | `PC Release` | 2006-02-07 | 7.10 | no |
| `BattlefrontII.Debug.FullScreen.1080.exe` | `PC Modtools Release` | 2006-02-09 | 7.10 | no |
| Steam 2017 | stripped | 2017-10-23 | 12.0 | yes (864) |
| GOG 2017 | stripped | 2017-10-23 | 12.0 | yes (882) |

No 2006-era build carries RTTI; only the 2017 recompiles enabled it.

Lua surface is effectively frozen: 576 `ScriptCB_` names here vs 577 in Steam 2017, the sole
difference being Steam's `ScriptCB_LastSignInError`.

## Official system requirements

From `Install\docs\english\readme.txt` as shipped.

| | Required | Recommended |
| --- | --- | --- |
| OS | Windows 2000 or XP | - |
| Computer | 100% DirectX 9.0c compatible | - |
| CPU | Pentium 4 1.5 GHz / Athlon XP 1500+ | Pentium 4 2.8 GHz / Athlon XP 2800+ |
| Memory | 256 MB RAM | 512 MB RAM |
| Graphics | 64 MB 3D card with hardware T&L | 128 MB 3D card with hardware VS/PS |
| Sound | 100% DirectX 9.0c compatible audio device | - |
| CD-ROM | 8x | 16x CD-ROM or DVD-ROM |
| Input | Keyboard and mouse | joystick or gamepad supported |
| Disk | 3.77 GB, plus ~500 MB headroom | - |

Multiplayer: 2 to 64 players, DSL/cable or faster. Dedicated server wants a 2.4 GHz+ CPU.

The launcher's own hardware gate (`Install\Launcher.xml`, `<HardwareEx>`) enforces:

```xml
<Display rasterizer="true" tnl="true" pixelshader="false" />
<Sound hardware="true" halt="false" />
<DirectX major="9" minor="0" letter="c" />
<OS platform="2" major="5" minor="0" />
```

`platform="2"` is `VER_PLATFORM_WIN32_NT` and `5.0` is Windows 2000, so NT-family 5.0 or newer.
Note hardware T&L is required but pixel shaders are not.

## Runtime requirements (verified against this binary)

**Imports** - `d3d9.dll`, `WS2_32.dll`, `KERNEL32.dll`, `USER32.dll`, `GDI32.dll`, `ADVAPI32.dll`,
`WSOCK32.dll`, `binkw32.dll`, `DSOUND.dll`, `DINPUT8.dll`, `WINMM.dll`, `ole32.dll`.
`unicows.dll` is referenced by name and loaded dynamically.

**Files that must sit beside the exe** - `BINKW32.DLL`, `EAX.DLL`, `UNICOWS.DLL`. All three ship
in the depot's `GameData`.

**Data** - `Data\_LVL_PC\` relative to the exe. 191 files, 3.8 GB; whole depot is 3.9 GB.
Reads `Data\_LVL_PC\device.def` (shipped) and `Data\_LVL_PC\vidmode.ini` (generated at runtime,
not shipped).

**Registry** - reads `SOFTWARE\LucasArts\Star Wars Battlefront II\1.0\`. As a 32-bit process on
64-bit Windows this resolves under `HKLM\SOFTWARE\WOW6432Node\`. Values written by the depot's
install script: `CD Key`, `ExePath`, `Launcher` (strings), `Installed`=1, `Revision`=10101 (dwords).
The key is **not** created by a Steam 2017 install, so it must be added by hand.

**DRM** - the shipped exe is SteamStub-wrapped and will not start without the Steam client holding
appid 6060. Unpack it (see [SteamStubDRM.md](SteamStubDRM.md)) to get a standalone binary.

## Running it today

1. Copy the depot's `GameData` to a writable location - the game writes profiles and config
   next to itself.
2. Unpack: `Steamless.CLI.exe --verbose BattlefrontII.exe`, then swap the `.unpacked.exe`
   into place. Delete `testapp.exe`; it is the same bytes.
3. Create the registry key above (elevated; `HKLM\SOFTWARE\WOW6432Node\LucasArts\...`).
4. Run `BattlefrontII.exe`.

Caveats: it is a 2006 D3D9 title, so a modern GPU may need compatibility mode or a wrapper such as
dgVoodoo; GameSpy is long dead so multiplayer is direct-IP/LAN only; and **BF2GameExt does not work
on this build** - every address in the project is derived from the other builds and this is a
different compile.

## Shipped data

Of 191 `_LVL_PC` files, 183 match the 2017 Steam data in size. 41 of those differ only in
uninitialised munger padding - stale heap pointers and leaked path fragments, e.g. 44 differing
bytes in a 60 MB `cor1.lvl`. Same assets, separate munge runs.

Eight files differ in size and were **not** verified against a clean reference (the local Steam copy
carries mods). `yav/yav1.lvl` is known to have been updated in a later depot: it diverges at byte 4
(the ucfb size field) and the 2017 copy is a strict superset - 143 tokens unique to it, zero unique
to the depot. So this depot holds the **pre-update** `yav1.lvl`.

The depot ships English only: no localised `.mvs` movies, no `sound\de` or `sound\fr`.
