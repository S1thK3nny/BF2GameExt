"""Read-only audit of the team icon patch against all three game PEs.

Usage: python tests/hud_team_texture_abi_tests.py PATH_TO_GAMEDATA
Requires pefile and capstone. Reads the address from game_addrs.hpp and the
install guard from the module, and checks them, and the instructions the
write-up rests on, against each executable. Never loads or executes the game.
Not an in-game behaviour test.
"""
import re
import struct
import sys
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "PatcherDLL/src/render/hud_team_texture.cpp").read_text()
addresses = (ROOT / "PatcherDLL/src/core/game_addrs.hpp").read_text()
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)


def array(name):
    match = re.search(r"constexpr uint8_t\s+" + name + r"\[\]\s*=\s*\{([^}]*)\}", source)
    assert match, name
    return bytes(int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", match.group(1)))


guard = {name: array(name) for name in ("kTest", "kTestThree", "kJneModtools", "kJneRetail",
                                        "kSendModtools", "kSendRetail")}
assert guard == {
    "kTest": b"\x83\xF8\x02\x74",
    "kTestThree": b"\x83\xF8\x03",
    "kJneModtools": b"\x0F\x85",
    "kJneRetail": b"\x75",
    "kSendModtools": b"\x8A\x46\x04\x84\xC0",
    "kSendRetail": b"\x80\x7E\x04\x00",
}, "Update the audit if the install guard changes"
assert "static_cast<uint8_t>(site[4] + 3)" in source, "Update the audit if the patch changes"
assert re.search(r"constexpr size_t kPatchLen = 2;", source)


def relocations(pe):
    """The addresses the loader moves when it loads the exe elsewhere (HIGHLOW entries)."""
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])
    base = pe.OPTIONAL_HEADER.ImageBase
    return {base + e.rva for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", [])
            for e in block.entries if e.type == pefile.RELOCATION_TYPE["IMAGE_REL_BASED_HIGHLOW"]}


# Read off the disassembly before recording.
facts = {
    "modtools": dict(
        site=0x006B190F, send=0x006B191D, skip=0x006B199A, flag_type=0x00B93CB0,
        set_flag_type=0x0046D090, show_team_points=0x0046C140, update=0x006B1870,
        # Space Assault: MOV AL,[EDX+0x621] / TEST AL,AL / JE flag test, then
        # ShowTeamPoints' setter for the team and a JMP past the send.
        space_assault=(0x006B18ED, "8A 82 21 06 00 00 84 C0 74 13"), space_assault_call=0x006B18FD,
        # The icon: [ESI+4] picks +0x24 or +0x20, looked up in the 0x2000-slot
        # table, else +0x1C.
        icon=[(0x006B1924, "8B 47 24"), (0x006B1929, "8B 47 20"), (0x006B192D, "68 00 20 00 00"),
              (0x006B1950, "8B 47 1C"), (0x006B1954, "68 00 20 00 00")],
    ),
    "steam": dict(
        site=0x00562717, send=0x00562721, skip=0x00562772, flag_type=0x01E55F1C,
        set_flag_type=0x0058D2F0, show_team_points=0x0058C440, update=0x00562690,
        space_assault=(0x005626FD, "80 B8 21 06 00 00 00 74 0C"), space_assault_call=0x0056270B,
        icon=[(0x00562727, "8B 47 24"), (0x0056272C, "8B 47 20"), (0x00562730, "68 00 20 00 00"),
              (0x00562746, "FF 77 1C"), (0x00562749, "68 00 20 00 00")],
    ),
    "gog": dict(
        site=0x00563497, send=0x005634A1, skip=0x005634F2, flag_type=0x01E573BC,
        set_flag_type=0x0058E290, show_team_points=0x0058D3F0, update=0x00563410,
        space_assault=(0x0056347D, "80 B8 21 06 00 00 00 74 0C"), space_assault_call=0x0056348B,
        icon=[(0x005634A7, "8B 47 24"), (0x005634AC, "8B 47 20"), (0x005634B0, "68 00 20 00 00"),
              (0x005634C6, "FF 77 1C"), (0x005634C9, "68 00 20 00 00")],
    ),
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
    f = facts[build]
    mt = build == "modtools"
    pe = pefile.PE(str(Path(sys.argv[1]) / filename), fast_load=True)
    try:
        image = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        assert base == 0x400000

        def at(va, n):
            return image[va - base:va - base + n]

        def thunk(va):
            while at(va, 1) == b"\xE9":
                va = va + 5 + struct.unpack("<i", at(va + 1, 4))[0]
            return va

        def call_target(va):
            assert at(va, 1) == b"\xE8", (build, hex(va), at(va, 5).hex())
            return thunk(va + 5 + struct.unpack("<i", at(va + 1, 4))[0])

        def calls(va, n):
            return [thunk(int(x.op_str, 16)) for x in decoder.disasm(at(va, n), va)
                    if x.mnemonic == "call" and x.op_str.startswith("0x")]

        site = table["hud_team_texture_flag_test"]
        assert site == f["site"], (build, hex(site))

        # The guard, as the module reads it.
        assert at(site - 5, 1) == b"\xA1", build
        assert at(site, 4) == guard["kTest"], (build, at(site, 4).hex())
        rel = at(site + 4, 1)[0]
        assert rel <= 0x7F - 3, (build, rel)
        assert at(site + 5, 3) == guard["kTestThree"], build
        jne = guard["kJneModtools"] if mt else guard["kJneRetail"]
        assert at(site + 8, len(jne)) == jne, build
        send = site + 5 + rel
        assert send == f["send"], (build, hex(send))
        want = guard["kSendModtools"] if mt else guard["kSendRetail"]
        assert at(send, len(want)) == want, (build, at(send, len(want)).hex())
        # Where the JNE goes when the type is neither: past the send.
        skip = (site + 14 + struct.unpack("<i", at(site + 10, 4))[0] if mt
                else site + 10 + struct.unpack("<b", at(site + 9, 1))[0])
        assert skip == f["skip"], (build, hex(skip))
        # The patch: JMP rel8 at the CMP, landing where the JE did.
        assert site + 2 + (rel + 3) == send, build

        # The MOV reads the flag type, which SetFlagGameplayType writes: 2 for
        # "1flag", 3 for "2flag" (PblHash 0x216B8B26, 0x9DB77DA7).
        operand = struct.unpack("<I", at(site - 4, 4))[0]
        assert operand == f["flag_type"], (build, hex(operand))
        name = image.find(b"\x00SetFlagGameplayType\x00") + 1
        entry = image.find(struct.pack("<I", base + name))
        assert struct.unpack("<I", at(base + entry + 4, 4))[0] == f["set_flag_type"], build
        stores = {}
        for x in decoder.disasm(at(f["set_flag_type"], 0x60), f["set_flag_type"]):
            m = re.match(r"dword ptr \[(0x[0-9a-f]+)\], (\w+)$", x.op_str)
            if x.mnemonic == "mov" and m:
                stores.setdefault(int(m.group(1), 16), []).append(m.group(2))
        assert set(stores) == {f["flag_type"]}, (build, stores)
        assert {"2", "3"} <= set(stores[f["flag_type"]]), (build, stores)
        code = at(f["set_flag_type"], 0x60)
        assert struct.pack("<I", 0x216B8B26) in code and struct.pack("<I", 0x9DB77DA7) in code, build

        # The update the site sits in, and the Space Assault branch ahead of the
        # test: it calls the setter the Lua ShowTeamPoints calls and sends no icon.
        update = f["update"]
        assert update < f["space_assault"][0] < site < update + 0x400, build
        va, text = f["space_assault"]
        assert at(va, len(bytes.fromhex(text.replace(" ", "")))) == bytes.fromhex(text.replace(" ", "")), \
            (build, hex(va))
        setter = call_target(f["space_assault_call"])
        name = image.find(b"\x00ShowTeamPoints\x00") + 1
        entry = image.find(struct.pack("<I", base + name))
        assert struct.unpack("<I", at(base + entry + 4, 4))[0] == f["show_team_points"], build
        assert setter in calls(f["show_team_points"], 0x50), (build, hex(setter))

        # The icon the send looks up.
        for va, text in f["icon"]:
            want = bytes.fromhex(text.replace(" ", ""))
            assert at(va, len(want)) == want, (build, hex(va), at(va, len(want)).hex())

        # Retail moves: the flag type's address in the MOV is relocated, and
        # nothing the patch writes is.
        relocs = relocations(pe)
        if mt:
            assert not relocs or site - 4 in relocs, build
        else:
            assert pe.OPTIONAL_HEADER.DllCharacteristics & 0x0040, (build, "retail is built to be relocated")
            assert site - 4 in relocs, (build, "the flag type operand is relocated")
        assert not any(r < site + 2 and r + 4 > site for r in relocs), (build, "patched bytes are relocated")

        print(f"{build}: flag type test, send, skip, flag type writer, Space Assault branch, "
              f"{len(f['icon'])} icon sites and relocations passed")
    finally:
        pe.close()
