#!/usr/bin/env python3
"""Generate the runtime GameAddrs table from the game_addrs.hpp namespaces.

Reads PatcherDLL/src/core/game_addrs.hpp and emits two generated includes:
  - game_addrs_table.inc : struct GameAddrs { uintptr_t <field>; ... };
  - game_addrs_data.inc  : constexpr GameAddrs kAddrs{Modtools,Steam,GOG} = {...};

Each field is one scalar game function/global. A build's instance lists only the
addresses defined in that build's namespace; everything else defaults to 0
(= "unknown for this build"), which is what the per-hook HOOK_REQUIRE / AddrCheck
gates test. Run this after adding/removing scalar addresses in the namespaces.
"""
import re, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HPP = os.path.join(ROOT, "PatcherDLL", "src", "core", "game_addrs.hpp")
OUT = os.path.join(ROOT, "PatcherDLL", "src", "core")

def ns(src, name):
    body = re.search(r'namespace %s\s*\{(.*?)\n\s*\}\s*//\s*namespace %s' % (name, name), src, re.S).group(1)
    # scalar constexpr uintptr_t NAME = 0x..; (arrays like foo[] are excluded)
    return {m.group(1): m.group(2) for m in re.finditer(r'constexpr uintptr_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;', body)}

def main():
    src = open(HPP, encoding="utf-8", errors="replace").read()
    mod, steam, gog = ns(src, "modtools"), ns(src, "steam"), ns(src, "gog")
    names = list(mod)
    for n in list(steam) + list(gog):
        if n not in names:
            names.append(n)

    struct = ["// AUTO-GENERATED from game_addrs.hpp namespaces by tools/gen_game_addrs_table.py.",
              "// Do not edit by hand; regenerate after editing the modtools/steam/gog namespaces.",
              "// 0 = address unknown for the active build.",
              "struct GameAddrs {"] + ["   uintptr_t %s;" % n for n in names] + ["};"]
    open(os.path.join(OUT, "game_addrs_table.inc"), "w", encoding="ascii").write("\n".join(struct) + "\n")

    data = ["// AUTO-GENERATED from game_addrs.hpp namespaces by tools/gen_game_addrs_table.py. Do not edit."]
    for var, nsname, tbl in [("kAddrsModtools", "modtools", mod), ("kAddrsSteam", "steam", steam), ("kAddrsGOG", "gog", gog)]:
        data.append("constexpr GameAddrs %s = {" % var)
        data += ["   .%s = game_addrs::%s::%s," % (n, nsname, n) for n in names if n in tbl]
        data.append("};")
    open(os.path.join(OUT, "game_addrs_data.inc"), "w", encoding="ascii").write("\n".join(data) + "\n")

    print("fields=%d  modtools=%d  steam=%d  gog=%d" % (len(names), len(mod), len(steam), len(gog)))

if __name__ == "__main__":
    main()
