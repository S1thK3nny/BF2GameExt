"""Read-only audit of the BarBitmap FillFrom hooks against all three game PEs.

Usage: python tests/hud_bar_fill_from_abi_tests.py PATH_TO_GAMEDATA
Requires pefile. Reads the install guards, addresses and offsets from the
production sources and checks them, and the instructions the offsets were read
from, against each executable. Never loads or executes the game. Not an in-game
behaviour test.
"""
import codecs
import re
import sys
from pathlib import Path

import pefile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "PatcherDLL/src/render/hud_bar_fill_from.cpp").read_text()
addresses = (ROOT / "PatcherDLL/src/core/game_addrs.hpp").read_text()


def const(name):
    match = re.search(r"\b" + name + r"\s*=\s*(0x[0-9a-fA-F]+)", source)
    assert match, name
    return int(match.group(1), 16)


assert const("kBitmap") == 0xB0
assert const("kBarWidth") == 0x47C
assert const("kBarU1") == 0x480
assert const("kIncFade") == 0x474
assert const("kDecFade") == 0x478
assert const("kVt_SetRect") == 0x4C

guards = re.findall(r'guard\(base, g_addr->(\w+), "[^"]*",\s*'
                    r'modtools \? "([^"]*)"\s*: "([^"]*)",\s*'
                    r'modtools \? "([^"]*)"\s*: "([^"]*)"\)', source)
assert len(guards) == 5, "Update the audit if the install guards change shape"


def b(text):
    return bytes.fromhex(text.replace(" ", ""))


# Each site was disassembled before recording: the bitmap pointer read and the
# width/U stores in PostReadSetup, SetValue's SetRect call, ReadData's fade stores.
sites = {
    "modtools": [
        (0x00696357, "8B BE B0 00 00 00"),          # MOV EDI,[ESI+0xB0]
        (0x00696385, "8D 96 80 04 00 00"),          # LEA EDX,[ESI+0x480]  mBarU1
        (0x0069638C, "D9 9E 7C 04 00 00"),          # FSTP [ESI+0x47C]     mBarWidth
        (0x0069618E, "FF 52 4C"),                   # CALL [EDX+0x4C]      SetRect
        (0x0069594A, "D9 9F 74 04 00 00"),          # FSTP [EDI+0x474]     Inc fade
        (0x00695933, "D9 9F 78 04 00 00"),          # FSTP [EDI+0x478]     Dec fade
    ],
    "steam": [
        (0x0054B33A, "8B B7 B0 00 00 00"),
        (0x0054B365, "8D 87 80 04 00 00"),
        (0x0054B375, "F3 0F 11 87 7C 04 00 00"),
        (0x0054B1C3, "FF 50 4C"),
        (0x0054B4B7, "89 86 74 04 00 00"),
        (0x0054B4A6, "89 86 78 04 00 00"),
    ],
    "gog": [
        (0x0054C08A, "8B B7 B0 00 00 00"),
        (0x0054C0B5, "8D 87 80 04 00 00"),
        (0x0054C0C5, "F3 0F 11 87 7C 04 00 00"),
        (0x0054BF13, "FF 50 4C"),
        (0x0054C207, "89 86 74 04 00 00"),
        (0x0054C1F6, "89 86 78 04 00 00"),
    ],
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
    pe = pefile.PE(str(Path(sys.argv[1]) / filename))
    try:
        image = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        assert base == 0x400000
        mt = build == "modtools"
        for name, mt_bytes, rt_bytes, mt_mask, rt_mask in guards:
            expected = codecs.decode(mt_bytes if mt else rt_bytes, "unicode_escape").encode("latin1")
            mask = mt_mask if mt else rt_mask
            address = table[name]
            actual = image[address - base:address - base + len(mask)]
            assert len(expected) == len(mask), (build, name)
            assert all(m != "x" or a == e for a, e, m in zip(actual, expected, mask)), \
                (build, name, hex(address), actual.hex())
        for address, text in sites[build]:
            want = b(text)
            got = image[address - base:address - base + len(want)]
            assert got == want, (build, hex(address), got.hex())
        print(f"{build}: {len(guards)} guards and {len(sites[build])} offset sites passed")
    finally:
        pe.close()
