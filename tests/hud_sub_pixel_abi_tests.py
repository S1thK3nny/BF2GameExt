"""Read-only audit of the sub-pixel interface patch against all three game PEs.

Usage: python tests/hud_sub_pixel_abi_tests.py PATH_TO_GAMEDATA
Requires pefile. Reads the addresses from game_addrs.hpp and the install guard
and site offsets from the module, and checks them, and the instructions the
write-up rests on, against each executable. Never loads or executes the game.
Not an in-game behaviour test.
"""
import codecs
import re
import struct
import sys
from pathlib import Path

import pefile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "PatcherDLL/src/render/hud_sub_pixel.cpp").read_text()
addresses = (ROOT / "PatcherDLL/src/core/game_addrs.hpp").read_text()


def const(name):
    match = re.search(r"\b" + name + r"\s*=\s*(-?0x[0-9a-fA-F]+)", source)
    assert match, name
    return int(match.group(1), 16)


half_offsets = {
    "modtools": (const("kHalfXModtools"), const("kHalfYModtools")),
    "retail": (const("kHalfXRetail"), const("kHalfYRetail")),
}
assert half_offsets == {"modtools": (-0x1A, -0x07), "retail": (-0x12, -0x15)}

LITERALS = r'((?:"[^"]*"\s*)+)'
guard = re.search(r'guard\(base, g_addr->hud_element_draw, "[^"]*",\s*'
                  r'modtools \? ' + LITERALS + r':\s*' + LITERALS + r',\s*'
                  r'modtools \? ' + LITERALS + r':\s*' + LITERALS + r'\)', source)
assert guard, "Update the audit if the install guard changes shape"


def joined(literals):
    text = "".join(re.findall(r'"([^"]*)"', literals))
    return codecs.decode(text, "unicode_escape").encode("latin1")


guard_bytes = {"modtools": joined(guard.group(1)), "retail": joined(guard.group(2))}
guard_mask = {"modtools": joined(guard.group(3)).decode(), "retail": joined(guard.group(4)).decode()}
for kind in guard_bytes:
    assert len(guard_bytes[kind]) == len(guard_mask[kind]), kind


def b(text):
    return bytes.fromhex(text.replace(" ", ""))


# Each read off the disassembly before recording. The draw tests the enabled
# flag, multiplies the local matrix (+0x30) by the parent's, loads the world
# translation's x and y, stores them back rounded, draws through vtable +0x40
# with that matrix and returns with RET 8. The group draw tests PropagateAlpha
# (+0x84 bit 0) and draws each child through the same function; the screen
# draws its top-level groups through it.
sites = {
    "modtools": [
        (0x00816FAF, "8B 43 14"),                    # MOV EAX,[EBX+0x14]  flags
        (0x00816FB2, "F6 C4 01"),                    # TEST AH,1           enabled (0x100)
        (0x00816FC1, "8D 4B 30"),                    # LEA ECX,[EBX+0x30]  local matrix
        (0x00816FDE, "D9 44 24 50"),                 # FLD [ESP+0x50]      world x
        (0x00816FEC, "8B 4C 24 54"),                 # MOV ECX,[ESP+0x54]  world y
        (0x00817029, "89 54 24 50"),                 # MOV [ESP+0x50],EDX  rounded x
        (0x00817032, "D9 5C 24 58"),                 # FSTP [ESP+0x58]     rounded y (ESP one push down)
        (0x00817053, "FF 56 40"),                    # CALL [ESI+0x40]     RenderUsingContext
        (0x0081706D, "C2 08 00"),                    # RET 8
        (0x00838BF4, "F6 85 84 00 00 00 01"),        # group draw: TEST [EBP+0x84],1
    ],
    "steam": [
        (0x006C0E0A, "F7 47 14 00 01 00 00"),        # TEST [EDI+0x14],0x100
        (0x006C0E18, "8D 47 30"),                    # LEA EAX,[EDI+0x30]
        (0x006C0E1C, "8D 45 B0"),                    # LEA EAX,[EBP-0x50]  world matrix
        (0x006C0E25, "F3 0F 10 45 E0"),              # MOVSS XMM0,[EBP-0x20]  world x
        (0x006C0E48, "F3 0F 10 45 E4"),              # MOVSS XMM0,[EBP-0x1C]  world y
        (0x006C0E55, "D9 5D E0"),                    # FSTP [EBP-0x20]     rounded x
        (0x006C0EAC, "D9 5D E4"),                    # FSTP [EBP-0x1C]     rounded y
        (0x006C0E94, "8D 4D B0"),                    # LEA ECX,[EBP-0x50]  passed on
        (0x006C0F3A, "FF 50 40"),                    # CALL [EAX+0x40]
        (0x006C0F5F, "C2 08 00"),                    # RET 8
        (0x006D6B50, "F6 82 84 00 00 00 01"),        # group draw: TEST [EDX+0x84],1
    ],
    "gog": [
        (0x006C1E9A, "F7 47 14 00 01 00 00"),
        (0x006C1EA8, "8D 47 30"),
        (0x006C1EAC, "8D 45 B0"),
        (0x006C1EB5, "F3 0F 10 45 E0"),
        (0x006C1ED8, "F3 0F 10 45 E4"),
        (0x006C1EE5, "D9 5D E0"),
        (0x006C1F3C, "D9 5D E4"),
        (0x006C1F24, "8D 4D B0"),
        (0x006C1FCA, "FF 50 40"),
        (0x006C1FEF, "C2 08 00"),
        (0x006D7BF0, "F6 82 84 00 00 00 01"),
    ],
}

# CALLs of the draw: the screen's top-level groups, then the group draw's two
# per-child calls (with and without PropagateAlpha).
callers = {
    "modtools": (0x00817CB8, 0x00838C34, 0x00838C4B),
    "steam": (0x006C142F, 0x006D6B9E, 0x006D6BB5),
    "gog": (0x006C24BF, 0x006D7C3E, 0x006D7C55),
}

builds = [
    ("modtools", "BF2_modtools_NoDVD_NoConsole.exe"),
    ("steam", "BattlefrontII.exe"),
    ("gog", "BattlefrontII_GoG.exe"),
]
for build, filename in builds:
    body = re.search(r"namespace " + build + r"\s*\{(.*?)\n\s*\}\s*//\s*namespace " + build,
                     addresses, re.S).group(1)
    table = {name: int(value, 16) for name, value in
             re.findall(r"constexpr uintptr_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;", body)}
    mt = build == "modtools"
    pe = pefile.PE(str(Path(sys.argv[1]) / filename), fast_load=mt)
    try:
        image = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        assert base == 0x400000

        def at(va, n):
            return image[va - base:va - base + n]

        def call_target(va):
            assert at(va, 1) == b"\xE8", (build, hex(va), at(va, 5).hex())
            return va + 5 + struct.unpack("<i", at(va + 1, 4))[0]

        kind = "modtools" if mt else "retail"
        draw = table["hud_element_draw"]
        floor = table["crt_floor"]
        actual = at(draw, len(guard_mask[kind]))
        assert all(m != "x" or a == e for a, e, m in zip(actual, guard_bytes[kind], guard_mask[kind])), \
            (build, "guard", actual.hex())

        # Each site: FSTP qword [esp] / CALL floor, after the add of a 0.5.
        for site, offset in zip((table["hud_element_draw_floor_x"], table["hud_element_draw_floor_y"]),
                                half_offsets[kind]):
            assert draw < site < draw + 0x100, (build, hex(site))
            assert at(site - 3, 3) == b"\xDD\x1C\x24", (build, hex(site))
            assert call_target(site) == floor, (build, hex(site))
            opcode = b"\xD8\x05" if mt else b"\xF3\x0F\x58\x05"
            assert at(site + offset - len(opcode), len(opcode)) == opcode, (build, hex(site))
            operand = struct.unpack("<I", at(site + offset, 4))[0]
            assert struct.unpack("<f", at(operand, 4))[0] == 0.5, (build, hex(site), hex(operand))

        # floor itself: the CRT's on modtools (its SSE2 dispatch first), the
        # MSVCR120 import thunk on retail.
        if mt:
            head = at(floor, 13)
            assert head[:2] == b"\x83\x3D" and head[6:9] == b"\x00\x0F\x84", head.hex()
        else:
            assert at(floor, 2) == b"\xFF\x25", at(floor, 6).hex()
            iat = struct.unpack("<I", at(floor + 2, 4))[0]
            names = [(entry.dll, imp.name) for entry in pe.DIRECTORY_ENTRY_IMPORT
                     for imp in entry.imports if imp.address == iat]
            assert names == [(b"MSVCR120.dll", b"floor")], names

        for va, text in sites[build]:
            want = b(text)
            assert at(va, len(want)) == want, (build, hex(va), at(va, len(want)).hex())
        for va in callers[build]:
            assert call_target(va) == draw, (build, hex(va))
        print(f"{build}: guard, 2 floor sites, floor, {len(sites[build])} draw sites and "
              f"{len(callers[build])} callers passed")
    finally:
        pe.close()
