"""Read-only audit of the walker foot diagnostic against all three game PEs.

Usage: python tests/walker_foot_diag_abi_tests.py PATH_TO_GAMEDATA
Requires pefile and capstone. Reads the walker layout (core/layout/walker.hpp),
the addresses (game_addrs.hpp) and the prologues the install checks
(entity/walker_foot_diag.cpp), and checks them, and the instructions the
diagnostic's reading of BF2's foot records rests on, against each executable.
Never loads or executes the game. Not an in-game behaviour test.
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
module = (SRC / "entity" / "walker_foot_diag.cpp").read_text()
walker = (SRC / "core" / "layout" / "walker.hpp").read_text()
character = (SRC / "core" / "layout" / "character.hpp").read_text()
addresses = (SRC / "core" / "game_addrs.hpp").read_text()

# The constants the reading rests on: mMinFootHeight after mLastFootHeight, the
# type 1 disarm bits, its 0.1, six feet at most; the player's vehicle slot and
# the Trackable call that turns it into the walker, as camera_shake.cpp makes it.
assert re.search(r"kMinFootHeight\s*=\s*0x18;", walker)
assert re.search(r"kFootDownBit\s*=\s*24;", walker)
assert re.search(r"kStompDrop\s*=\s*0\.1f;", walker)
assert re.search(r"kMaxFeet\s*=\s*6;", walker)
assert re.search(r"kVehicle\s*=\s*0x14C;", character)
assert "kCtrl_Trackable   = 0x18" in module and "kVt_GetGameObject = 0x1C" in module

layouts = {}
for name in ("kModtools", "kRelease"):
    got = re.search(r"Offsets " + name + r"\s*=\s*\{([^}]*)\}", walker).group(1)
    layouts[name] = tuple(int(v, 0) for v in got.split(","))
    assert len(layouts[name]) == 12, name


def guards():
    """Every guard(...) in the module: (address name, {build kind: (bytes, mask)})."""
    out = {}
    for m in re.finditer(r'guard\(base, g_addr->(\w+), "[^"]*",\s*(.*?)\)(?: \|\||\))', module, re.S):
        pair = re.match(r'modtools \? ((?:"[^"]*"\s*)+):\s*((?:"[^"]*"\s*)+),\s*'
                        r'modtools \? "([^"]*)" : "([^"]*)"', m.group(2))
        assert pair, "Update the audit if a guard changes shape: " + m.group(1)
        mt = "".join(re.findall(r'"([^"]*)"', pair.group(1)))
        rt = "".join(re.findall(r'"([^"]*)"', pair.group(2)))
        out[m.group(1)] = {"modtools": (mt, pair.group(3)), "release": (rt, pair.group(4))}
    return out


GUARDS = guards()
assert set(GUARDS) == {"walker_do_foot_impact_effects", "net_game_get_local_player"}, GUARDS

# The 0.1 type 1 tests a foot's drop against, by build.
one_tenth = {"modtools": 0x00A2C074, "steam": 0x007B1F60, "gog": 0x007B2ED8}


def sites(build, o):
    """(address, [(mnemonic, operands)]) the diagnostic's reading rests on; {fn} is the hooked function."""
    cls, foot_state, num_feet, height, stomp_type, threshold = o[0], o[3], o[8], o[9], o[10], o[11]
    tenth = hex(one_tenth[build])
    if build == "modtools":
        return {
            # UpdateState calls it for the walker (ESI there), through the thunk.
            0x0055BC96: [("mov", "ecx, esi"), ("call", "0x40c329")],
            0x0040C329: [("jmp", "{fn}")],
            # Its this, the class's mNumFeet, and the foot records' base kept
            # at [esp + 0x18] for the loop.
            0x00555B6E: [("mov", "esi, ecx"), ("mov", f"eax, dword ptr [esi + {hex(cls)}]"),
                         ("movzx", f"eax, byte ptr [eax + {hex(num_feet)}]")],
            0x00555B91: [("lea", f"ebx, [esi + {hex(height)}]")],
            0x00555BA1: [("mov", "dword ptr [esp + 0x18], ebx")],
            0x00555BAB: [("mov", "ebx, dword ptr [esp + 0x18]")],
            # StompDetectionType; type 0: the new height under the last, then
            # under the lowest (+0x18) plus StompThreshold.
            0x00555CBA: [("mov", f"al, byte ptr [ecx + {hex(stomp_type)}]"), ("test", "al, al")],
            0x00555CC8: [("fld", "dword ptr [esp + 0xc]"), ("fcomp", "dword ptr [ebx]")],
            0x00555CD5: [("fld", f"dword ptr [ecx + {hex(threshold)}]"), ("fadd", "dword ptr [ebx + 0x18]")],
            # Type 1: the last height less the new one against 0.1, and the
            # foot's disarm bit, 24 + its number, in mFootState.
            0x00555CFB: [("fld", "dword ptr [ebx]"), ("mov", "ecx, dword ptr [esp + 0x14]"),
                         ("fsub", "dword ptr [esp + 0xc]"), ("add", "ecx, 0x18"), ("mov", "edx, 1"),
                         ("shl", "edx, cl"), ("fcomp", f"dword ptr [{tenth}]"),
                         ("mov", f"ecx, dword ptr [esi + {hex(foot_state)}]")],
            # The lowest kept, and the new height stored as the last.
            0x00555D48: [("fld", "dword ptr [esp + 0xc]"), ("fcomp", "dword ptr [ebx + 0x18]")],
            0x00555D56: [("mov", "edx, dword ptr [esp + 0xc]"), ("mov", "dword ptr [ebx + 0x18], edx")],
            0x00555F31: [("mov", "ecx, dword ptr [esp + 0xc]"), ("mov", "edx, dword ptr [esp + 0x18]"),
                         ("mov", "dword ptr [edx], ecx")],
            # The next foot's records, four bytes on.
            0x005560C3: [("mov", "edx, dword ptr [esp + 0x18]"), ("mov", "ecx, dword ptr [esp + 0x1c]"),
                         ("mov", "eax, dword ptr [esp + 0x5c]"), ("inc", "edi"), ("add", "edx, 4")],
            0x005560F0: [("ret", "")],
        }
    return {
        # UpdateState calls it for the walker (EDI there).
        0x00503212: [("mov", "ecx, edi"), ("call", "{fn}")],
        0x0050072E: [("mov", "edi, ecx"), ("mov", f"eax, dword ptr [edi + {hex(cls)}]"),
                     ("movzx", f"eax, byte ptr [eax + {hex(num_feet)}]")],
        0x00500754: [("lea", f"eax, [edi + {hex(height)}]"), ("mov", "dword ptr [ebp - 0xc], 1"),
                     ("sub", "ecx, edi"), ("mov", "dword ptr [ebp - 8], eax")],
        0x00500831: [("cmp", f"byte ptr [eax + {hex(stomp_type)}], 0")],
        0x0050083F: [("mov", "esi, dword ptr [ebp - 8]"), ("movss", "xmm1, dword ptr [esi]"),
                     ("comiss", "xmm1, xmm4")],
        0x0050084B: [("movss", f"xmm0, dword ptr [eax + {hex(threshold)}]"),
                     ("addss", "xmm0, dword ptr [esi + 0x18]")],
        0x00500873: [("mov", f"edx, dword ptr [edi + {hex(foot_state)}]"), ("rol", "eax, 0x18"),
                     ("movss", "xmm0, dword ptr [ecx]"), ("subss", "xmm0, xmm4"), ("test", "edx, eax")],
        0x00500888: [("comiss", f"xmm0, dword ptr [{tenth}]")],
        0x0050089D: [("movss", f"xmm1, dword ptr [{tenth}]"), ("comiss", "xmm1, xmm0")],
        0x005008BB: [("mov", "eax, dword ptr [ebp - 8]"), ("movss", "xmm0, dword ptr [eax + 0x18]")],
        0x00500A50: [("mov", "ecx, dword ptr [ebp - 8]"), ("movss", "xmm0, dword ptr [ebp - 0x14]"),
                     ("movss", "dword ptr [ecx], xmm0")],
        0x00500B53: [("mov", "eax, dword ptr [ebp - 8]"), ("inc", "ecx"), ("add", "eax, 4")],
        0x00500B79: [("ret", "")],
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
    layout = layouts["kModtools" if build == "modtools" else "kRelease"]
    pe = pefile.PE(str(Path(sys.argv[1]) / filename), fast_load=True)
    try:
        image = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        assert base == 0x400000

        def at(va, n):
            return image[va - base:va - base + n]

        for name, per_kind in GUARDS.items():
            raw, mask = per_kind[kind]
            want = codecs.decode(raw, "unicode_escape").encode("latin1")
            assert len(want) == len(mask), (build, name, "guard length")
            got = at(table[name], len(want))
            assert all(m != "x" or g == w for g, w, m in zip(got, want, mask)), (build, name, got.hex())

        fn = table["walker_do_foot_impact_effects"]
        for address, instructions in sites(build, layout).items():
            want_ins = [(m, ops.format(fn=hex(fn))) for m, ops in instructions]
            got_ins = [(x.mnemonic, x.op_str) for x in decoder.disasm(at(address, 48), address, count=len(want_ins))]
            assert got_ins == want_ins, (build, hex(address), got_ins, want_ins)
        assert abs(struct.unpack("<f", at(one_tenth[build], 4))[0] - 0.1) < 1e-7, (build, "0.1")

        # One function, one exit: decoded straight through, its only RET is the last.
        end = max(a for a, ins in sites(build, layout).items() if ins == [("ret", "")])
        rets = [x.address for x in decoder.disasm(at(fn, end + 1 - fn), fn) if x.mnemonic == "ret"]
        assert rets == [end], (build, [hex(r) for r in rets])

        # One caller: every CALL in .text that reaches the function, directly or
        # through a JMP thunk, is the one in UpdateState.
        text = [s for s in pe.sections if s.Name.rstrip(b"\0") == b".text"][0]
        t0 = base + text.VirtualAddress
        code = image[text.VirtualAddress:text.VirtualAddress + text.Misc_VirtualSize]

        def branches(opcode, targets):
            out = []
            for m in re.finditer(re.escape(bytes([opcode])), code):
                i = m.start()
                rel = struct.unpack("<i", code[i + 1:i + 5])[0] if i + 5 <= len(code) else None
                if rel is not None and t0 + i + 5 + rel in targets:
                    out.append(t0 + i)
            return out

        reach = {fn} | set(branches(0xE9, {fn}))
        callers = branches(0xE8, reach)
        expected_call = 0x0055BC98 if build == "modtools" else 0x00503214
        assert callers == [expected_call], (build, [hex(c) for c in callers])
        print(f"{build}: {len(GUARDS)} guards, {len(sites(build, layout))} sites, the 0.1, one RET "
              f"and one caller passed")
    finally:
        pe.close()
