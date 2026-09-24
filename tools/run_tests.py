#!/usr/bin/env python3
"""Compile and run every tests/*.cpp.

Each test is a standalone program with its own main() that asserts and exits
non-zero on failure. They cover the pure-math headers the DLL shares with them
(HUD horizon, target-bar geometry and fade) and need no game.

Each file is compiled with the 32-bit MSVC toolchain, the same one the DLL
uses, found through vswhere the way package_release.py finds MSBuild. NDEBUG
is left undefined so assert() stays live. Output goes to build/tests/.

Usage:
    python tools/run_tests.py
"""
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TESTS = REPO / "tests"
OUT = REPO / "build" / "tests"


def find_vcvars() -> Path:
    """Locate vcvars32.bat of the latest VS install that has the C++ tools."""
    vswhere = Path(
        r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    )
    if vswhere.exists():
        out = subprocess.run(
            [str(vswhere), "-latest", "-products", "*", "-requires",
             "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationPath"],
            capture_output=True, text=True,
        ).stdout.strip().splitlines()
        if out:
            vcvars = Path(out[0]) / "VC" / "Auxiliary" / "Build" / "vcvars32.bat"
            if vcvars.exists():
                return vcvars
    sys.exit("ERROR: vcvars32.bat not found. Install the VS C++ build tools.")


def main():
    vcvars = find_vcvars()
    OUT.mkdir(parents=True, exist_ok=True)
    sources = sorted(TESTS.glob("*.cpp"))
    if not sources:
        sys.exit("ERROR: no tests found in %s" % TESTS)

    failed = []
    for src in sources:
        exe = OUT / (src.stem + ".exe")
        compile_cmd = (
            f'call "{vcvars}" >nul && cl /nologo /std:c++20 /EHsc /W3 /Od '
            f'"{src}" /Fe:"{exe}" /Fo:"{OUT}\\\\"'
        )
        build = subprocess.run(compile_cmd, shell=True, cwd=OUT,
                               capture_output=True, text=True, errors="replace")
        if build.returncode != 0:
            print(f"FAIL  {src.name} (compile)\n{build.stdout}{build.stderr}")
            failed.append(src.name)
            continue

        run = subprocess.run([str(exe)], capture_output=True, text=True,
                             errors="replace")
        status = "ok  " if run.returncode == 0 else "FAIL"
        print(f"{status}  {src.name}: {(run.stdout + run.stderr).strip()}")
        if run.returncode != 0:
            failed.append(src.name)

    print(f"\n{len(sources) - len(failed)}/{len(sources)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
