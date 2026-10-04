"""Read-only audit of the flyer roll throttle fix against all three game PEs.

Usage: python tests/flyer_roll_throttle_abi_tests.py PATH_TO_GAMEDATA
Requires pefile and capstone. Reads the site addresses from game_addrs.hpp and
the bytes the install checks from the module, and checks them, the addresses
the loader moves inside them, and the code the shims rely on, against each
executable. Never loads or executes the game. Not an in-game behaviour test.
"""
import re
import struct
import sys
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "PatcherDLL/src/entity/flyer_roll_throttle_fix.cpp").read_text()
addresses = (ROOT / "PatcherDLL/src/core/game_addrs.hpp").read_text()


def array(name):
    match = re.search(r"const uint8_t " + name + r"\[\]\s*=\s*\{(.*?)\};", source, re.S)
    assert match, name
    body = re.sub(r"//[^\n]*", "", match.group(1))
    return bytes(int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]{2}", body))


module = {
    "modtools": (array("kSiteModtools"), array("kBeforeModtools"), array("kAfterModtools")),
    "steam": (array("kSiteSteam"), array("kBeforeRelease"), array("kAfterRelease")),
    "gog": (array("kSiteGog"), array("kBeforeRelease"), array("kAfterRelease")),
}
assert "kCtrl_Trackable   = 0x18" in source and "kVt_GetGameObject = 0x1C" in source
assert "push   dword ptr [ebx + 4]" in source and "push   dword ptr [edi + 4]" in source

# Where the 1.0's address sits in the compared bytes; the install moves it as
# the loader did before comparing (Steam and GOG are always relocated).
ONE = {"modtools": int(re.search(r"kOneModtools = (\d+);", source).group(1)),
       "release": int(re.search(r"kOneRelease  = (\d+);", source).group(1))}
assert "matches(at, site, siteLen, modtools ? kOneModtools : kOneRelease, exe_base)" in source
assert "matches(at + siteLen, after, afterLen, kOneModtools, exe_base)" in source
assert "rebase_operand(want, one, base);" in source


def relocations(pe):
    """The addresses the loader moves when it loads the exe elsewhere (HIGHLOW entries)."""
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])
    base = pe.OPTIONAL_HEADER.ImageBase
    return {base + e.rva for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", [])
            for e in block.entries if e.type == pefile.RELOCATION_TYPE["IMAGE_REL_BASED_HIGHLOW"]}

# Each read off the disassembly before recording. The function sets its this
# register once at entry; after the compare come the two paths, then the
# stores of the pair into the driven Controllable (this + 4): the sideways
# input at +0x84 (mControlStrafe) and the throttle at +0x80 (mControlMove).
# build: (this register's entry move, the scaling path, the keep path, the
#         stores as (offset from the site, bytes))
expected = {
    "modtools": ("8B D9",                               # MOV EBX,ECX
                 "D8 3D",                               # FDIVR [1.0]
                 "DD D8",                               # FSTP ST(0): the length off
                 [(0x29, "8B 43 04"),                   # MOV EAX,[EBX+4]
                  (0x30, "89 88 84 00 00 00"),          # MOV [EAX+0x84],ECX
                  (0x3A, "8B 53 04"),                   # MOV EDX,[EBX+4]
                  (0x3D, "89 82 80 00 00 00")]),        # MOV [EDX+0x80],EAX
    "steam": ("8B F9",                                  # MOV EDI,ECX
              "F3 0F 10 44 24 10 F3 0F 5E C1",          # MOVSS XMM0,[ESP+0x10]; DIVSS XMM0,XMM1
              "F3 0F 10 44 24 0C",                      # MOVSS XMM0,[ESP+0xC]: the throttle as it came
              [(0x30, "8B 47 04"),                      # MOV EAX,[EDI+4]
               (0x39, "F3 0F 11 88 84 00 00 00"),       # MOVSS [EAX+0x84],XMM1
               (0x41, "8B 47 04"),
               (0x44, "F3 0F 11 80 80 00 00 00")]),     # MOVSS [EAX+0x80],XMM0
}
expected["gog"] = expected["steam"]

builds = [
    ("modtools", "BF2_modtools_NoDVD_NoConsole.exe"),
    ("steam", "BattlefrontII.exe"),
    ("gog", "BattlefrontII_GoG.exe"),
]
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)


def b(text):
    return bytes.fromhex(text.replace(" ", ""))


for build, filename in builds:
    body = re.search(r"namespace " + build + r"\s*\{(.*?)\n\s*\}\s*//\s*namespace " + build,
                     addresses, re.S).group(1)
    table = {name: int(value, 16) for name, value in
             re.findall(r"constexpr uintptr_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;", body)}
    mt = build == "modtools"
    pe = pefile.PE(str(Path(sys.argv[1]) / filename), fast_load=True)
    try:
        image = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        assert base == 0x400000

        def at(va, n):
            return image[va - base:va - base + n]

        site_bytes, before, after = module[build]
        entry_move, scale, keep_code, stores = expected[build]
        update = table["player_controller_update"]
        site = table["player_controller_input_cap"]
        assert update < site < update + 0x1000, (build, hex(site))

        # What the install compares, and the jump it leaves.
        assert at(site, len(site_bytes)) == site_bytes, (build, at(site, len(site_bytes)).hex())
        assert at(site - len(before), len(before)) == before, (build, "before")
        assert at(site + len(site_bytes), len(after)) == after, (build, "after")
        assert len(site_bytes) >= 5
        keep = site + len(site_bytes) + struct.unpack("<b", site_bytes[-1:])[0]
        assert at(site + len(site_bytes), len(b(scale))) == b(scale), (build, "scaling path")
        assert at(keep, len(b(keep_code))) == b(keep_code), (build, "keep path", hex(keep))

        # The compare is against 1.0, and so is modtools' divide after it.
        one = ONE["modtools" if mt else "release"]
        operand = struct.unpack("<I", site_bytes[one:one + 4])[0]
        assert struct.unpack("<f", at(operand, 4))[0] == 1.0, (build, hex(operand))
        if mt:
            assert struct.unpack("<I", after[one:one + 4])[0] == operand, (build, "the divide's 1.0")

        # What the loader moves inside the compared bytes: on Steam and GOG, which
        # are always loaded away from their build address, only the 1.0's address
        # in the compare, which the install moves the same way; modtools never moves.
        moved = relocations(pe)
        lo, hi = site - len(before), site + len(site_bytes) + len(after)
        inside = sorted(va for va in moved if lo - 3 <= va < hi)
        if mt:
            assert pe.FILE_HEADER.Characteristics & 0x0001 and not moved, (build, "modtools has a fixed base")
            assert inside == [], (build, [hex(va) for va in inside])
        else:
            assert pe.OPTIONAL_HEADER.DllCharacteristics & 0x0040, (build, "retail is built to be relocated")
            assert inside == [site + one], (build, [hex(va) for va in inside])

        # The this register: set from ECX at entry and never written again on
        # the way down to the site (pops on early-return paths aside).
        reg = "ebx" if mt else "edi"
        code = list(decoder.disasm(at(update, site - update), update))
        assert sum(1 for x in code if x.bytes == b(entry_move)) == 1, (build, "entry move")
        writes = [x for x in code if x.op_str.startswith(reg + ",") and x.mnemonic not in ("cmp", "test")]
        assert [x.bytes for x in writes] == [b(entry_move)], (build, [(hex(x.address), x.mnemonic, x.op_str) for x in writes])

        for offset, text in stores:
            assert at(site + offset, len(b(text))) == b(text), (build, hex(site + offset))
        print(f"{build}: site, both paths, the 1.0 compare, its relocation, this register and "
              f"{len(stores)} stores passed")
    finally:
        pe.close()
