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
assert const("kFlags") == 0x484
assert const("kBarBase") == 0x220
assert const("kFlagScaleTexture") == 0x01
assert const("kFlagScaleRect") == 0x02

guards = re.findall(r'guard\(base, g_addr->(\w+), "[^"]*",\s*'
                    r'modtools \? "([^"]*)"\s*: "([^"]*)",\s*'
                    r'modtools \? "([^"]*)"\s*: "([^"]*)"\)', source)
assert len(guards) == 6, "Update the audit if the install guards change shape"


def b(text):
    return bytes.fromhex(text.replace(" ", ""))


# Each site was disassembled before recording: the bitmap pointer read and the
# width/U stores in PostReadSetup, SetValue's SetRect call, ReadData's fade stores,
# and SetValue's contract: its bitmap read through the ElementBar base
# ([this-0x170], so the base is bar + 0x220), mValue at [this+0x1C], the two flag
# bits at [this+0x264] (bar + 0x484) and RET 4.
sites = {
    "modtools": [
        (0x00696357, "8B BE B0 00 00 00"),          # MOV EDI,[ESI+0xB0]
        (0x00696385, "8D 96 80 04 00 00"),          # LEA EDX,[ESI+0x480]  mBarU1
        (0x0069638C, "D9 9E 7C 04 00 00"),          # FSTP [ESI+0x47C]     mBarWidth
        (0x0069618E, "FF 52 4C"),                   # CALL [EDX+0x4C]      SetRect
        (0x0069594A, "D9 9F 74 04 00 00"),          # FSTP [EDI+0x474]     Inc fade
        (0x00695933, "D9 9F 78 04 00 00"),          # FSTP [EDI+0x478]     Dec fade
        (0x0069609B, "8B 46 1C"),                   # MOV EAX,[ESI+0x1C]   mValue
        (0x0069609F, "8B BE 90 FE FF FF"),          # MOV EDI,[ESI-0x170]  bitmap
        (0x006960EB, "F6 86 64 02 00 00 01"),       # TEST [ESI+0x264],1   ScaleTexture
        (0x0069612D, "F6 86 64 02 00 00 02"),       # TEST [ESI+0x264],2   edge moves
        (0x006962AC, "C2 04 00"),                   # RET 4
    ],
    "steam": [
        (0x0054B33A, "8B B7 B0 00 00 00"),
        (0x0054B365, "8D 87 80 04 00 00"),
        (0x0054B375, "F3 0F 11 87 7C 04 00 00"),
        (0x0054B1C3, "FF 50 4C"),
        (0x0054B4B7, "89 86 74 04 00 00"),
        (0x0054B4A6, "89 86 78 04 00 00"),
        (0x0054B07E, "F3 0F 10 47 1C"),
        (0x0054B083, "8B B7 90 FE FF FF"),
        (0x0054B0E9, "F6 87 64 02 00 00 01"),
        (0x0054B147, "F6 87 64 02 00 00 02"),
        (0x0054B316, "C2 04 00"),
    ],
    "gog": [
        (0x0054C08A, "8B B7 B0 00 00 00"),
        (0x0054C0B5, "8D 87 80 04 00 00"),
        (0x0054C0C5, "F3 0F 11 87 7C 04 00 00"),
        (0x0054BF13, "FF 50 4C"),
        (0x0054C207, "89 86 74 04 00 00"),
        (0x0054C1F6, "89 86 78 04 00 00"),
        (0x0054BDCE, "F3 0F 10 47 1C"),
        (0x0054BDD3, "8B B7 90 FE FF FF"),
        (0x0054BE39, "F6 87 64 02 00 00 01"),
        (0x0054BE97, "F6 87 64 02 00 00 02"),
        (0x0054C066, "C2 04 00"),
    ],
}

LITERAL = r'"([^"]*)"'
editor_guards = re.findall(r'matches\(base, a\.(\w+), ' + LITERAL + r', (\d+)\)', source)
assert [g[0] for g in editor_guards] == ["hud_bar_bitmap_write_data", "hud_bar_bitmap_set_property",
                                         "hud_bar_bitmap_get_property", "hud_write_indent", "hud_write_format"]
def in_order(body, steps, what):
    at = 0
    for step in steps:
        found = body.find(step, at)
        assert found >= 0, (what, step)
        at = found + len(step)


# The writer and the getter work on the bar as the file has it and put the
# drawn bar back afterwards; the writer adds the FillFrom line.
writer = source[source.index("void __fastcall hooked_WriteData"):source.index("bool __fastcall hooked_SetProperty")]
in_order(writer, ("const Stored live = read_state(bar, bitmap);", "write_state(bar, bitmap, authored->state);",
                  "s_writeData(self, edx, file, indent);", "s_indent(file, nullptr, indent);",
                  's_format(file, "FillFrom(\\"%s\\")\\n", mode_name(authored->mode));',
                  "write_state(bar, bitmap, live);"), "WriteData")
getter = source[source.index("bool __fastcall hooked_GetProperty"):source.index("bool guard(")]
in_order(getter, ("const Stored live = read_state(bar, bitmap);", "write_state(bar, bitmap, authored->state);",
                  "s_getProperty(self, edx, property, value);", "write_state(bar, bitmap, live);"), "GetProperty")
# The bar as the stock setup left it is kept before FillFrom changes it; an
# editor change is made to that state with the bar's SetValue held off, kept,
# and FillFrom laid out again at the bar's value.
post_read = source[source.index("void __fastcall hooked_PostRead"):source.index("float __fastcall hooked_SetValue")]
kept = post_read.find("const Stored authored = read_state(bar, bitmap);")
laid_out = post_read.find("apply_fill_from(bar, bitmap, mode, authored, false);")
assert 0 <= kept < laid_out, "the bar is kept before FillFrom changes it"
set_property = source[source.index("bool __fastcall hooked_SetProperty"):source.index("bool __fastcall hooked_GetProperty")]
for step in ("write_state(bar, bitmap, authored->state);", "s_held = bar;", "s_setProperty(self, edx, property, value);",
             "s_held = nullptr;", "authored->state = read_state(bar, bitmap);",
             "apply_fill_from(bar, bitmap, authored->mode, authored->state, true);"):
    assert step in set_property, step
assert "if (s_held && static_cast<uint8_t*>(self) - kBarBase == s_held)" in source
assert re.search(r"constexpr uint32_t kValue\s*= 0x1C;", source)

# The HUD editor's bar writer, modtools only (GameExt keeps the editor off on
# Steam and GOG), read off the disassembly: ElementBarBitmap::WriteData reads
# the bitmap (+0xB0), takes TexCoords with U1 from mBarU1 (+0x480), writes the
# two flag bits (+0x484), the fade times (+0x474, +0x478), then widens the rect
# to mBarWidth (+0x47C) through SetRect (+0x4C) for the bitmap's own writer and
# puts it back; RET 8. All of these are what write_state hands back.
editor_sites = [
    (0x00695B19, "8B BE B0 00 00 00"),          # MOV EDI,[ESI+0xB0]
    (0x00695B3E, "8B 96 80 04 00 00"),          # MOV EDX,[ESI+0x480]  mBarU1
    (0x00695B63, "8A 96 84 04 00 00"),          # MOV DL,[ESI+0x484]   flags
    (0x00695B80, "8A 86 84 04 00 00"),
    (0x00695BB2, "8B 96 74 04 00 00"),          # MOV EDX,[ESI+0x474]  Inc fade
    (0x00695BC8, "8B 86 78 04 00 00"),          # MOV EAX,[ESI+0x478]  Dec fade
    (0x00695C0E, "D8 86 7C 04 00 00"),          # FADD [ESI+0x47C]     mBarWidth
    (0x00695C27, "FF 52 4C"),                   # CALL [EDX+0x4C]      SetRect
    (0x00695C4B, "FF 52 4C"),                   #                      and back
    (0x00695C55, "C2 08 00"),                   # RET 8
]
editor_calls = {0x00695B35: "red_bitmap_get_tex_coords", 0x00695B54: "red_bitmap_set_tex_coords",
                0x00695C01: "red_bitmap_get_rect"}
bar_vtable = 0x00A5C824   # ElementBarBitmap's; +0x14 SetProperty, +0x18 GetProperty, +0x28 WriteData
bar_base_vtable = 0x00A5C7FC   # its ElementBar base's (bar + 0x220); +4 is SetValue
# The bar's SetProperty and GetProperty (modtools), read off the disassembly:
# a TexCoords change sets mBarU1 from its U1 and a size change mBarWidth from
# its width, before both go on to the bitmap's; ScaleTexture and ScaleSize call
# the bar's SetValue with its own value; both return with RET 8. SetValue does
# nothing when the value is unchanged, which is why a change is filled to the
# value by GameExt itself. The bar's constructor puts the ElementBar base's
# vtable at +0x220.
editor_property_sites = [
    (0x006952FB, "81 FF 40 F3 B5 79"),          # CMP EDI,ScaleTexture
    (0x0069534F, "8B 4B 08 89 8E 80 04 00 00"), # TexCoords: mBarU1 = U1
    (0x006953BB, "81 FF 61 D4 7A 96"),          # CMP EDI,the size
    (0x0069542A, "8B 13 89 96 7C 04 00 00"),    # size: mBarWidth = width
    (0x00695398, "8B 86 3C 02 00 00"),          # ScaleTexture: its value (+0x220 + 0x1C) ...
    (0x006953AB, "FF 52 04"),                   #   ... to SetValue
    (0x006953F0, "8B 96 3C 02 00 00"),          # ScaleSize: the same
    (0x00695403, "FF 50 04"),
    (0x00695335, "C2 08 00"),                   # RET 8
    (0x006954C9, "C2 08 00"),                   # GetProperty: RET 8
    (0x006960B5, "D9 44 24 30 D8 64 24 40 D9 E1 D8 1D DC C6 A5 00 DF E0 F6 C4 41 0F 85"),   # SetValue: skip
    (0x00695DFA, "8D 9E 20 02 00 00"),          # ctor: LEA EBX,[ESI+0x220]
    (0x00695E15, "C7 06 24 C8 A5 00 C7 03 FC C7 A5 00"),   # the two vtables
]

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
        editor = ""
        if mt:
            def target(address):
                while image[address - base] == 0xE9:
                    address += 5 + int.from_bytes(image[address - base + 1:address - base + 5], "little", signed=True)
                return address

            for name, literal, length in editor_guards:
                expected = codecs.decode(literal, "unicode_escape").encode("latin1")
                assert len(expected) == int(length), name
                assert image[table[name] - base:table[name] - base + len(expected)] == expected, (build, name)
            for address, text in editor_sites:
                want = b(text)
                assert image[address - base:address - base + len(want)] == want, (build, hex(address))
            for address, name in editor_calls.items():
                assert image[address - base] == 0xE8, (build, hex(address))
                called = address + 5 + int.from_bytes(image[address - base + 1:address - base + 5], "little", signed=True)
                assert target(called) == table[name], (build, name)
            def slot(vtable, offset):
                return target(int.from_bytes(image[vtable + offset - base:vtable + offset + 4 - base], "little"))

            assert slot(bar_vtable, 0x28) == table["hud_bar_bitmap_write_data"], (build, "WriteData")
            assert slot(bar_vtable, 0x14) == table["hud_bar_bitmap_set_property"], (build, "SetProperty")
            assert slot(bar_vtable, 0x18) == table["hud_bar_bitmap_get_property"], (build, "GetProperty")
            assert slot(bar_base_vtable, 0x04) == table["hud_bar_bitmap_set_value"], (build, "SetValue")
            assert table["hud_bar_bitmap_write_data"] == editor_sites[0][0] - 9
            for address, text in editor_property_sites:
                want = b(text)
                assert image[address - base:address - base + len(want)] == want, (build, hex(address))
            assert table["hud_bar_bitmap_set_property"] < 0x006952FB < 0x00695335 < table["hud_bar_bitmap_get_property"]
            editor = (f" and the editor's bar writer, setter and getter "
                      f"({len(editor_sites) + len(editor_property_sites)} sites)")
        else:
            for name in ("hud_bar_bitmap_write_data", "hud_bar_bitmap_set_property", "hud_bar_bitmap_get_property"):
                assert name not in table, (build, name, "the editor's bar hooks are modtools only")
        print(f"{build}: {len(guards)} guards and {len(sites[build])} offset sites{editor} passed")
    finally:
        pe.close()
