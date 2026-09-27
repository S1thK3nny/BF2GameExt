"""Read-only audit of the class icon reads against all three supported game PEs.

Usage: python tests/hud_class_icons_abi_tests.py PATH_TO_GAMEDATA
Requires pefile. Reads the module's constants and guards from the production
sources and checks the instruction bytes they were read from; never loads or
executes the game or writes to its files. Not an in-game behaviour test.
"""
import codecs
import re
import sys
from pathlib import Path

import pefile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "PatcherDLL/src/render/hud_class_icons.cpp").read_text()
addresses = (ROOT / "PatcherDLL/src/core/game_addrs.hpp").read_text()


def const(name):
    match = re.search(r"\b" + name + r"\s*=\s*(0x[0-9a-fA-F]+)", source)
    assert match, name
    return int(match.group(1), 16)


def layout(name):
    match = re.search(r"\b" + name + r"\s*=\s*\{\s*(0x[0-9a-fA-F]+)", source)
    assert match, name
    return int(match.group(1), 16)


assert const("kVt_GetGameObject") == 0x1C
assert const("kVt_GetEntityClass") == 0x28
assert const("kCtrl_Trackable") == 0x18
assert const("kVt_GetWeaponIndex") == 0x3C
assert const("kVt_GetWeapon") == 0x40
assert const("kWeaponClassIcon") == 0x6C
weapon_layout = (ROOT / "PatcherDLL/src/core/layout/weapon.hpp").read_text()
assert re.search(r"\bkClass\s*=\s*0x064\b", weapon_layout), "Weapon::mClass"
health = {"modtools": layout("kModtools"), "steam": layout("kRelease"), "gog": layout("kRelease")}
guard = re.search(r'"PblHashTableCode::_Find",\s*modtools \? "([^"]*)"\s*: "([^"]*)"', source)
assert guard, "Update the audit if the _Find guard changes shape"
find_prologue = {"modtools": guard.group(1), "steam": guard.group(2), "gog": guard.group(2)}


def b(text):
    return bytes.fromhex(text.replace(" ", ""))


# Each site was disassembled before recording. The GetGameObject and
# GetEntityClass sites are HUD::GameEvents::UpdateVehicleHealth's own calls on
# Character::mVehicle; the store is EntityClass::SetProperty's HealthTexture
# case; the last is EventBitmap's type 3 (Uint) call into SetTexture(uint).
sites = {
    "modtools": [
        (0x006B48E9, "8B 50 18 8D 48 18 FF 52 1C"),        # [mVehicle+0x18]->slot +0x1C
        (0x006B4AE1, "8B 01 FF 50 28 8B 40 4C"),           # GetEntityClass, mHUDModelID
        (0x004D017F, "89 46 48"),                          # mHealthTexture store
        (0x00698FA7, "8B 11 50 FF 52 50"),                 # SetTexture(uint)
        (0x0061F754, "89 56 6C"),                          # WeaponClass::mIconTexture store
        # UpdateWeaponEvents: GetWeaponIndex(ch), GetWeapon(index), Weapon::mClass
        (0x006B2F0A, "FF 50 3C 33 FF 3B C7 7C 1B 8B 16 50 8B CE FF 52 40"
                     "8B D8 3B DF 89 5C 24 34 74 0F 8B 43 64"),
    ],
    "steam": [
        (0x00561D86, "8B 41 18 83 C1 18 FF 50 1C"),
        (0x00561EF3, "8B 03 8B CB FF 50 28 8B 40 2C"),
        (0x00491B5D, "89 47 28"),
        (0x0054DC57, "FF 56 50"),
        (0x0067B084, "89 47 6C"),
        (0x00560ADE, "8B 40 3C FF D0 8B D0 85 D2 78 1B 8B 06 8B CE 52 8B 40 40"
                     "FF D0 8B F0 89 75 F0 85 F6 74 0D 8B 46 64"),
    ],
    "gog": [
        (0x00562B06, "8B 41 18 83 C1 18 FF 50 1C"),
        (0x00562C73, "8B 03 8B CB FF 50 28 8B 40 2C"),
        (0x00491B5D, "89 47 28"),
        (0x0054E9A7, "FF 56 50"),
        (0x0067C124, "89 47 6C"),
        (0x0056185E, "8B 40 3C FF D0 8B D0 85 D2 78 1B 8B 06 8B CE 52 8B 40 40"
                     "FF D0 8B F0 89 75 F0 85 F6 74 0D 8B 46 64"),
    ],
}
# The mHealthTexture store's displacement must be the module's layout value.
for build, entries in sites.items():
    assert entries[2][1].split()[-1] == "%02X" % health[build], (build, "layout")
    assert "FF 50 28" in entries[1][1], build          # CALL [EAX+0x28]
    assert entries[0][1].endswith(" 1C"), build        # CALL [reg+0x1C]
    assert entries[4][1].endswith(" 6C"), build        # WeaponClass +0x6C
    assert entries[5][1].endswith(" 64"), build        # Weapon::mClass +0x64

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

        def at(address, length):
            return image[address - base:address - base + length]

        for address, expected in sites[build]:
            want = b(expected)
            assert at(address, len(want)) == want, (build, hex(address), at(address, len(want)).hex())

        prologue = codecs.decode(find_prologue[build], "unicode_escape").encode("latin1")
        assert at(table["pbl_hash_table_find"], len(prologue)) == prologue, (build, "_Find")
        # The engine itself looks textures up in this table with size 0x2000.
        push = b"\x68\x00\x20\x00\x00\x68" + table["tex_hash_table"].to_bytes(4, "little")
        assert image.find(push) != -1, (build, "texture table")
        print(f"{build}: {len(sites[build]) + 2} class icon checks passed")
    finally:
        pe.close()
