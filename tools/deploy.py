#!/usr/bin/env python3
"""Deploy the built DLL and refresh the INI without losing the user's settings.

The INI is merged, never overwritten: every key already present in the live file
keeps its value, new keys arrive with their generated defaults, and the comment
text is refreshed from dist/BF2GameExt.ini. An earlier fixed "preserve these
six" list silently reset a diagnostic the user had turned on, which is exactly
what this avoids.
"""
import io, os, shutil, sys

GAMEDATA = os.environ.get(
    "BF2_GAMEDATA",
    r"C:/Program Files (x86)/Steam/steamapps/common/Star Wars Battlefront II Classic/GameData")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_values(path):
    """(section, key) -> value for every assignment in an ini."""
    out, sec = {}, None
    if not os.path.exists(path):
        return out
    for line in io.open(path, encoding="utf-8", errors="replace"):
        t = line.strip()
        if t.startswith("[") and t.endswith("]"):
            sec = t
        elif t and not t.startswith(";") and "=" in t:
            k, v = t.split("=", 1)
            out[(sec, k.strip())] = v.strip()
    return out


def main():
    live_ini = os.path.join(GAMEDATA, "BF2GameExt.ini")
    dist_ini = os.path.join(ROOT, "dist", "BF2GameExt.ini")
    dll_src = os.path.join(ROOT, "bin", "Release", "BF2GameExt.dll")

    if not os.path.exists(dll_src):
        sys.exit("no built DLL at %s" % dll_src)

    live = read_values(live_ini)
    out, sec, kept, added = [], None, 0, []
    for line in io.open(dist_ini, encoding="utf-8"):
        t = line.strip()
        if t.startswith("[") and t.endswith("]"):
            sec = t
        elif t and not t.startswith(";") and "=" in t:
            k = t.split("=", 1)[0].strip()
            if (sec, k) in live:
                line = "%s=%s\n" % (k, live[(sec, k)])
                kept += 1
            else:
                # brand new key: keep the generated default
                added.append("%s %s" % (sec, k))
        out.append(line)

    if os.path.exists(live_ini):
        shutil.copy2(live_ini, live_ini + ".bak")
    io.open(live_ini, "w", encoding="utf-8", newline="").write("".join(out))
    print("ini: %d existing values preserved, %d new key(s)%s"
          % (kept, len(added), (": " + ", ".join(added)) if added else ""))

    try:
        shutil.copy2(dll_src, os.path.join(GAMEDATA, "BF2GameExt.dll"))
    except PermissionError:
        sys.exit("DLL is locked - close the game and re-run")
    print("dll: deployed %d bytes" % os.path.getsize(os.path.join(GAMEDATA, "BF2GameExt.dll")))


if __name__ == "__main__":
    main()
