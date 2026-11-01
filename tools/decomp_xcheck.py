#!/usr/bin/env python3
"""Cross-check the steam addresses in game_addrs.hpp against the SWBF2 decomp.

The decomp (https://github.com/Gametoast/swbf2decomp) is a reccmp matching
decompilation of the Steam exe. Every function, global and vtable carries a
"// FUNCTION: SWBF2 0x..." style annotation. This tool reads those, then sorts
each game_addrs::steam constant into:

  exact   the address is an annotated symbol; the decomp name is shown, and
          the entry is flagged when it shares no word with our name
  inside  the address falls inside an annotated function (a patch or call
          site), shown as Function+offset
  data    the address follows an annotated global, vtable or string
  none    no annotation near it

Nothing from the decomp is copied into this repo; it is only read from the
path you pass in.

Usage:
    python tools/decomp_xcheck.py <decomp>               # summary + flagged names
    python tools/decomp_xcheck.py <decomp> --all         # every constant
    python tools/decomp_xcheck.py <decomp> --users       # add files using each one
    python tools/decomp_xcheck.py <decomp> --addr 0x62a146 [0x...]
    python tools/decomp_xcheck.py <decomp> --find PropGenerator::Cleanup
<decomp> is the repo root or its src/ folder; defaults to $SWBF2_DECOMP.
Exit code is 1 when a name is flagged, so it can gate a check.
"""
import argparse, bisect, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HPP = os.path.join(ROOT, "PatcherDLL", "src", "core", "game_addrs.hpp")
DLL_SRC = os.path.join(ROOT, "PatcherDLL", "src")

ANNOT = re.compile(r"//\s*(FUNCTION|SYNTHETIC|TEMPLATE|LIBRARY|GLOBAL|VTABLE|STRING):\s*SWBF2\s+0x([0-9a-fA-F]+)")
CODE = {"FUNCTION", "SYNTHETIC", "TEMPLATE", "LIBRARY"}
MAX_FUNC = 0x10000  # an "inside" hit further than this from its function is not trusted

# Words in our names that say what the address is, not what it belongs to
NOISE = {"vtable", "global", "call", "addr", "site", "slot", "ptr", "thunk", "orig", "hook",
         "base", "func", "real", "jmp", "ret", "patch", "start", "end", "offset", "data",
         "get", "set", "the", "and", "for", "from", "with", "stub", "entry", "loop", "check",
         "steam", "retail", "next", "return", "false", "true", "unk", "init", "update"}
# Our abbreviations -> decomp words (first 4 letters compared)
ALIAS = {"snd": "soun", "veh": "vehi", "anim": "anim", "cp": "comm", "hud": "hud", "lua": "lua",
         "msg": "mess", "mgr": "mana", "obj": "obje", "ent": "enti", "tex": "text", "mat": "mate",
         "ai": "ai", "fx": "effe", "gc": "gala", "net": "net", "lod": "lod", "pbl": "pbl",
         "red": "red", "cam": "came", "pos": "posi", "dmg": "dama", "wpn": "weap", "ord": "ordn"}

# Checked by hand: our name differs but the address is the one the code wants.
# Key is the constant name, value is the decomp symbol it was checked against.
REVIEWED = {
    "veh_view_return_false_thunk": "Trackable::IsForcedFirstPerson",  # shared XOR AL,AL; RET
    "joystick_discover":      "PlayerProfile::tProfile::UpdateProfCRC",
    "joystick_sync":          "PlayerProfile::tProfile::ApplySettings",
    "joystick_config_base":   "mProfiles",
    "controller_base_global": "FLInputManager::s_instance",
    "blade_lookup":           "WeaponMeleeClass::_AttachDamageEdge",
    "blade_global_namehash":  "s_uiAttachToHP",
    "blade_global_unk":       "s_uiEdgeHP",
    "hud_math_factory_alloc": "operator new",
    "spline_build":           "PblHermiteVector3::Set",
    "controls_row_actions":   "sKeyboardCmdsPerMode",
    "ai_lod_interval_t0":     "__xmm@3f800000400000004040000040800000",
}


def words(text):
    text = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", text)
    text = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1 \2", text)
    return [w for w in re.findall(r"[a-z]+", text.lower())]


def key(w):
    return ALIAS.get(w, w[:4])


# ---- decomp side ------------------------------------------------------------

def enclosing_class(lines, i):
    """Nearest class/struct opened above line i at a smaller indent (inline members)."""
    indent = len(lines[i]) - len(lines[i].lstrip())
    for j in range(i - 1, max(-1, i - 2000), -1):
        m = re.match(r"(\s*)(?:class|struct)\s+(\w+)(?![^;{]*;\s*$)", lines[j])
        if m and len(m.group(1)) < indent:
            return m.group(2)
    return None


def symbol_name(lines, i):
    """Name for the annotation block that ends at line i-1."""
    for j in range(i, min(i + 6, len(lines))):
        s = lines[j].strip()
        if not s or ANNOT.search(s) or re.match(r"template\s*<[^>]*>$", s):
            continue
        s = re.sub(r"^(?:inline\s+)?STDMETHODIMP(?:_\([^)]*\))?\s*", "", s)
        if s.startswith("//"):
            return s[2:].strip()
        m = re.match(r"(?:template\s+)?(?:class|struct)\s+(\w+)", s)
        if m and "(" not in s:
            return m.group(1)
        if "(" in s:
            head = s.split("(")[0]
            m = re.search(r"([\w:~]+(?:<[^()]*>)?(?:::[\w~]+)?|operator\s*\S+)\s*$", head.strip())
            name = m.group(1) if m else head.strip()
            if not re.match(r"(?:template\s+)?\w", s) or name in ("if", "while", "for"):
                return s[:80]
            if "::" not in name:
                cls = enclosing_class(lines, j)
                if cls and name.lstrip("~") != cls or cls and name.startswith("~"):
                    name = "%s::%s" % (cls, name)
            return name
        m = re.search(r"([\w:]+)\s*(?:\[[^\]]*\])*\s*(?:=|;|\{|$)", s)
        return m.group(1) if m else s[:80]
    return "?"


def load_decomp(src):
    syms = {}  # addr -> (kind, name, relpath, line)
    for root, _, files in os.walk(src):
        for f in files:
            if not f.lower().endswith((".cpp", ".c", ".h", ".hpp", ".inl", ".inc")):
                continue
            path = os.path.join(root, f)
            with open(path, encoding="latin-1") as fh:
                lines = fh.read().splitlines()
            pending = []
            for i, line in enumerate(lines):
                m = ANNOT.search(line)
                if m:
                    pending.append((m.group(1), int(m.group(2), 16), i + 1))
                    continue
                if pending and line.strip():
                    name = symbol_name(lines, i)
                    rel = os.path.relpath(path, src).replace("\\", "/")
                    for kind, addr, ln in pending:
                        syms.setdefault(addr, (kind, name, rel, ln))
                    pending = []
    return syms


# ---- our side ---------------------------------------------------------------

def load_ours():
    src = open(HPP, encoding="utf-8", errors="replace").read()
    body = re.search(r"namespace steam\s*\{(.*?)\n\}\s*//\s*namespace steam", src, re.S)
    if not body:
        sys.exit("steam namespace not found in " + HPP)
    out = []
    for m in re.finditer(r"constexpr\s+uintptr_t\s+(\w+)\s*(\[\s*\w*\s*\])?\s*=\s*([^;]+);", body.group(1)):
        name, arr, val = m.group(1), m.group(2), m.group(3)
        vals = re.findall(r"0x[0-9a-fA-F]+", val)
        for n, v in enumerate(vals):
            a = int(v, 16)
            if 0x400000 <= a < 0x2000000:
                out.append(("%s[%d]" % (name, n) if arr else name, a))
    return out


def load_users(names):
    """Source files that mention each constant (g_addr->field, a namespace, or bare)."""
    bare = {n.split("[")[0] for n in names}
    users = {n: set() for n in bare}
    pat = re.compile(r"\b(\w+)\b")
    for root, _, files in os.walk(DLL_SRC):
        for f in files:
            if not f.endswith((".cpp", ".hpp", ".h", ".inc")) or f.startswith("game_addrs"):
                continue
            path = os.path.join(root, f)
            text = open(path, encoding="utf-8", errors="replace").read()
            rel = os.path.relpath(path, DLL_SRC).replace("\\", "/")
            for w in set(pat.findall(text)) & bare:
                users[w].add(rel)
    return users


# ---- classify ---------------------------------------------------------------

def classify(addr, syms, starts):
    if addr in syms:
        return "exact", syms[addr], 0
    i = bisect.bisect_right(starts, addr) - 1
    if i < 0:
        return "none", None, 0
    base = starts[i]
    sym, off = syms[base], addr - base
    if sym[0] in CODE:
        nxt = starts[i + 1] if i + 1 < len(starts) else None
        if off < MAX_FUNC and (nxt is None or addr < nxt):
            return "inside", sym, off
        return "none", sym, off
    return ("data", sym, off) if off < 0x10000 else ("none", sym, off)


def disagrees(ours, sym):
    a = {key(w) for w in words(ours.split("[")[0]) if len(w) >= 2 and w not in NOISE}
    if not a:
        return False
    stem = os.path.splitext(os.path.basename(sym[2]))[0]
    b = {key(w) for w in words(sym[1] + " " + stem) if len(w) >= 2}
    return not (a & b)


def fmt(sym, off):
    if not sym:
        return "-"
    loc = "%s:%d" % (sym[2], sym[3])
    name = sym[1] if not off else "%s+0x%X" % (sym[1], off)
    return "%-9s %-60s %s" % (sym[0], name[:60], loc)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("decomp", nargs="?", default=os.environ.get("SWBF2_DECOMP"))
    ap.add_argument("--all", action="store_true", help="list every constant")
    ap.add_argument("--users", action="store_true", help="show which DLL files use each constant")
    ap.add_argument("--addr", nargs="+", help="look up raw Steam addresses")
    ap.add_argument("--find", help="look up decomp symbols whose name contains this text")
    args = ap.parse_args()
    if not args.decomp:
        ap.error("pass the decomp path or set SWBF2_DECOMP")
    src = args.decomp
    if os.path.isdir(os.path.join(src, "src")):
        src = os.path.join(src, "src")
    if not os.path.isdir(src):
        sys.exit("not a directory: " + src)

    syms = load_decomp(src)
    starts = sorted(syms)

    if args.addr or args.find:
        if args.addr:
            for a in args.addr:
                kind, sym, off = classify(int(a, 16), syms, starts)
                print("%08X  %-6s %s" % (int(a, 16), kind, fmt(sym, off)))
        if args.find:
            q = args.find.lower()
            for a in starts:
                if q in syms[a][1].lower():
                    print("%08X  %s" % (a, fmt(syms[a], 0)))
        return 0

    ours = load_ours()
    users = load_users([n for n, _ in ours]) if args.users else {}
    rows, counts, flagged = [], {"exact": 0, "inside": 0, "data": 0, "none": 0}, []
    for name, addr in ours:
        kind, sym, off = classify(addr, syms, starts)
        counts[kind] += 1
        bad = kind == "exact" and disagrees(name, sym) and REVIEWED.get(name) != sym[1]
        if bad:
            flagged.append(name)
        rows.append((addr, name, kind, sym, off, bad))

    print("decomp symbols: %d   steam constants: %d" % (len(syms), len(ours)))
    print("  exact %(exact)d   inside %(inside)d   data %(data)d   none %(none)d" % counts)

    def show(title, sel):
        sel = [r for r in rows if sel(r)]
        if not sel:
            return
        print("\n== %s (%d)" % (title, len(sel)))
        for addr, name, kind, sym, off, bad in sorted(sel):
            print("%08X  %-44s %-6s %s%s" % (addr, name[:44], kind, "! " if bad else "", fmt(sym, off)))
            if args.users:
                bare = name.split("[")[0]
                print("%10s used by: %s" % ("", ", ".join(sorted(users.get(bare, ()))) or "(nothing)"))

    show("name disagrees with decomp", lambda r: r[5])
    if args.all:
        show("all constants", lambda r: True)
    else:
        show("no decomp annotation nearby", lambda r: r[2] == "none")
    return 1 if flagged else 0


if __name__ == "__main__":
    sys.exit(main())
