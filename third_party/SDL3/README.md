# SDL3

`SDL3.dll` is the unmodified 32-bit (x86) build from the official SDL 3.4.18 release:
https://github.com/libsdl-org/SDL/releases/tag/release-3.4.18 (`SDL3-3.4.18-win32-x86.zip`, the same file as in `SDL3-devel-3.4.18-VC.zip` under `lib/x86`).

SHA-256: `c840a5106e7690379a6d328698d1531f42f1707e7f5baade32c64ddcef3b5e10`

BF2GameExt loads it at runtime for gamepad input (`PatcherDLL/src/controller/gamepad_sdl.cpp`), so no SDL headers or import library are needed to build. It ships next to `BF2GameExt.dll`. SDL is under the zlib license in `LICENSE.txt`, which is compatible with BF2GameExt's MIT license.

To update: drop in the x86 `SDL3.dll` from a newer release and update the version and hash above. SDL 3 keeps its ABI stable within the major version.
