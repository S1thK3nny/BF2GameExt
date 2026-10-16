"""Read-only audit of everything flyer-to-flyer avoidance reads (modtools only).

Usage: python tests/flyer_avoidance_abi_tests.py PATH_TO_GAMEDATA
Requires pefile and capstone. Does not execute the game or modify its files.
These are instruction/layout regression checks, not an in-game behaviour test.
"""
import re
import struct
import sys
from pathlib import Path

import capstone
import pefile


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "PatcherDLL/src"
source = (SRC / "ai/flyer_avoidance.cpp").read_text()
wingmen = (SRC / "ai/flyer_wingmen.cpp").read_text()
real = (SRC / "ai/flyer_real_flight.cpp").read_text()
addrs = (SRC / "core/game_addrs.hpp").read_text()


def constant(text, name):
    match = re.search(r"\b" + name + r"\s*=\s*(-?0x[0-9a-fA-F]+|-?\d+)\s*;", text)
    assert match, name
    return int(match.group(1), 0)


for name, value in {
    "kFly_Follower": 0x3CC,
    "kFly_State": 0x5A4,
    "kFly_SphereStack": 0x10,
    "kFly_SphereIndex": 0x14,
    "kFly_SphereRadius": 0x24,
    "kStack_Radius": 0x4C,
    "kList_Next": 0x04,
    "kList_Follower": 0x0C,
    "kList_Count": 0x10,
    "kStateTakeoff": 1,
    "kStateLanding": 3,
    "kDest_PosY": 0x08,
}.items():
    assert constant(source, name) == value, name
# The reads it shares with the wingmen (tests/flyer_wingmen_abi_tests.py audits those).
for name in ("kVt_GetVelocity", "kGO_MatrixIface", "kVt_GetMatrix", "kGO_Flags", "kFly_State"):
    assert constant(source, name) == constant(wingmen, name), name
assert constant(source, "kFly_Follower") == constant(real, "kFly_Follower")
assert constant(source, "kDest_PosY") == constant(wingmen, "kDest_Pos") + 4
# The CalculateDest stand-in asks it after the original and after its own steering is put back.
calc = re.search(r"bool __fastcall calc_dest_mt\(.*?\n\}", wingmen, re.S).group(0)
assert calc.index("s_calcDest)(flyer, dt, dest, point, speed, myPos)") < calc.index("flyer_avoidance::adjust(flyer")
assert calc.index("steer_after(flyer, st, r, point, speed)") < calc.index("flyer_avoidance::adjust(flyer")
assert "if (r && flyer_avoidance::active())" in calc
assert "flyer_avoidance::install(base)" in wingmen and "flyer_avoidance::uninstall()" in wingmen

mt = re.search(r"namespace modtools\s*\{(.*?)\n\s*\}\s*//\s*namespace modtools", addrs, re.S).group(1)
assert constant(mt, "flyer_follower_list") == 0x00AD3524
assert constant(mt, "flyer_follower_link") == 0x005E8AE3
assert constant(mt, "flyer_raise_ray_call") == 0x004F429E

checks = {
    # EntityPathFollower's constructor: its node at +0x34 ...
    0x005E89F0: [("push", "ebx"), ("push", "esi"), ("push", "edi"), ("mov", "esi, ecx")],
    0x005E8A08: [("lea", "edi, [esi + 0x34]")],
    # ... linked in at the list's tail: the count (head +0x10) up, the node's +0xC its
    # follower, +0 the head, +4 the head (the next), +8 the old tail.
    0x005E8AE3: [("inc", "dword ptr [0xad3534]"), ("mov", "dword ptr [edi + 0xc], esi"),
                 ("mov", "dword ptr [edi], 0xad3524"), ("mov", "dword ptr [edi + 4], 0xad3524"),
                 ("mov", "eax, dword ptr [0xad352c]"), ("mov", "dword ptr [edi + 8], eax"),
                 ("mov", "dword ptr [0xad352c], edi"), ("mov", "ecx, dword ptr [edi + 8]"),
                 ("mov", "dword ptr [ecx + 4], edi")],
    # Only EntityFlyer's constructor builds one (through the thunk 0x0040E42B), at its +0x3CC.
    0x0040E42B: [("jmp", "0x5e89f0")],
    0x0050381F: [("lea", "ebx, [esi + 0x3cc]"), ("push", "edx"), ("mov", "ecx, ebx"), ("call", "0x40e42b")],
    # The game walks it the same way: from [head + 4], each node's +0xC its follower, +4 the next.
    0x005E9012: [("mov", "esi, dword ptr [0xad3528]"), ("mov", "ecx, dword ptr [esi + 0xc]")],
    # A flyer's states (EntityFlyer::Land): 1 taking off, 2 flying, 3 landing.
    0x004F1380: [("push", "esi"), ("mov", "esi, ecx"), ("mov", "eax, dword ptr [esi + 0x5a4]"), ("cmp", "eax, 2"),
                 ("push", "edi"), ("je", "0x4f1399"), ("cmp", "eax, 1"), ("je", "0x4f1399"), ("cmp", "eax, 3"),
                 ("jne", "0x4f13a3"), ("mov", "dword ptr [esi + 0x5a4], 3")],
    # A flyer's collision sphere, as RaiseHeight reads its own (EDI the flyer): the stack +0x10
    # (the radius at stack + index * 16 + 0x4C, the index at +0x14), else +0x24.
    0x004F4259: [("mov", "eax, dword ptr [edi + 0x10]"), ("test", "eax, eax"), ("je", "0x4f426c"),
                 ("mov", "ecx, dword ptr [edi + 0x14]"), ("shl", "ecx, 4"), ("fld", "dword ptr [ecx + eax + 0x4c]"),
                 ("jmp", "0x4f426f"), ("fld", "dword ptr [edi + 0x24]")],
    0x004F40A6: [("mov", "edi, ecx")],
    # UpdateIndirect calls CalculateDest only for a flyer flying (2), not taking off (1).
    0x004F4D01: [("cmp", "dword ptr [esi + 0x364], 1")],
    # CalculateDest copies mDest's mPos into its dest (+4), then steers at it: point = dest mPos.
    0x004F48A1: [("push", "ebx"), ("mov", "ecx, ebp"), ("call", "0x409827")],
}

decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
pe = pefile.PE(str(Path(sys.argv[1]) / "BF2_modtools_NoDVD_NoConsole.exe"))
image = pe.get_memory_mapped_image()
base = pe.OPTIONAL_HEADER.ImageBase
assert base == 0x400000
for address, expected in checks.items():
    data = image[address - base:address - base + 96]
    actual = [(i.mnemonic, i.op_str) for i in decoder.disasm(data, address, count=len(expected))]
    assert actual == expected, (hex(address), actual, expected)


def dword(va):
    return int.from_bytes(image[va - base:va - base + 4], "little")


# The head as the exe starts: empty, its next and back its own address, no follower, none counted.
assert [dword(0x00AD3524 + 4 * k) for k in range(5)] == [0x00AD3524, 0x00AD3524, 0x00AD3524, 0, 0]
# The constructor is reached through that one thunk, called once.
callers = [i for i in range(0x1000, 0x00A2A000 - base)
           if image[i] in (0xE8, 0xE9) and base + i + 5 + struct.unpack_from("<i", image, i + 1)[0] == 0x005E89F0]
assert [base + i for i in callers] == [0x0040E42B], [hex(base + i) for i in callers]
thunk_callers = [i for i in range(0x1000, 0x00A2A000 - base)
                 if image[i] in (0xE8, 0xE9) and base + i + 5 + struct.unpack_from("<i", image, i + 1)[0] == 0x0040E42B]
assert [base + i for i in thunk_callers] == [0x00503828], [hex(base + i) for i in thunk_callers]
# The guards' bytes are the code's: the link-in as install builds it, RaiseHeight's sphere reads.
link = bytes.fromhex("FF05 3435AD00 89770C C707 2435AD00 C74704 2435AD00".replace(" ", ""))
assert image[0x005E8AE3 - base:0x005E8AE3 - base + len(link)] == link
assert "uint8_t linkIn[22] = {0xFF, 0x05, 0, 0, 0, 0, 0x89, 0x77, 0x0C, 0xC7, 0x07, 0, 0, 0, 0, 0xC7, 0x47, 0x04" in source
sphere = bytes.fromhex("8B4710 85C0 740C 8B4F14 C1E104 D944014C EB03 D94724".replace(" ", ""))
assert image[0x004F4259 - base:0x004F4259 - base + len(sphere)] == sphere
assert "0x8B, 0x47, 0x10, 0x85, 0xC0, 0x74, 0x0C, 0x8B, 0x4F, 0x14, 0xC1," in source
assert "resolve(base, a->flyer_raise_ray_call) - 0x45" in source and 0x004F429E - 0x004F4259 == 0x45
print(f"modtools: {len(checks)} instruction checks, the list head, its one builder and the guards passed")
