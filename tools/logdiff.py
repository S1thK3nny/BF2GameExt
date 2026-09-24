#!/usr/bin/env python3
"""Check that a change did not alter what GameExt logs on any build.

Used to verify behaviour-preserving cleanups: capture the logs once before the
change (the baseline), then after it, and diff. A difference means some hook or
patch set installed differently, e.g. a feature that quietly turned itself off.

Expected files in GameData, one launch per build, renamed after each run:
    BF2GameExt_Modtools.log  BF2GameExt_Steam.log  BF2GameExt_GOG.log
    BFront2_Modtools.log     BFront2_Steam.log     BFront2_GOG.log
Steam and GOG only write BFront2.log with [Features] GameLogging=1.

BF2GameExt logs are compared whole. BFront2 logs are reduced to GameExt's own
messages (header "PatcherDLL\\src\\..."), as "severity | file | text" with the
source line number dropped, since refactors move lines. Shell/Lua chatter varies
with every click and is ignored.

Normalized before comparing: build stamp, clock times, 8-digit hex values
outside the exe image range (heap/stack/DLL pointers, which move every launch),
the free-address-space figure, and all numbers in the runtime-diagnostic lines
(SndDiag, PoolGrow, final stats), which scale with session length.

Usage:
    python tools/logdiff.py <GameData> --save   # store current logs as baseline
    python tools/logdiff.py <GameData>          # compare current logs to it
<GameData> defaults to $BF2_GAMEDATA. The baseline lives in <GameData>/baseline.
"""
import difflib, os, re, shutil, sys

BUILDS = ["Modtools", "Steam", "GOG"]
NAMES = ["%s_%s.log" % (kind, b) for b in BUILDS for kind in ("BF2GameExt", "BFront2")]


def norm(line):
    line = re.sub(r"\(built [^)]*\)", "(built <stamp>)", line)
    line = re.sub(r"\b\d{1,2}:\d{2}:\d{2}(\.\d+)?\b", "<time>", line)

    def hexsub(m):
        v = int(m.group(2), 16)
        keep = 0x400000 <= v < 0x2000000  # exe image incl. retail .bss
        return (m.group(1) or "") + (m.group(2) if keep else "<ptr>")

    line = re.sub(r"\b(0x)?([0-9A-Fa-f]{7,8})\b", hexsub, line)
    line = re.sub(r"largest free range was \d+", "largest free range was N", line)
    # Runtime counters scale with session length, not with the code.
    if re.match(r"\s*\[(SndDiag|PoolGrow)\]", line) or "Final stats" in line:
        # SndDiag also prints addresses inside a sound module that rebases per launch.
        line = re.sub(r"\b[0-9A-Fa-f]{8}\b", "<ptr>", line)
        line = re.sub(r"\b\d+(\.\d+)?\b", "N", line)
    return line.rstrip()


def read(p):
    return open(p, encoding="utf-8", errors="replace").read().splitlines()


def gameext_lines(p):
    # SndDiag prints a timed report every 600 engine ticks (~10 s), so how many
    # appear depends on how long the session ran. Drop the report header line
    # ("[SndDiag] HH:MM:SS.mmm  tick N ...") and its indented detail lines; the
    # install line and the first/final markers are still compared.
    report = re.compile(r"\[SndDiag\] (\d{2}:\d{2}:\d{2}|  )")
    return [norm(l) for l in read(p) if not report.match(l)]


def bfront2_lines(p):
    lines, out = read(p), []
    for i, l in enumerate(lines):
        if not l.startswith("Message Severity:") or i + 2 >= len(lines):
            continue
        hdr = lines[i + 1]
        if not hdr.startswith("PatcherDLL"):
            continue
        src = re.sub(r"\(\d+\)$", "", hdr)
        out.append(norm("%s | %s | %s" % (l.split(":", 1)[1].strip(), src, lines[i + 2])))
    return out


def save(gamedata, baseline):
    missing = [n for n in NAMES if not os.path.exists(os.path.join(gamedata, n))]
    if missing:
        sys.exit("ERROR: missing %s" % ", ".join(missing))
    os.makedirs(baseline, exist_ok=True)
    for n in NAMES:
        shutil.copy2(os.path.join(gamedata, n), os.path.join(baseline, n))
    print("Saved %d logs to %s" % (len(NAMES), baseline))


def compare(gamedata, baseline):
    bad = 0
    for name in NAMES:
        fn = gameext_lines if name.startswith("BF2GameExt") else bfront2_lines
        pa, pb = os.path.join(baseline, name), os.path.join(gamedata, name)
        if not os.path.exists(pa) or not os.path.exists(pb):
            print("MISSING  %s" % name)
            bad += 1
            continue
        a, c = fn(pa), fn(pb)
        d = list(difflib.unified_diff(a, c, "baseline/" + name, "current/" + name,
                                      lineterm="", n=1))
        if d:
            bad += 1
            print("DIFF     %s\n%s\n" % (name, "\n".join(d)))
        else:
            print("same     %s (%d lines)" % (name, len(a)))
    return bad


def main():
    args = [a for a in sys.argv[1:] if a != "--save"]
    gamedata = args[0] if args else os.environ.get("BF2_GAMEDATA")
    if not gamedata or not os.path.isdir(gamedata):
        sys.exit(__doc__)
    baseline = os.path.join(gamedata, "baseline")
    if "--save" in sys.argv:
        save(gamedata, baseline)
    else:
        sys.exit(1 if compare(gamedata, baseline) else 0)


if __name__ == "__main__":
    main()
