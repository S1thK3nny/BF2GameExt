"""Read-only audit of the walker stomp fix against all three game PEs.

Usage: python tests/walker_stomp_fix_abi_tests.py PATH_TO_GAMEDATA
Requires pefile and capstone. Reads the addresses (game_addrs.hpp), the
prologue and the compares the install checks (entity/walker_stomp_fix.cpp),
and checks them, and the code the fix rests on, against each executable:
UpdateState's contract and that its first argument is the update's length,
and that each compare it moves is type 1's test with 0.1 in
DoFootImpactEffects. Never loads or executes the game. Not an in-game
behaviour test.
"""
import codecs
import re
import struct
import sys
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "PatcherDLL" / "src"
module = (SRC / "entity" / "walker_stomp_fix.cpp").read_text()
header = (SRC / "entity" / "walker_stomp_fix.hpp").read_text()
walker = (SRC / "core" / "layout" / "walker.hpp").read_text()
addresses = (SRC / "core" / "game_addrs.hpp").read_text()

assert re.search(r"kWalkerStompSpeed\s*=\s*3\.0f;", header), "the speed: 0.1 an update at 30 a second"


def array(name):
    match = re.search(r"const uint8_t " + name + r"\[\]\s*=\s*\{([^}]*)\};", module)
    assert match, name
    return bytes(int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]{2}", match.group(1)))


compares = {
    "modtools": [array("kTestModtools")],
    "steam": [array("kRearmSteam"), array("kLandSteam")],
    "gog": [array("kRearmGog"), array("kLandGog")],
}


def guards():
    out = {}
    for m in re.finditer(r'guard\(base, g_addr->(\w+), "[^"]*",\s*(.*?)\)\)', module, re.S):
        pair = re.match(r'modtools \? ((?:"[^"]*"\s*)+):\s*((?:"[^"]*"\s*)+),\s*'
                        r'modtools \? "([^"]*)" : "([^"]*)"', m.group(2))
        assert pair, "Update the audit if the guard changes shape: " + m.group(1)
        mt = "".join(re.findall(r'"([^"]*)"', pair.group(1)))
        rt = "".join(re.findall(r'"([^"]*)"', pair.group(2)))
        out[m.group(1)] = {"modtools": (mt, pair.group(3)), "release": (rt, pair.group(4))}
    return out


GUARDS = guards()
assert set(GUARDS) == {"walker_update_state"}, GUARDS


def offsets(name):
    got = re.search(r"Offsets " + name + r"\s*=\s*\{([^}]*)\}", walker).group(1)
    return tuple(int(v, 0) for v in got.split(","))


LAYOUT = {"modtools": offsets("kModtools"), "release": offsets("kRelease")}

# Per build: UpdateState's one RET, the vtable slots that hold it (modtools
# through a thunk), its call of DoFootImpactEffects, and the constant.
facts = {
    "modtools": dict(ret=0x0055C44D, slots=(0x00A421F8, 0x00A57FF0), thunk=0x004022B1, call=0x0055BC98,
                     constant=0x00A2C074),
    "steam": dict(ret=0x005038FB, slots=(0x007987B8, 0x0079DC78), thunk=None, call=0x00503214,
                  constant=0x007B1F60),
    "gog": dict(ret=0x005038FB, slots=(0x00799758, 0x0079EC18), thunk=None, call=0x00503214,
                constant=0x007B2ED8),
}


def code_sites(build, layout):
    """(address, [(mnemonic, operands)]); {c} is this build's 0.1, {fn} DoFootImpactEffects."""
    foot_state, timer = hex(layout[3]), hex(layout[4])
    if build == "modtools":
        return {
            # UpdateState: 0x3C of locals and four pushes put its first argument
            # at [esp + 0x50], which it adds to m_fGroundedTimer: the update's length.
            0x0055B3C0: [("sub", "esp, 0x3c")],
            0x0055B3D1: [("push", "ebx"), ("push", "ebp")],
            0x0055B3D7: [("push", "esi")],
            0x0055B3DA: [("push", "edi"), ("mov", "esi, ecx")],
            0x0055B575: [("fld", "dword ptr [esp + 0x50]"), ("fadd", f"dword ptr [esi + {timer}]"),
                         ("fstp", f"dword ptr [esi + {timer}]")],
            0x0055BC96: [("mov", "ecx, esi"), ("call", "0x40c329")],
            # DoFootImpactEffects: one compare of (last - now) with 0.1 serves both
            # tests. A disarmed foot (its bit 24 + i set) is re-armed past it; an
            # armed one lands under it.
            0x00555D0F: [("fcomp", "dword ptr [{c}]"), ("mov", f"ecx, dword ptr [esi + {foot_state}]"),
                         ("test", "edx, ecx"), ("fnstsw", "ax"), ("je", "0x555d32"),
                         ("test", "ah, 0x41"), ("jne", "0x555d48"), ("not", "edx"), ("and", "edx, ecx"),
                         ("mov", f"dword ptr [esi + {foot_state}], edx")],
            0x00555D32: [("test", "ah, 5"), ("jp", "0x555d48"), ("or", "ecx, edx"),
                         ("mov", "byte ptr [esp + 0x13], 1"), ("mov", f"dword ptr [esi + {foot_state}], ecx")],
        }
    return {
        # UpdateState: its first argument, [ebp + 8], is added to m_fGroundedTimer.
        0x00502890: [("push", "ebp"), ("mov", "ebp, esp")],
        0x005028BE: [("mov", "edi, ecx")],
        0x005029DA: [("movss", "xmm2, dword ptr [ebp + 8]")],
        0x00502A32: [("movss", f"xmm0, dword ptr [edi + {timer}]"), ("addss", "xmm0, xmm2"),
                     ("movss", f"dword ptr [edi + {timer}], xmm0")],
        0x00503212: [("mov", "ecx, edi"), ("call", "{fn}")],
        # It compares another field with the same constant, which is why the
        # fix moves the operands and leaves the constant.
        0x005028DA: [("comiss", "xmm0, dword ptr [{c}]")],
        # DoFootImpactEffects: (last - now) in XMM0, the foot's bit 24 + i in EAX.
        # Disarmed: re-armed past 0.1. Armed: lands under it.
        0x0050087C: [("movss", "xmm0, dword ptr [ecx]"), ("subss", "xmm0, xmm4"), ("test", "edx, eax"),
                     ("je", "0x50089d"), ("comiss", "xmm0, dword ptr [{c}]"), ("jbe", "0x5008bb"),
                     ("not", "eax"), ("and", "eax, edx"), ("mov", f"dword ptr [edi + {foot_state}], eax")],
        0x0050089D: [("movss", "xmm1, dword ptr [{c}]"), ("comiss", "xmm1, xmm0"), ("jbe", "0x5008bb"),
                     ("or", "eax, edx"), ("mov", "byte ptr [ebp - 1], 1"),
                     ("mov", f"dword ptr [edi + {foot_state}], eax")],
    }


builds = [
    ("modtools", "BF2_modtools_NoDVD_NoConsole.exe"),
    ("steam", "BattlefrontII.exe"),
    ("gog", "BattlefrontII_GoG.exe"),
]
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

for build, filename in builds:
    body = re.search(r"namespace " + build + r"\s*\{(.*?)\n\s*\}\s*//\s*namespace " + build,
                     addresses, re.S).group(1)
    table = {name: int(value, 16) for name, value in
             re.findall(r"constexpr uintptr_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;", body)}
    kind = "modtools" if build == "modtools" else "release"
    f = facts[build]
    pe = pefile.PE(str(Path(sys.argv[1]) / filename), fast_load=True)
    try:
        image = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        assert base == 0x400000

        def at(va, n):
            return image[va - base:va - base + n]

        def dword(va):
            return struct.unpack("<I", at(va, 4))[0]

        update = table["walker_update_state"]
        foot_fx = table["walker_do_foot_impact_effects"]

        # The prologue the install checks.
        raw, mask = GUARDS["walker_update_state"][kind]
        want = codecs.decode(raw, "unicode_escape").encode("latin1")
        assert len(want) == len(mask)
        got = at(update, len(want))
        assert all(m != "x" or g == w for g, w, m in zip(got, want, mask)), (build, got.hex())

        # One exit, RET 0x14: five stack arguments.
        rets = [(x.address, x.op_str) for x in decoder.disasm(at(update, f["ret"] + 3 - update), update)
                if x.mnemonic == "ret"]
        assert rets == [(f["ret"], "0x14")], (build, rets)

        # Reached through the walker vtables.
        target = f["thunk"] or update
        for slot in f["slots"]:
            assert dword(slot) == target, (build, hex(slot))
        if f["thunk"]:
            jmp = list(decoder.disasm(at(f["thunk"], 5), f["thunk"], count=1))[0]
            assert (jmp.mnemonic, jmp.op_str) == ("jmp", hex(update)), (build, "thunk")
        assert update < f["call"] < f["ret"], (build, "the foot check is called from UpdateState")

        # Retail adds XMM2 to the timer, loaded from its first argument, [ebp + 8],
        # just before (0x005029DA). Nothing writes XMM2 between the load and the
        # add, and nothing writes the argument before the load.
        if build != "modtools":
            def writes(start, end, dest):
                code = decoder.disasm(at(start, end - start), start)
                return [hex(x.address) for x in code if x.op_str.startswith(dest)]
            assert writes(0x005029DF, 0x00502A3A, "xmm2,") == [], (build, "XMM2 rewritten")
            assert writes(0x00502890, 0x005029DA, "dword ptr [ebp + 8],") == [], (build, "argument rewritten")

        # The compares the fix moves: the bytes, inside DoFootImpactEffects, on 0.1.
        sites = [table["walker_stomp_drop_site"]] + ([table["walker_stomp_drop_site2"]] if build != "modtools" else [])
        assert len(sites) == len(compares[build])
        for site, expect in zip(sites, compares[build]):
            assert at(site, len(expect)) == expect, (build, hex(site), at(site, len(expect)).hex())
            assert struct.unpack("<I", expect[-4:])[0] == f["constant"], (build, "constant")
            assert foot_fx < site < foot_fx + 0x500, (build, hex(site), "outside DoFootImpactEffects")
        assert struct.unpack("<f", at(f["constant"], 4))[0] == struct.unpack("<f", struct.pack("<f", 0.1))[0]

        names = {"c": hex(f["constant"]), "fn": hex(foot_fx)}
        checks = code_sites(build, LAYOUT[kind])
        for address, instructions in checks.items():
            want_ins = [(m, ops.format(**names)) for m, ops in instructions]
            got_ins = [(x.mnemonic, x.op_str) for x in decoder.disasm(at(address, 64), address, count=len(want_ins))]
            assert got_ins == want_ins, (build, hex(address), got_ins, want_ins)

        print(f"{build}: UpdateState's prologue, RET 0x14, its length argument and {len(f['slots'])} vtable "
              f"slots, {len(sites)} compare(s) on 0.1 and {len(checks)} code sites passed")
    finally:
        pe.close()
