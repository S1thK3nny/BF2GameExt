#!/usr/bin/env python3
"""Port Steam BattlefrontII.exe addresses to the GOG build.

The two retail exes are the same source compiled with the same toolchain; GOG
inserts/removes a little code, so .text is the Steam .text shifted by a
piecewise-constant amount and .data/.rdata/.bss globals move too.

Method (same shape as the modtools->Steam port_confirm pass):

  1. Anchors.  Hash every N-byte window of both .text sections, keep the windows
     that are unique in BOTH, and pair them up.  Each pair yields a candidate
     shift gogVA - steamVA.  Sorting the pairs by Steam VA gives a piecewise
     shift map that predicts where any Steam address lands in GOG.

  2. Code addresses.  Predict gogVA = steamVA + shift(steamVA), then verify by
     disassembling both sides in lockstep: mnemonics and operand shapes must
     match exactly, while 32-bit immediates/displacements that look like VAs are
     allowed to differ (they are the relocated globals).  If the prediction
     fails, re-scan .text for the Steam byte pattern with those VA-shaped fields
     wildcarded.

  3. Globals.  A data address has no bytes to match, so it is ported through
     the code that references it: find `xx xx xx xx` = the Steam VA inside Steam
     .text, port each referencing instruction, then read the same operand slot
     out of the GOG instruction.  Multiple independent sites must agree.

Usage:
    python tools/port_gog.py map                    # dump the shift map
    python tools/port_gog.py code  0x630650 ...     # port code addresses
    python tools/port_gog.py data  0x0093E4A4 ...   # port globals
    python tools/port_gog.py auto                   # port every steam:: addr
                                                    # missing from gog::
"""
import sys, os, re, struct, collections

import pefile
import capstone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Point BF2_GAMEDATA at the folder holding both retail exes, or override either
# path individually with BF2_STEAM_EXE / BF2_GOG_EXE.
GAMEDATA = os.environ.get(
    "BF2_GAMEDATA",
    r"D:\SteamLibrary\steamapps\common\Star Wars Battlefront II Classic\GameData")
STEAM_EXE = os.environ.get("BF2_STEAM_EXE", os.path.join(GAMEDATA, "BattlefrontII.exe"))
GOG_EXE = os.environ.get("BF2_GOG_EXE", os.path.join(GAMEDATA, "BattlefrontII_GOG.exe"))
HPP = os.path.join(ROOT, "PatcherDLL", "src", "core", "game_addrs.hpp")

WINDOW = 24  # anchor window size


class Image:
    """A loaded PE, addressed by (unrelocated) virtual address."""

    def __init__(self, path):
        self.path = path
        pe = pefile.PE(path, fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.sections = []
        for s in pe.sections:
            name = s.Name.rstrip(b"\0").decode("latin1")
            self.sections.append(
                dict(name=name,
                     va=self.base + s.VirtualAddress,
                     vsize=s.Misc_VirtualSize,
                     raw=s.get_data(),
                     exec=bool(s.Characteristics & 0x20000000)))
        pe.close()
        self.text = next(s for s in self.sections if s["name"] == ".text")
        self.text_va = self.text["va"]
        self.text_bytes = self.text["raw"][:self.text["vsize"]]
        self.text_end = self.text_va + len(self.text_bytes)

    def read(self, va, n):
        for s in self.sections:
            if s["va"] <= va < s["va"] + max(s["vsize"], len(s["raw"])):
                off = va - s["va"]
                return s["raw"][off:off + n]
        return b""

    def in_text(self, va):
        return self.text_va <= va < self.text_end

    def toff(self, va):
        return va - self.text_va


def unique_index(buf, window):
    """window-byte key -> single offset, for keys occurring exactly once."""
    seen = {}
    dupe = set()
    for i in range(0, len(buf) - window):
        k = buf[i:i + window]
        if k in dupe:
            continue
        if k in seen:
            del seen[k]
            dupe.add(k)
        else:
            seen[k] = i
    return seen


class ShiftMap:
    """Piecewise-constant Steam .text VA -> GOG .text VA offset."""

    def __init__(self, steam, gog, window=WINDOW):
        si = unique_index(steam.text_bytes, window)
        gi = unique_index(gog.text_bytes, window)
        pairs = []
        for k, soff in si.items():
            goff = gi.get(k)
            if goff is not None:
                pairs.append((steam.text_va + soff, gog.text_va + goff))
        pairs.sort()
        # Keep only anchors that agree with their neighbours: a correct anchor
        # sits in a run of identical shifts, a coincidental match does not.
        runs = []
        for sva, gva in pairs:
            d = gva - sva
            if runs and runs[-1][2] == d:
                runs[-1][1] = sva
                runs[-1][3] += 1
            else:
                runs.append([sva, sva, d, 1])
        self.runs = [r for r in runs if r[3] >= 3]  # >=3 concurring anchors
        self.anchors = len(pairs)

    def shift(self, sva):
        """Shift for a Steam VA: the run covering it, else the nearest one."""
        best = None
        for lo, hi, d, n in self.runs:
            if lo <= sva <= hi:
                return d, "in-run"
            dist = lo - sva if sva < lo else sva - hi
            if best is None or dist < best[0]:
                best = (dist, d)
        return (best[1], "nearest(%d)" % best[0]) if best else (0, "none")


# ---------------------------------------------------------------------------
# instruction-level comparison
# ---------------------------------------------------------------------------

MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
MD.detail = True

VA_LO, VA_HI = 0x00400000, 0x02100000


def looks_like_va(v):
    return VA_LO <= (v & 0xFFFFFFFF) < VA_HI


def insn_key(ins):
    """Comparable shape of an instruction, with VA-valued fields blanked.

    Two builds of the same source emit the same instruction with the same
    register/size operands; only addresses of relocated globals and the targets
    of relative branches change.  Those become '*'.
    """
    parts = [ins.mnemonic]
    for op in ins.operands:
        if op.type == capstone.x86.X86_OP_REG:
            parts.append("r%d" % op.reg)
        elif op.type == capstone.x86.X86_OP_IMM:
            parts.append("i*" if looks_like_va(op.imm) or ins.group(capstone.x86.X86_GRP_JUMP)
                         or ins.group(capstone.x86.X86_GRP_CALL) else "i%d" % op.imm)
        elif op.type == capstone.x86.X86_OP_MEM:
            m = op.mem
            d = "*" if looks_like_va(m.disp) else str(m.disp)
            parts.append("m%d.%d.%d.%s" % (m.base, m.index, m.scale, d))
    return "|".join(parts)


TERMINATORS = ("ret", "retf", "int3", "jmp")


def disasm_keys(img, va, count, stop_at_end=False):
    """(keys, instructions) for `count` instructions starting at va.

    With stop_at_end, decoding stops after a terminator so a short function is
    never compared against whatever the other build happens to place after it.
    """
    data = img.read(va, count * 16 + 32)
    keys, insns = [], []
    for ins in MD.disasm(data, va):
        keys.append(insn_key(ins))
        insns.append(ins)
        if len(keys) >= count:
            break
        if stop_at_end and ins.mnemonic in TERMINATORS:
            break
    return keys, insns


def compare(steam, gog, sva, gva, count=24):
    """Fraction of the first `count` instructions whose shapes match."""
    sk, si = disasm_keys(steam, sva, count, stop_at_end=True)
    gk, gi = disasm_keys(gog, gva, count, stop_at_end=True)
    if not sk or not gk:
        return 0.0, 0
    n = min(len(sk), len(gk))
    same = sum(1 for i in range(n) if sk[i] == gk[i])
    return same / n, n


def port_code(steam, gog, smap, sva, count=24, thresh=0.95):
    """Port one Steam code VA. -> (gogVA, score, how) or (None, score, how)."""
    d, how = smap.shift(sva)
    cand = sva + d
    if gog.in_text(cand):
        score, n = compare(steam, gog, sva, cand, count)
        if score >= thresh and n >= 6:
            return cand, score, "shift %+#x (%s)" % (d, how)
        # Tiny thunks (`MOV AL,1; RET`) terminate in 2-3 instructions, so they
        # can never reach the 6-instruction bar.  Accept them only on an exact
        # full-body match -- there is nothing else to check them against, so
        # anything less than perfect is a miss.
        if n and n < 6 and score == 1.0:
            sk, _ = disasm_keys(steam, sva, count, stop_at_end=True)
            gk, _ = disasm_keys(gog, cand, count, stop_at_end=True)
            if len(sk) == len(gk):
                return cand, score, "shift %+#x (%s, %d-insn thunk)" % (d, how, n)
    # Fallback: masked byte scan of the whole GOG .text.
    hit = masked_scan(steam, gog, sva)
    if hit is not None:
        score, n = compare(steam, gog, sva, hit, count)
        return hit, score, "byte-scan"
    score, _ = compare(steam, gog, sva, cand, count) if gog.in_text(cand) else (0.0, 0)
    return None, score, "shift %+#x (%s) FAILED" % (d, how)


def masked_scan(steam, gog, sva, nbytes=64):
    """Find the Steam function's byte pattern in GOG, wildcarding VA fields."""
    pat = bytearray(steam.read(sva, nbytes))
    if len(pat) < 16:
        return None
    mask = bytearray(b"\xff" * len(pat))
    off = 0
    for ins in MD.disasm(bytes(pat), sva):
        if off + ins.size > len(pat):
            break
        # Wildcard every 4-byte field that holds a VA or a relative target.
        for i in range(ins.size - 3):
            v = struct.unpack_from("<I", pat, off + i)[0]
            if looks_like_va(v):
                mask[off + i:off + i + 4] = b"\x00\x00\x00\x00"
        if ins.group(capstone.x86.X86_GRP_JUMP) or ins.group(capstone.x86.X86_GRP_CALL):
            mask[off + 1:off + ins.size] = b"\x00" * (ins.size - 1)
        off += ins.size
        if off >= len(pat):
            break
    pat, mask = bytes(pat[:off]), bytes(mask[:off])
    if off < 12:
        return None
    # Anchor the search on the longest unmasked run.
    best_i = best_len = 0
    cur = 0
    for i, m in enumerate(mask):
        cur = cur + 1 if m else 0
        if cur > best_len:
            best_len, best_i = cur, i - cur + 1
    if best_len < 8:
        return None
    anchor = pat[best_i:best_i + best_len]
    hits = []
    buf, start = gog.text_bytes, 0
    while True:
        j = buf.find(anchor, start)
        if j < 0:
            break
        start = j + 1
        cand = j - best_i
        if cand < 0:
            continue
        cbuf = buf[cand:cand + off]
        if len(cbuf) == off and all(mask[k] == 0 or cbuf[k] == pat[k] for k in range(off)):
            hits.append(gog.text_va + cand)
        if len(hits) > 4:
            return None
    return hits[0] if len(hits) == 1 else None


# ---------------------------------------------------------------------------
# globals, ported through their referencing instructions
# ---------------------------------------------------------------------------

def find_refs(img, va, limit=40):
    """Offsets in .text where the literal 4-byte VA appears."""
    needle = struct.pack("<I", va)
    out, start = [], 0
    while len(out) < limit:
        j = img.text_bytes.find(needle, start)
        if j < 0:
            break
        out.append(j)
        start = j + 1
    return out


def containing_insn(img, site_va, expect=None, maxback=16):
    """Instruction covering site_va. -> (insn_va, field_offset) or (None, None).

    Tries every back-off and prefers a decode that actually carries `expect` as
    an operand value, which rules out the misaligned decodes that a naive
    "first decode that spans the byte" search picks up.
    """
    best = None
    for back in range(0, maxback):
        _, insns = disasm_keys(img, site_va - back, 1)
        if not insns:
            continue
        ins = insns[0]
        if not (ins.address <= site_va < ins.address + ins.size):
            continue
        field = site_va - ins.address
        if expect is not None:
            vals = [o.imm for o in ins.operands if o.type == capstone.x86.X86_OP_IMM]
            vals += [o.mem.disp for o in ins.operands if o.type == capstone.x86.X86_OP_MEM]
            if any((v & 0xFFFFFFFF) == (expect & 0xFFFFFFFF) for v in vals):
                return ins.address, field  # exact operand match wins outright
        if best is None:
            best = (ins.address, field)
    return best if best else (None, None)


def port_data(steam, gog, smap, sva, want=2):
    """Port a global. -> (gogVA, votes, detail) or (None, 0, detail)."""
    votes = collections.Counter()
    detail = []
    strong = {}
    for soff in find_refs(steam, sva):
        site_va = steam.text_va + soff
        ins_va, field = containing_insn(steam, site_va, expect=sva)
        if ins_va is None:
            continue
        gsite, score, _ = port_code(steam, gog, smap, ins_va, count=8, thresh=0.85)
        if gsite is None:
            continue
        raw = gog.read(gsite + field, 4)
        if len(raw) != 4:
            continue
        gva = struct.unpack("<I", raw)[0]
        if looks_like_va(gva):
            votes[gva] += 1
            if score >= 0.99:
                strong[gva] = strong.get(gva, 0) + 1
            detail.append("%#x->%#x (score %.2f)" % (ins_va, gva, score))
    if not votes:
        return None, 0, "no usable refs"
    gva, n = votes.most_common(1)[0]
    # A vtable base or other write-once global has exactly one referencing site;
    # a single site that ported at full confidence is still conclusive.
    ok = n >= want or (len(votes) == 1 and strong.get(gva, 0) >= 1)
    return (gva if ok else None), n, "; ".join(detail[:4])


def port_op(steam, gog, smap, sva):
    """Port an address that points INTO an instruction (an operand field).

    Disassembling from such an address decodes garbage, so port the containing
    instruction and re-apply the field offset.
    """
    # Several back-offs can decode into an instruction spanning sva (x86 is not
    # self-synchronising backwards); the real one is the one that ports.
    for back in range(0, 16):
        _, insns = disasm_keys(steam, sva - back, 1)
        if not insns:
            continue
        ins = insns[0]
        if not (ins.address <= sva < ins.address + ins.size):
            continue
        field = sva - ins.address
        gins, score, how = port_code(steam, gog, smap, ins.address, count=10, thresh=0.9)
        if gins is not None and score >= 0.9:
            return gins + field, score, "insn %#x+%d -> %#x+%d (%s)" % (
                ins.address, field, gins, field, how)
    return None, 0.0, "no containing instruction ported"


# ---------------------------------------------------------------------------
# .rdata pointer tables (vtables, string tables)
# ---------------------------------------------------------------------------

def read_ptrs(img, va, n):
    raw = img.read(va, n * 4)
    return list(struct.unpack_from("<%dI" % (len(raw) // 4), raw)) if len(raw) >= 4 else []


def find_ptr_seq(img, seq, section_names=(".rdata", ".data", ".text")):
    """Every VA where the exact pointer sequence `seq` appears."""
    needle = struct.pack("<%dI" % len(seq), *seq)
    hits = []
    for s in img.sections:
        if s["name"] not in section_names:
            continue
        buf, start = s["raw"], 0
        while True:
            j = buf.find(needle, start)
            if j < 0:
                break
            hits.append(s["va"] + j)
            start = j + 1
    return hits


def port_vtable_slot(steam, gog, smap, sva, ctx=8):
    """Port a vtable slot address by porting its neighbourhood of function
    pointers and locating that pointer run in the GOG image.

    Interior vtable addresses have no code reference to ride on, but a run of
    consecutive slots is a fingerprint: port each Steam entry as code, then
    search GOG .rdata for the resulting sequence.
    """
    for back in range(ctx, -1, -1):
        base = sva - back * 4
        vals = read_ptrs(steam, base, ctx + 1)
        if len(vals) < 4 or not all(steam.in_text(v) for v in vals):
            continue
        ported = []
        for v in vals:
            g, sc, _ = port_code(steam, gog, smap, v, count=12, thresh=0.9)
            if g is None:
                break
            ported.append(g)
        if len(ported) < 4:
            continue
        for take in range(len(ported), 3, -1):
            hits = find_ptr_seq(gog, ported[:take])
            if len(hits) == 1:
                return hits[0] + back * 4, take, "%d-slot run @%#x" % (take, base)
    # Fallback: a slot run made only of folded thunks (`MOV AL,1; RET` and
    # friends) repeats across classes and is never unique.  Walk back to the
    # vtable base instead — that address IS referenced, by the ctor storing it —
    # and re-apply the slot offset.
    for back in range(0, 4096, 4):
        base = sva - back
        gbase, votes, _ = port_data(steam, gog, smap, base)
        if gbase is not None:
            return gbase + back, votes, "vtable base %#x -> %#x, slot +%#x" % (base, gbase, back)
    return None, 0, "no unique slot run and no referenced base"


def cstr(img, va, maxlen=128):
    raw = img.read(va, maxlen)
    i = raw.find(b"\0")
    return raw[:i if i >= 0 else len(raw)]


def port_string_table(steam, gog, smap, sva, stride=4, ctx=6):
    """Port a table of char* (optionally with padding between entries) by
    matching the strings it points at."""
    for back in range(ctx, -1, -1):
        base = sva - back * stride
        gseq = []
        ok = True
        for i in range(ctx + 1):
            p = read_ptrs(steam, base + i * stride, 1)
            if not p:
                ok = False
                break
            s = cstr(steam, p[0])
            if not s or not all(32 <= c < 127 for c in s):
                ok = False
                break
            hits = [h for h in find_ptr_seq_bytes(gog, s + b"\0")]
            if len(hits) != 1:
                ok = False
                break
            gseq.append(hits[0])
        if not ok or len(gseq) < 4:
            continue
        # Locate the GOG pointer table: entries at `stride` spacing.
        cands = None
        for s_ in gog.sections:
            if s_["name"] not in (".rdata", ".data"):
                continue
            buf = s_["raw"]
            first = struct.pack("<I", gseq[0])
            start = 0
            while True:
                j = buf.find(first, start)
                if j < 0:
                    break
                start = j + 1
                if all(buf[j + i * stride:j + i * stride + 4] == struct.pack("<I", gseq[i])
                       for i in range(len(gseq))):
                    cands = (cands or []) + [s_["va"] + j]
        if cands and len(cands) == 1:
            return cands[0] + back * stride, len(gseq), "%d strings from %#x" % (len(gseq), base)
    return None, 0, "no unique string run"


def find_ptr_seq_bytes(img, needle, section_names=(".rdata", ".data")):
    hits = []
    for s in img.sections:
        if s["name"] not in section_names:
            continue
        buf, start = s["raw"], 0
        while True:
            j = buf.find(needle, start)
            if j < 0:
                break
            # Must be a string start (preceded by NUL or padding).
            if j == 0 or buf[j - 1] in (0, 0xCC):
                hits.append(s["va"] + j)
            start = j + 1
    return hits


# ---------------------------------------------------------------------------
# game_addrs.hpp parsing
# ---------------------------------------------------------------------------

def parse_ns(name):
    src = open(HPP, encoding="utf-8", errors="replace").read()
    body = re.search(r"namespace %s\s*\{(.*?)\n\s*\}\s*//\s*namespace %s" % (name, name),
                     src, re.S).group(1)
    scal = {m.group(1): int(m.group(2), 16)
            for m in re.finditer(r"constexpr uintptr_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;", body)}
    arr = {}
    for m in re.finditer(r"constexpr uintptr_t\s+(\w+)\[\]\s*=\s*\{(.*?)\}", body, re.S):
        arr[m.group(1)] = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", m.group(2))]
    return scal, arr


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "map"
    steam, gog = Image(STEAM_EXE), Image(GOG_EXE)
    smap = ShiftMap(steam, gog)

    if cmd == "map":
        print("anchors=%d runs=%d" % (smap.anchors, len(smap.runs)))
        print("steam .text %#x-%#x   gog .text %#x-%#x"
              % (steam.text_va, steam.text_end, gog.text_va, gog.text_end))
        for lo, hi, d, n in smap.runs:
            print("  %#010x - %#010x  shift %+#8x  (%d anchors)" % (lo, hi, d, n))
        return

    if cmd == "code":
        for a in sys.argv[2:]:
            sva = int(a, 16)
            gva, score, how = port_code(steam, gog, smap, sva)
            print("%#010x -> %s   score=%.2f  %s"
                  % (sva, ("%#010x" % gva) if gva else "FAIL", score, how))
        return

    if cmd == "data":
        for a in sys.argv[2:]:
            sva = int(a, 16)
            gva, n, detail = port_data(steam, gog, smap, sva)
            print("%#010x -> %s   votes=%d  %s"
                  % (sva, ("%#010x" % gva) if gva else "FAIL", n, detail))
        return

    if cmd == "op":
        for a in sys.argv[2:]:
            sva = int(a, 16)
            gva, score, how = port_op(steam, gog, smap, sva)
            print("%#010x -> %s   score=%.2f  %s"
                  % (sva, ("%#010x" % gva) if gva else "FAIL", score, how))
        return

    if cmd == "vt":
        for a in sys.argv[2:]:
            sva = int(a, 16)
            gva, n, how = port_vtable_slot(steam, gog, smap, sva)
            print("%#010x -> %s   slots=%d  %s"
                  % (sva, ("%#010x" % gva) if gva else "FAIL", n, how))
        return

    if cmd == "strtab":
        stride = int(sys.argv[2])
        for a in sys.argv[3:]:
            sva = int(a, 16)
            gva, n, how = port_string_table(steam, gog, smap, sva, stride)
            print("%#010x -> %s   n=%d  %s"
                  % (sva, ("%#010x" % gva) if gva else "FAIL", n, how))
        return

    if cmd == "deep":
        # Whole-function comparison: every instruction, listing any divergence.
        # Used to prove struct offsets are shared between the two release
        # builds -- non-VA displacements are compared exactly by insn_key, so a
        # clean run means every field offset in the function is identical.
        for a in sys.argv[2:]:
            sva = int(a, 16)
            gva, score, how = port_code(steam, gog, smap, sva)
            if gva is None:
                print("%#010x FAIL (%s)" % (sva, how))
                continue
            sk, si = disasm_keys(steam, sva, 4000, stop_at_end=True)
            gk, gi = disasm_keys(gog, gva, 4000, stop_at_end=True)
            n = min(len(sk), len(gk))
            bad = [i for i in range(n) if sk[i] != gk[i]]
            print("%#010x -> %#010x  %d/%d insns compared, %d differ  (%s)"
                  % (sva, gva, n, max(len(sk), len(gk)), len(bad), how))
            for i in bad[:8]:
                print("     steam %08x %-9s %-28s | gog %08x %-9s %s"
                      % (si[i].address, si[i].mnemonic, si[i].op_str,
                         gi[i].address, gi[i].mnemonic, gi[i].op_str))
        return

    if cmd == "check":
        # Re-derive every address already present in gog:: and compare against
        # the hand-derived value.  Disagreement means one of the two is wrong.
        sscal, _ = parse_ns("steam")
        gscal, _ = parse_ns("gog")
        agree = differ = skip = 0
        for name in sorted(gscal):
            if name not in sscal:
                skip += 1
                continue
            sva, have = sscal[name], gscal[name]
            if steam.in_text(sva):
                got, score, how = port_code(steam, gog, smap, sva)
                if got is None:
                    got, score, how = port_op(steam, gog, smap, sva)
            else:
                got, n, how = port_data(steam, gog, smap, sva)
                score = n
            tag = "OK " if got == have else "DIFF"
            if got == have:
                agree += 1
            else:
                differ += 1
            print("%-4s %-40s steam %#010x  have %#010x  got %s   %s"
                  % (tag, name, sva, have, ("%#010x" % got) if got else "FAIL", how[:60]))
        print("# agree=%d differ=%d no-steam-counterpart=%d" % (agree, differ, skip))
        return

    if cmd == "auto":
        sscal, sarr = parse_ns("steam")
        gscal, garr = parse_ns("gog")
        todo = [(k, v) for k, v in sscal.items() if k not in gscal]
        print("# steam=%d gog=%d missing=%d" % (len(sscal), len(gscal), len(todo)))
        for name, sva in todo:
            if steam.in_text(sva):
                gva, score, how = port_code(steam, gog, smap, sva)
                print("%-38s %#010x -> %-12s CODE score=%.2f %s"
                      % (name, sva, ("%#010x" % gva) if gva else "FAIL", score, how))
            else:
                gva, n, detail = port_data(steam, gog, smap, sva)
                print("%-38s %#010x -> %-12s DATA votes=%d %s"
                      % (name, sva, ("%#010x" % gva) if gva else "FAIL", n, detail[:90]))
        for name, vals in sarr.items():
            if name in garr:
                continue
            for sva in vals:
                gva, score, how = port_code(steam, gog, smap, sva, count=6, thresh=0.85)
                print("%-38s %#010x -> %-12s ARR  score=%.2f %s"
                      % (name + "[]", sva, ("%#010x" % gva) if gva else "FAIL", score, how))
        return

    print(__doc__)


if __name__ == "__main__":
    main()
