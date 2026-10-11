"""Read-only audit of the selection inputs in all three supported game PEs,
including the crosshair pick: the engine's candidate tests it copies, the
weapon range and aim line it casts along, and the ray's exclude list.

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
    "kVt_IsVisibleToMe": 0x94,
    "kCtrl_EyePoint": 0xDC,
    "kCtrl_EyeDir": 0xE8,
    "kCtrl_AimStart": 0x148,
    "kGO_Class": 0x08,
    "kGO_Targetable": 0x238,
    "kWC_ExtremeRange": 0xF0,
}.items():
    match = re.search(r"\b" + name + r"\s*=\s*(0x[0-9a-fA-F]+)", source)
    assert match and int(match.group(1), 16) == value, name
assert "g_build == GameBuild::Modtools ? 0x128 : 0x104" in source
# COLL_SOFT | COLL_RIGID | COLL_STATIC | COLL_ATTACHED | COLL_TERRAIN
assert "kAimRayMask = 0x02 | 0x08 | 0x10 | 0x20 | 0x80;" in source

# The headers the crosshair pick reads through.
headers = ROOT / "PatcherDLL/src/game/Battlefront2/Source"
for header, pattern in [
    ("GameObjectClass.h", r"Field<uint8_t>\s+mTargetFlags\{0x36C,\s*0x328\}"),
    ("GameObjectClass.h", r"kNotTargetableByPlayer\s*=\s*0x10;"),
    ("EntitySoldier.h", r"Field<uint8_t>\s+m_uiInvisibilityAlpha\{0x753,\s*0x743\}"),
    ("Weapon.h", r"\bkClass\s*=\s*0x064;"),
    ("CollisionObject.h", r"\bkVt_GetGameObject\s*=\s*0x38;"),
]:
    assert re.search(pattern, (headers / header).read_text()), (header, pattern)

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
    # The crosshair pick. PlayerController::Update (0x0059B460): Weapon::mClass,
    # mAimStart, mEyeDir, and WeaponClass::mExtremeRange, which is the radius
    # of its search (GetObjectsInRange's range, pushed from [esp+0x58]).
    0x0059C230: [("mov", "esi, dword ptr [eax + 0x64]")],
    0x0059C32C: [("mov", "ecx, dword ptr [eax + 0x148]")],
    0x0059C34A: [("mov", "edx, dword ptr [eax + 0xe8]")],
    0x0059C38F: [("mov", "ecx, dword ptr [esi + 0xf0]"),
                 ("mov", "dword ptr [esp + 0x74], edx"),
                 ("mov", "dword ptr [esp + 0x5c], eax"),
                 ("mov", "dword ptr [esp + 0x58], ecx")],
    0x0059C70C: [("mov", "esi, dword ptr [esp + 0x58]"), ("push", "0x3f000000"),
                 ("push", "esi")],
    # Its candidate tests: mIsTargetable, IsVisibleToMe(viewer), the class's
    # IsNotTargetableByPlayer, a soldier's m_uiInvisibilityAlpha (candidate and
    # current target).
    0x0059C800: [("test", "byte ptr [esi + 0x238], 1")],
    0x0059C80D: [("mov", "eax, dword ptr [esp + 0x2c]"), ("mov", "edx, dword ptr [esi]"),
                 ("push", "eax"), ("mov", "ecx, esi"),
                 ("call", "dword ptr [edx + 0x94]"), ("test", "al, al")],
    0x0059C831: [("mov", "edx, dword ptr [esi + 8]"),
                 ("test", "byte ptr [edx + 0x36c], 0x10")],
    0x0059C857: [("movzx", "eax, byte ptr [esi + 0x753]")],
    0x0059C2E2: [("movzx", "edx, byte ptr [ecx + 0x753]")],
    # GameObjectClass::SetProperty: IsNotTargetableByPlayer sets that bit.
    0x00561223: [("cmp", "eax, 0x1b2d820b")],
    0x00561246: [("mov", "al, byte ptr [esi + 0x36c]"), ("or", "al, 0x10"),
                 ("pop", "edi"), ("mov", "byte ptr [esi + 0x36c], al")],
    # GameObject::IsVisibleToMe: thiscall, one argument, bool in AL.
    0x0055F830: [("mov", "al, 1"), ("ret", "4")],
    # RayCallback: the hit's CollisionObject GetGameObject (+0x38) is what the
    # exclude list ([data+8], count [data+0xC]) is compared with.
    0x00429E14: [("mov", "eax, dword ptr [edi]"), ("push", "esi"), ("mov", "ecx, edi"),
                 ("call", "dword ptr [eax + 0x38]"), ("mov", "esi, dword ptr [esp + 0x10]"),
                 ("mov", "edx, dword ptr [esi + 0xc]")],
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
    # The crosshair pick, as on modtools. XMM3 (mExtremeRange) is kept at
    # [esp+0x18], which becomes GetObjectsInRange's range in XMM1.
    0x0061AF34: [("mov", "eax, dword ptr [ecx + 0x64]")],
    0x0061B03D: [("movq", "xmm0, qword ptr [ecx + 0x148]")],
    0x0061B055: [("movq", "xmm0, qword ptr [ecx + 0xe8]")],
    0x0061B087: [("movss", "xmm3, dword ptr [eax + 0xf0]")],
    0x0061B5A8: [("movss", "xmm1, dword ptr [esp + 0x18]"), ("lea", "edx, [esp + 0x160]"),
                 ("push", "0x3b")],
    0x0061B63F: [("test", "byte ptr [edi + 0x238], 1")],
    0x0061B64C: [("mov", "eax, dword ptr [edi]"), ("mov", "ecx, edi"),
                 ("push", "dword ptr [esp + 0x10]"), ("mov", "eax, dword ptr [eax + 0x94]"),
                 ("call", "eax"), ("test", "al, al")],
    0x0061B674: [("mov", "eax, dword ptr [edi + 8]"),
                 ("test", "byte ptr [eax + 0x328], 0x10")],
    0x0061B69B: [("movzx", "eax, byte ptr [edi + 0x743]")],
    0x0061AFE3: [("movzx", "eax, byte ptr [esi + 0x743]")],
    0x005368A0: [("cmp", "eax, 0x1b2d820b")],
    0x005368BF: [("or", "byte ptr [esi + 0x328], 0x10")],
    0x00535A50: [("mov", "al, 1"), ("ret", "4")],
    0x0045F088: [("mov", "eax, dword ptr [ebx]"), ("mov", "ecx, ebx"), ("push", "esi"),
                 ("push", "edi"), ("call", "dword ptr [eax + 0x38]"),
                 ("mov", "esi, dword ptr [ebp + 0xc]"), ("xor", "ecx, ecx"),
                 ("mov", "edx, eax"), ("mov", "edi, dword ptr [esi + 0xc]")],
}
# Each GOG site was independently disassembled before recording these addresses.
gog_checks = dict(zip(
    [0x0056185E, 0x0056186E, 0x00561DA5, 0x00561DB1, 0x00561DCF, 0x00561FEB,
     0x0061C8D3, 0x0061C8F7, 0x0061C91B,
     0x0061BFA4, 0x0061C0AD, 0x0061C0C5, 0x0061C0F7, 0x0061C618, 0x0061C6AF,
     0x0061C6BC, 0x0061C6E4, 0x0061C70B, 0x0061C053, 0x00537610, 0x0053762F,
     0x005367C0, 0x0045F088],
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
        print(f"{name}: {len(checks)} selection-input, cache and crosshair-pick checks passed")
    finally:
        pe.close()
