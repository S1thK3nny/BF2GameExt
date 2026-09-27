"""Read-only audit of the selection inputs in all three supported game PEs.

Usage: python tests/target_bar_selection_abi_tests.py PATH_TO_GAMEDATA
Requires pefile and capstone. Does not execute the game or modify its files.
These are instruction/layout regression checks, not an in-game behaviour test.
"""
import re
import sys
from pathlib import Path

import capstone
import pefile


ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "PatcherDLL/src/render/target_bar_latch.cpp").read_text()
for name, value in {
    "kVt_GetWeaponIndex": 0x3C,
    "kVt_GetWeapon": 0x40,
    "kCtrl_ReticuleTgt": 0x164,
    "kGO_HandleId": 0x204,
    "kWD_TargetObject": 0x14,
    "kWD_TargetHandleId": 0x18,
    "kVt_GetControllable": 0x6C,
    "kCtrl_Character": 0xCC,
    "kCtrl_Trackable": 0x18,
    "kVt_GetGameObject": 0x20,
}.items():
    match = re.search(r"\b" + name + r"\s*=\s*(0x[0-9a-fA-F]+)", source)
    assert match and int(match.group(1), 16) == value, name
assert "g_build == GameBuild::Modtools ? 0x128 : 0x104" in source

mt_checks = {
    0x006B2F0A: [("call", "dword ptr [eax + 0x3c]")],
    0x006B2F18: [("call", "dword ptr [edx + 0x40]")],
    0x006B3609: [("mov", "ecx, dword ptr [eax + 0x128]"),
                 ("add", "eax, 0x128")],
    0x006B361A: [("mov", "ecx, dword ptr [ecx + 0x204]"),
                 ("cmp", "ecx, dword ptr [eax + 4]")],
    0x006B3630: [("lea", "eax, [eax + ecx*8 + 0x164]")],
    # The rider -> vehicle chain the pair uses, where modtools swaps a rider for
    # its vehicle: GetControllable, mCharacter, mVehicle, Trackable GetGameObject.
    0x004CE784: [("call", "dword ptr [eax + 0x6c]")],
    0x004CE792: [("mov", "ecx, dword ptr [eax + 0xcc]")],
    0x004CE7A9: [("mov", "eax, dword ptr [ecx + 0x14c]")],
    0x004CE7C6: [("mov", "edx, dword ptr [ecx + 0x18]"), ("add", "ecx, 0x18"),
                 ("call", "dword ptr [edx + 0x20]")],
}
steam_checks = {
    0x00560ADE: [("mov", "eax, dword ptr [eax + 0x3c]"), ("call", "eax")],
    0x00560AEE: [("mov", "eax, dword ptr [eax + 0x40]"), ("call", "eax")],
    0x00561025: [("add", "ecx, 0x104"), ("mov", "eax, dword ptr [ecx]")],
    0x00561031: [("mov", "eax, dword ptr [eax + 0x204]"),
                 ("cmp", "eax, dword ptr [ecx + 4]")],
    0x0056104F: [("lea", "ecx, [ecx + eax*8]"), ("add", "ecx, 0x164")],
    0x0056126B: [("mov", "dword ptr [edi + 0x14], esi"),
                 ("mov", "dword ptr [edi + 0x18], ecx")],
    # PlayerController::Update skipping a candidate that rides the player's own
    # vehicle: the same chain the rider pair reads.
    0x0061B863: [("mov", "eax, dword ptr [eax + 0x6c]"), ("call", "eax")],
    0x0061B887: [("mov", "eax, dword ptr [eax + 0xcc]"),
                 ("cmp", "dword ptr [eax + 0x14c], 0")],
    0x0061B8AB: [("mov", "eax, dword ptr [ecx + 0x18]"), ("add", "ecx, 0x18"),
                 ("mov", "eax, dword ptr [eax + 0x20]"), ("call", "eax"),
                 ("cmp", "eax, esi")],
}
# Each GOG site was independently disassembled before recording these addresses.
gog_checks = dict(zip(
    [0x0056185E, 0x0056186E, 0x00561DA5, 0x00561DB1, 0x00561DCF, 0x00561FEB,
     0x0061C8D3, 0x0061C8F7, 0x0061C91B],
    steam_checks.values(),
))

builds = [
    ("modtools", "BF2_modtools_NoDVD_NoConsole.exe", mt_checks),
    ("steam", "BattlefrontII.exe", steam_checks),
    ("gog", "BattlefrontII_GoG.exe", gog_checks),
]
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for name, filename, checks in builds:
    pe = pefile.PE(str(Path(sys.argv[1]) / filename))
    try:
        image = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        assert base == 0x400000
        for address, expected in checks.items():
            data = image[address - base:address - base + 32]
            actual = [(i.mnemonic, i.op_str) for i in
                      decoder.disasm(data, address, count=len(expected))]
            assert actual == expected, (name, hex(address), actual, expected)
        print(f"{name}: {len(checks)} selection-input/cache instruction checks passed")
    finally:
        pe.close()
