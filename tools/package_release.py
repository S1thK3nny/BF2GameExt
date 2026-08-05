#!/usr/bin/env python3
"""Build and package a BF2GameExt release zip.

Produces  dist/BF2GameExt-<version>.zip  laid out so a user can drag the
GameData folder straight onto their install:

    README.txt
    GameData/
        dinput8.dll
        BF2GameExt.dll
        BF2GameExt.ini
        data/
            _lvl_pc/
                prone.lvl

The version comes from version.h and nowhere else. Before zipping, the script
checks the version resource compiled into each binary against version.h and
refuses to package on a mismatch, which is what catches "I forgot to rebuild".

Usage:
    python tools/package_release.py              # clean build, then package
    python tools/package_release.py --no-build   # package what is in bin/Release
"""

import argparse
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
VERSION_H = REPO / "version.h"
SLN = REPO / "BF2GameExt.sln"
BUILD_DIR = REPO / "bin" / "Release"
DIST = REPO / "dist"
STAGING = REPO / "build" / "package"

# Built binaries: (name in bin/Release, destination inside the zip).
# BF2GameExt.exe (the exe patcher) is still built by the solution but is no
# longer shipped, so it is not listed and not version-checked.
PAYLOAD = [
    ("dinput8.dll", "GameData/dinput8.dll"),
    ("BF2GameExt.dll", "GameData/BF2GameExt.dll"),
]

# Files copied straight from the source tree.
EXTRA = [
    (DIST / "BF2GameExt.ini", "GameData/BF2GameExt.ini"),
    # Prone needs its animation bank present or the feature cannot be enabled,
    # so the munged lvl ships in the addon data path the game already scans.
    (REPO / "GameAssets" / "Prone" / "prone.lvl",
     "GameData/data/_lvl_pc/prone.lvl"),
    # MIT requires the notice to travel with the binaries, so it ships at the
    # zip root beside README.txt rather than inside GameData. .txt so it opens
    # on a double click on Windows.
    (REPO / "LICENSE", "LICENSE.txt"),
]

README_TXT = """\
BF2GameExt {version}
====================

A DLL extension for Star Wars Battlefront II (2005) that adds custom Lua
functions, ODF properties, loading screen parameters, engine bug fixes and
limit increases.

Works with the Modtools, Steam and GOG builds. It does NOT work with the
Aspyr Classic Collection, which is a different executable.


INSTALL
-------

Drag the GameData folder in this zip onto your game install folder and let it
merge. That puts these files in place:

    GameData/dinput8.dll              the proxy, loaded by the game
    GameData/BF2GameExt.dll           the extension
    GameData/BF2GameExt.ini           settings
    GameData/data/_lvl_pc/prone.lvl   animation assets for the Prone feature

That is the whole install. Launch the game normally.

Typical locations:
    Steam     ...\\steamapps\\common\\Star Wars Battlefront II\\GameData
    GOG       ...\\Star Wars Battlefront II\\GameData
    Modtools  wherever BF2_modtools.exe lives

Say yes if Windows asks to merge the data folder. It already exists in your
install and nothing in it is overwritten; prone.lvl is a new file.


VERIFY IT LOADED
----------------

Right-click GameData\\BF2GameExt.dll, Properties, Details. File version should
read {version}.

Or check that GameData\\BF2GameExt.log exists and its timestamp is from your
last launch. That file is rewritten every time the game starts, so a missing
file or an old timestamp means the extension never ran.


CONFIGURE
---------

Edit GameData\\BF2GameExt.ini. Every key is documented at
docs/user/CONFIGURATION.md in the repository, gamepad bindings at
docs/user/CONTROLLER.md.


UNINSTALL
---------

You probably do not need to. To switch it off completely, set

    Enabled=0

in GameData\\BF2GameExt.ini. The game then runs fully stock with the files still
in place, and you can flip it back whenever you want. That is the easiest way
to rule BF2GameExt out when something else is misbehaving.

To remove it properly, delete:

    GameData\\dinput8.dll
    GameData\\BF2GameExt.dll
    GameData\\BF2GameExt.ini
    GameData\\data\\_lvl_pc\\prone.lvl     (optional)

prone.lvl is inert by itself. Nothing loads it unless BF2GameExt is running, so
leaving it in the data folder does nothing at all if you would rather not go
poking around in there.

Either way nothing is written to the registry and no game files are modified,
so the game reverts exactly to stock.


TROUBLE
-------

See docs/user/TROUBLESHOOTING.md. If your antivirus quarantines dinput8.dll,
that is a false positive on the proxy-DLL pattern and the file is explained
there.

Report issues at https://github.com/S1thK3nny/BF2GameExt/issues
"""


def parse_version() -> str:
    text = VERSION_H.read_text(encoding="ascii")
    parts = []
    for field in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(rf"#define\s+GAMEEXT_VERSION_{field}\s+(\d+)", text)
        if not m:
            sys.exit(f"ERROR: GAMEEXT_VERSION_{field} missing from {VERSION_H}")
        parts.append(m.group(1))
    return ".".join(parts)


def find_msbuild() -> str:
    """Locate MSBuild.exe via vswhere, the way VS itself does."""
    vswhere = Path(
        r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    )
    if vswhere.exists():
        out = subprocess.run(
            [str(vswhere), "-latest", "-requires",
             "Microsoft.Component.MSBuild", "-find",
             r"MSBuild\**\Bin\MSBuild.exe"],
            capture_output=True, text=True,
        ).stdout.strip().splitlines()
        if out:
            return out[0]
    if shutil.which("msbuild"):
        return "msbuild"
    sys.exit("ERROR: MSBuild not found. Run this from a VS Developer Prompt.")


def build():
    msbuild = find_msbuild()
    print(f"MSBuild: {msbuild}")

    # A clean build is the point. Incremental output is exactly how a stale
    # or Debug-linked binary sneaks into a release.
    if BUILD_DIR.exists():
        print(f"Cleaning {BUILD_DIR}")
        shutil.rmtree(BUILD_DIR)

    cmd = [msbuild, str(SLN), "-p:Configuration=Release",
           "-p:Platform=x86", "-t:Rebuild", "-m", "-v:minimal", "-nologo"]
    print("Building Release|x86 ...")
    if subprocess.run(cmd, cwd=REPO).returncode != 0:
        sys.exit("ERROR: build failed")


def file_version(path: Path):
    """Read the PE version resource. Returns 'a.b.c' or None."""
    try:
        import ctypes
        from ctypes import wintypes
    except ImportError:
        return None

    ver = ctypes.WinDLL("version")
    size = ver.GetFileVersionInfoSizeW(str(path), None)
    if not size:
        return None
    buf = ctypes.create_string_buffer(size)
    if not ver.GetFileVersionInfoW(str(path), 0, size, buf):
        return None
    block = ctypes.c_void_p()
    length = wintypes.UINT()
    if not ver.VerQueryValueW(buf, "\\", ctypes.byref(block), ctypes.byref(length)):
        return None
    # VS_FIXEDFILEINFO: dwFileVersionMS at +8, dwFileVersionLS at +12
    raw = ctypes.string_at(block, length.value)
    ms = int.from_bytes(raw[8:12], "little")
    ls = int.from_bytes(raw[12:16], "little")
    return f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}"


def verify(version: str):
    """Refuse to ship binaries whose stamped version is not version.h."""
    problems = []
    for src, _ in PAYLOAD:
        path = BUILD_DIR / src
        if not path.exists():
            problems.append(f"{src}: missing from {BUILD_DIR}")
            continue
        got = file_version(path)
        if got is None:
            print(f"  {src}: version resource unreadable, skipping check")
        elif got != version:
            problems.append(f"{src}: stamped {got}, expected {version}")
        else:
            print(f"  {src}: {got} OK")
    if problems:
        for p in problems:
            print(f"ERROR: {p}")
        sys.exit("Refusing to package. Rebuild, or check version.h.")


def package(version: str) -> Path:
    if STAGING.exists():
        shutil.rmtree(STAGING)
    STAGING.mkdir(parents=True)

    for src, dest in PAYLOAD:
        target = STAGING / dest
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(BUILD_DIR / src, target)

    for src, dest in EXTRA:
        if not src.exists():
            hint = (" Run generate_ini.py first."
                    if src.suffix == ".ini" else "")
            sys.exit(f"ERROR: {src} missing.{hint}")
        target = STAGING / dest
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, target)

    (STAGING / "README.txt").write_text(
        README_TXT.format(version=version), encoding="utf-8", newline="\r\n"
    )

    DIST.mkdir(parents=True, exist_ok=True)
    zip_path = DIST / f"BF2GameExt-{version}.zip"
    if zip_path.exists():
        zip_path.unlink()

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in sorted(STAGING.rglob("*")):
            if f.is_file():
                z.write(f, f.relative_to(STAGING).as_posix())
    return zip_path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true",
                    help="package whatever is already in bin/Release")
    args = ap.parse_args()

    version = parse_version()
    print(f"BF2GameExt {version}\n")

    if not args.no_build:
        build()
    else:
        print("Skipping build (--no-build)")

    print("\nVerifying stamped versions:")
    verify(version)

    zip_path = package(version)
    size = zip_path.stat().st_size
    print(f"\nWrote {zip_path}  ({size / 1024:.0f} KB)")
    with zipfile.ZipFile(zip_path) as z:
        for n in sorted(z.namelist()):
            print(f"    {n}")


if __name__ == "__main__":
    main()
