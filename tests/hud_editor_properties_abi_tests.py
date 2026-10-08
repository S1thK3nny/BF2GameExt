"""Read-only audit of the HUD editor's GameExt properties (render/hud_editor_properties.cpp)
against the modtools executable, the only build with the editor.

Usage: python tests/hud_editor_properties_abi_tests.py PATH_TO_GAMEDATA
Requires pefile and capstone. Reads the addresses from game_addrs.hpp and the guards, layout
constants and names from the module, and checks them and the code the editor's panel runs:
the factory list and property list, Property::Data, the panel's line for a property, the base
enum names and the order the HUD opens in. Never loads or executes the game.
"""
import codecs
import re
import struct
import sys
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "PatcherDLL/src"
source = (SRC / "render/hud_editor_properties.cpp").read_text()
tws = (SRC / "render/hud_true_widescreen.cpp").read_text()
tws_core = (SRC / "render/hud_true_widescreen_core.hpp").read_text()
fill = (SRC / "render/hud_bar_fill_from.cpp").read_text()
latch = (SRC / "render/target_bar_latch.cpp").read_text()
dllmain = (SRC / "core/dllmain.cpp").read_text()
addresses = (SRC / "core/game_addrs.hpp").read_text()

LITERALS = r'((?:"[^"]*"\s*)+)'


def joined(literals):
    text = "".join(re.findall(r'"([^"]*)"', literals))
    return codecs.decode(text, "unicode_escape").encode("latin1")


def const(text, name):
    match = re.search(r"constexpr \w+\s+" + name + r"\s*=\s*(0x[0-9A-Fa-f]+|\d+)", text)
    assert match, name
    return int(match.group(1), 0)


def pbl_hash(text):
    h = 0x811C9DC5
    for c in text.encode():
        h = ((h ^ (c | 0x20)) * 0x01000193) & 0xFFFFFFFF
    return h


def b(text):
    return bytes.fromhex(text.replace(" ", ""))


# ---- the module ------------------------------------------------------------------
layout = {name: const(source, name) for name in ("kTypeEnum", "kFactoryProperties", "kFactoryType", "kFactoryNode")}
assert layout == {"kTypeEnum": 2, "kFactoryProperties": 4, "kFactoryType": 8, "kFactoryNode": 0x0C}, layout
assert 'static_assert(sizeof(PropertyData) == 0x3C, "Property::Data");' in source
assert re.search(r"struct PropertyData \{\s*const char\*\s+name;\s*uint32_t\s+hash;\s*uint32_t\s+type;\s*"
                 r"const char\* const\* names;\s*uint32_t\s+minValue;\s*uint32_t\s+maxValue;", source)
assert re.search(r"struct PropertyNode \{\s*PropertyNode\*\s+next;\s*const PropertyData\* data;", source)

# The names, in the order the owners count: ScreenAnchor none, 0 to 1 by 0.05 with the edges by
# name (hud_true_widescreen_core.hpp anchor_index); FillFrom Left, Right, Bottom, Top
# (hud_bar_fill_from.cpp fill_index).
anchor_names = re.findall(r'"([^"]*)"', re.search(r"kAnchorNames\[\] = \{(.*?)\};", source, re.S).group(1))
want = ["None", "Left", "0.05", "0.1", "0.15", "0.2", "0.25", "0.3", "0.35", "0.4", "0.45", "Center",
        "0.55", "0.6", "0.65", "0.7", "0.75", "0.8", "0.85", "0.9", "0.95", "Right"]
assert anchor_names == want, anchor_names
assert "constexpr int kAnchorChoices = 22;" in tws_core
assert "return index == 1 || index == 11 || index == kAnchorChoices - 1;" in tws_core
fill_names = re.findall(r'"([^"]*)"', re.search(r"kFillNames\[\] = \{(.*?)\};", source, re.S).group(1))
assert fill_names == ["Left", "Right", "Bottom", "Top"], fill_names
assert "return authored->mode == kRight ? 1 : authored->mode == kBottom ? 2 : 3;" in fill
assert "const Mode mode = index == 1 ? kRight : index == 2 ? kBottom : kTop;" in fill
assert 'PropertyData s_anchor = { "ScreenAnchor", pbl_hash("ScreenAnchor"), kTypeEnum, kAnchorNames, 0, kAnchorCount - 1, {} };' in source
assert 'PropertyData s_fill   = { "FillFrom", pbl_hash("FillFrom"), kTypeEnum, kFillNames, 0, kFillCount - 1, {} };' in source
assert 'const uint32_t kFillFactories[] = { pbl_hash("BarBitmap"), pbl_hash("ProceduralBarBitmap") };' in source

# Their owners answer the panel: ScreenAnchor in Element's GetProperty and SetProperty
# (hud_true_widescreen.cpp), FillFrom in the bar's (hud_bar_fill_from.cpp).
assert 'constexpr uint32_t kScreenAnchor   = pbl_hash("ScreenAnchor");' in tws
assert "if (property != kScreenAnchor || !value) return s_elementGet(self, edx, property, value);" in tws
assert "if (property != kScreenAnchor || !value) return s_elementSet(self, edx, property, value);" in tws
assert 'constexpr uint32_t kFillFrom      = pbl_hash("FillFrom");' in fill
assert fill.count("if (property == kFillFrom && value) {") == 2
# Linked when the HUD opens, after the stock opener and the other modules' resets; installed
# with the rest.
opener = re.search(r"static void __cdecl hooked_Open\(\)\n\{(.*?)\n\}", latch, re.S).group(1)
assert all(call in opener for call in ("original_Open();", "hud_bar_fill_from_open();",
                                       "hud_editor_properties_open();")), "linked when the HUD opens"
assert opener.index("original_Open();") < opener.index("hud_bar_fill_from_open();") < \
    opener.index("hud_editor_properties_open();"), "linked after GameEvents::Open"
assert "hud_editor_properties_install(exe_base);" in dllmain
assert "if (g_build != GameBuild::Modtools) return;" in source

guards = re.findall(r'\bcode_is\(base, a\.(\w+), "([^"]*)",\s*' + LITERALS + r',\s*(\d+)\)', source)
assert len(guards) == 4, len(guards)
assert "DetourAttach(&(PVOID&)s_translate, hooked_Translate)" in source
assert "using TranslateFn = const char*(__fastcall*)(void* self, void* edx, uint32_t property, uint32_t value);" \
    in source

# ---- the executable -------------------------------------------------------------------
body = re.search(r"namespace modtools\s*\{(.*?)\n\s*\}\s*//\s*namespace modtools", addresses, re.S).group(1)
table = {name: int(value, 16) for name, value in
         re.findall(r"constexpr uintptr_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;", body)}
pe = pefile.PE(str(Path(sys.argv[1]) / "BF2_modtools_NoDVD_NoConsole.exe"), fast_load=True)
image = pe.get_memory_mapped_image()
base = pe.OPTIONAL_HEADER.ImageBase
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
text_section = [s for s in pe.sections if s.Name.rstrip(b"\0") == b".text"][0]
text_lo = base + text_section.VirtualAddress
text = image[text_section.VirtualAddress:text_section.VirtualAddress + text_section.Misc_VirtualSize]


def at(va, n):
    return image[va - base:va - base + n]


def u32(va):
    return struct.unpack("<I", at(va, 4))[0]


def target(va):
    dest = va + 5 + struct.unpack("<i", at(va + 1, 4))[0]
    while at(dest, 1) == b"\xE9":
        dest = dest + 5 + struct.unpack("<i", at(dest + 1, 4))[0]
    return dest


def branches_to(va, kinds=(0xE8, 0xE9)):
    """Every CALL/JMP rel32 that lands on va, through modtools' incremental-link JMPs."""
    out = []
    for i in range(len(text) - 5):
        if text[i] in kinds:
            try:
                if target(text_lo + i) == va:
                    out.append((text_lo + i, text[i]))
            except Exception:
                pass
    return out


assert pe.FILE_HEADER.Characteristics & 0x0001, "modtools has a fixed base"
for name, what, literals, length in guards:
    want = joined(literals)
    assert len(want) == int(length), what
    assert at(table[name], len(want)) == want, (what, at(table[name], len(want)).hex())

# FindByHashID walks the list the module reads: its terminator, each factory's node 0xC in and
# its type 8 in.
find = table["hud_factory_find"]
assert at(find, 6) == b("8B 0D") + struct.pack("<I", table["hud_factory_list"])
assert at(find + 0x1E, 6) == b("8D 41 F4 39 50 08")
# PropertyGetFirst: the property list's terminator at factory + 4.
assert at(table["hud_factory_property_first"], 8) == b("8B 41 04 83 C1 04 3B C1")
# Property::Init for an enum: name +0, hash +4, type 2 at +8, names +0xC, min +0x10, max +0x14.
init = table["hud_property_init_enum"]
assert at(init + 0x0E, 2) == b("89 01") and at(init + 0x1F, 3) == b("89 51 04")
assert at(init + 0x29, 7) == b("C7 40 08 02 00 00 00")
assert at(init + 0x33, 3) == b("89 51 0C") and at(init + 0x3D, 3) == b("89 48 10") and at(init + 0x4B, 3) == b("89 42 14")

# Element::PropertyTranslateEnum: thiscall(hash, value) -> name, RET 8; the element types that
# name their own enums jump to it for any other, with the hash put back.
translate = table["hud_element_translate_enum"]
assert at(translate + 0x14, 3) == b("C2 08 00") and at(translate + 0x22, 3) == b("C2 08 00")
jumps = [va for va, kind in branches_to(translate) if kind == 0xE9 and va > 0x00420000]
assert len(jumps) == 3, [hex(j) for j in jumps]
for va in jumps:
    assert at(va - 4, 4) == b("89 44 24 04"), hex(va)   # MOV [ESP+4],EAX: the hash again

# The panel's line for a property (Editor::PropertyString, found by its "%s (%s)"): the
# selected element (+0x14)'s GetProperty (+0x18) first, nothing shown if it says no; then by
# the property's type, an enum (2) named through PropertyTranslateEnum (+0x1C).
fmt = base + image.find(b"%s (%s)\x00")
pushes = [text_lo + m.start() for m in re.finditer(re.escape(b"\x68" + struct.pack("<I", fmt)), text)]
assert pushes, "the panel's format"
line = 0x0068E3B0
assert all(line < p < line + 0x320 for p in pushes), [hex(p) for p in pushes]
code = at(line, 0x60)
assert b("8B 47 14 8B 18") in code and b("8B 4F 14 50 FF 53 18") in code, "GetProperty on the selected element"
skip = code.find(b("84 DB"))
assert skip > 0 and code[skip + 6:skip + 8] == b("0F 84"), "no line when GetProperty says no"
jump_table = u32(line + 0x4B)
assert at(line + 0x48, 3) == b("FF 24 85") and at(line + 0x3F, 3) == b("83 F8 0A"), "the type switch"
enum_case = u32(jump_table + 4)   # type 2, the switch subtracting 1
assert b("FF 53 1C") in at(enum_case, 0x20), hex(enum_case)

# HUD::Manager::Open calls GameEvents::Open after every factory and the editor are made: the
# last call but the heap's restore.
manager_open = 0x006B8A00
calls = []
for ins in decoder.disasm(at(manager_open, 0x503), manager_open):
    if ins.bytes[0] == 0xE8:
        calls.append((ins.address, target(ins.address)))
    if ins.mnemonic == "ret":
        break
events = [i for i, (_, t) in enumerate(calls) if t == table["hud_game_events_open"]]
editor = [i for i, (_, t) in enumerate(calls) if t == 0x0068F310]
assert len(events) == 1 and len(editor) == 1 and editor[0] < events[0] and events[0] >= len(calls) - 2, \
    ([hex(a) for a, _ in calls], events, editor)

pe.close()
print("modtools: %d guards, the factory and property lists, Property::Data, the base enum names and the "
      "three types that defer to them, the panel's line for a property, the order the HUD opens in, and "
      "the owners' answers passed" % len(guards))
