"""Read-only audit of everything AI sprint reads or calls in all three supported game PEs.

Usage: python tests/ai_sprint_abi_tests.py PATH_TO_GAMEDATA
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
source = (SRC / "ai/ai_sprint.cpp").read_text()
core = (SRC / "ai/ai_sprint_core.hpp").read_text()
addrs = (SRC / "core/game_addrs.hpp").read_text()


def constant(text, name):
    match = re.search(r"\b" + name + r"\s*=\s*(-?0x[0-9a-fA-F]+|-?\d+)\s*;", text)
    assert match, name
    return int(match.group(1), 0)


for name, value in {
    "kCtrl_Controller": 0xC8,
    "kCtrl_Object": -0x240,
    "kGO_MatrixTrans": 0x120,
    "kGO_HandleId": 0x204,
    "kUC_State": 0x7C,
    "kUC_LowLevel": 0x2C4,
    "kLL_Navigator": 0x18,
    "kLL_Height": 0x6C,
    "kLL_Target": 0xA0,
    "kNav_PathFollower": 0x10,
    "kGraph_GlobalDest": 0x24,
    "kPF_FinalPoint": 0x30,
    "kVt_IsRtti": 0x00,
    "kVt_NavIsDone": 0x14,
    "kVt_NavIsRtti": 0x20,
    "kEB_Class": 0x0,
    "kEB_Energy": 0x4,
    "kEB_Flags": 0x8,
    "kEBC_Max": 0x0,
    "kEnergyBarModtools": 0x7E0,
    "kEnergyBarRelease": 0x7D0,
    "kStateCombat": 2,
}.items():
    assert constant(source, name) == value, name
for name, value in {"kHeightStand": 0, "kHeightSprint": 3, "kStateStand": 0, "kStateSprint": 3}.items():
    assert constant(core, name) == value, name
for name in ("UnitController", "NavigatorGraphFollower", "NavigatorPathFollower"):
    assert f'pbl_hash("{name}")' in source, name


def namespace(text, name):
    body = re.search(r"namespace %s\s*\{(.*?)\n\s*\}\s*//\s*namespace %s" % (name, name), text, re.S).group(1)
    return constant(body, "entity_soldier_update_indirect")


assert namespace(addrs, "modtools") == 0x0053B920
assert namespace(addrs, "steam") == 0x004F0110
assert namespace(addrs, "gog") == 0x004F0110

# The prologue the install guards, as the source spells it.
assert r'"\x83\xEC\x48\x53\x55\x56\x57\x8B\xF1"' in source
assert r'"\x55\x8B\xEC\x83\xE4\xF8\x83\xEC\x44\x53\x56\x57\x8B\xF9"' in source

UPDATE_MT = [("sub", "esp, 0x48"), ("push", "ebx"), ("push", "ebp"), ("push", "esi"),
             ("push", "edi"), ("mov", "esi, ecx")]
UPDATE_RT = [("push", "ebp"), ("mov", "ebp, esp"), ("and", "esp, 0xfffffff8"), ("sub", "esp, 0x44"),
             ("push", "ebx"), ("push", "esi"), ("push", "edi"), ("mov", "edi, ecx")]

mt_checks = {
    # EntitySoldier::UpdateIndirect: this in ESI, dt on the stack, RET 4.
    0x0053B920: UPDATE_MT,
    0x0053BFF5: [("mov", "al, 1"), ("pop", "ebx"), ("add", "esp, 0x48"), ("ret", "4")],
    0x0053BA63: [("mov", "eax, dword ptr [esi + 0x514]")],                 # mState
    0x0053BA8A: [("mov", "eax, dword ptr [esi + 0xc8]"), ("push", "eax"),  # mCtrl
                 ("call", "0x40f493")],                                     # UnitControllerPtr
    0x0053BA9F: [("add", "edi, 0x2c4")],                                    # mLowLevel
    0x0053BCB0: [("mov", "eax, dword ptr [edi + 0x6c]"), ("cmp", "eax, 4")],  # the stance switch
    0x0053BCD2: [("mov", "eax, dword ptr [esi]"), ("mov", "ecx, esi"),      # Sprint(), slot +0xAC
                 ("call", "dword ptr [eax + 0xac]"), ("test", "al, al")],
    # UnitControllerPtr: Controller vptr slot 0 is IsRtti.
    0x0059E67F: [("mov", "eax, dword ptr [esi]"), ("push", "ecx"), ("mov", "ecx, esi"),
                 ("call", "dword ptr [eax]")],
    # UnitController::IsInCombat.
    0x0059EA10: [("mov", "edx, dword ptr [ecx + 0x7c]"), ("xor", "eax, eax"), ("cmp", "edx, 2"),
                 ("sete", "al"), ("ret", "")],
    # AILowLevel::IsNavigatorInProgress: mNavigator +0x18, IsDone at vptr +0x14.
    0x005A2D70: [("mov", "eax, dword ptr [ecx + 0x18]"), ("test", "eax, eax"), ("je", "0x5a2d88"),
                 ("mov", "ecx, eax"), ("mov", "eax, dword ptr [ecx]"), ("call", "dword ptr [eax + 0x14]")],
    # NavigatorGraphFollower::GetGlobalDest and IsRtti.
    0x005A7280: [("lea", "eax, [ecx + 0x24]"), ("ret", "")],
    0x005A72B0: [("mov", "eax, dword ptr [esp + 4]"), ("cmp", "dword ptr [0xb8f0f8], eax"),
                 ("jne", "0x5a72c1"), ("mov", "al, 1"), ("ret", "4")],
    # Navigator::GetGlobalDest: mPathFollower +0x10, its mFinalPoint +0x30.
    0x005A4CF0: [("mov", "eax, dword ptr [ecx + 0x10]"), ("test", "eax, eax")],
    0x005A4D05: [("add", "eax, 0x30"), ("ret", "")],
    # AILowLevel::ProcessFire reads mTarget.
    0x005A4126: [("lea", "ecx, [edi + 0xa0]")],
    # EntitySoldier::SprintUsingEnergy: the EnergyBar's overheat flag, then Sprint().
    0x00529640: [("test", "byte ptr [ecx + 0xa28], 1")],
    0x0052964C: [("mov", "eax, dword ptr [ecx + 0x240]"), ("add", "ecx, 0x240"),
                 ("jmp", "dword ptr [eax + 0xac]")],
}

STEAM_GOG_UPDATE = {
    0x004F0110: UPDATE_RT,
    0x004F07AE: [("pop", "edi"), ("pop", "esi"), ("mov", "al, 1"), ("pop", "ebx"), ("mov", "esp, ebp"),
                 ("pop", "ebp"), ("ret", "4")],
    0x004F01F5: [("mov", "eax, dword ptr [edi + 0x504]")],
    0x004F044A: [("mov", "eax, dword ptr [ebx + 0x6c]"), ("cmp", "eax, 4")],
    0x004F046C: [("mov", "eax, dword ptr [edi]"), ("mov", "ecx, edi"), ("mov", "eax, dword ptr [eax + 0xac]"),
                 ("call", "eax"), ("test", "al, al")],
    # SprintUsingEnergy and Sprint() (standing 0 or sprinting 3).
    0x004EDD90: [("test", "byte ptr [ecx + 0xa18], 1")],
    0x004EDD9C: [("mov", "eax, dword ptr [ecx + 0x240]"), ("add", "ecx, 0x240"),
                 ("mov", "eax, dword ptr [eax + 0xac]"), ("jmp", "eax")],
    0x004EDBE0: [("mov", "ecx, dword ptr [esi + 0x504]"), ("test", "ecx, ecx"), ("je", "0x4edbef"),
                 ("cmp", "ecx, 3")],
}
steam_checks = dict(STEAM_GOG_UPDATE)
steam_checks.update({
    0x004F0220: [("mov", "ecx, dword ptr [edi + 0xc8]"), ("call", "0x6630e0"), ("lea", "ebx, [eax + 0x2c4]")],
    0x006630E7: [("mov", "eax, dword ptr [esi]"), ("push", "dword ptr [0x1f99384]"),
                 ("mov", "eax, dword ptr [eax]"), ("call", "eax")],
    0x00663B92: [("cmp", "dword ptr [ecx + 0x7c], 2"), ("sete", "al"), ("ret", "")],
    0x0043BED0: [("mov", "ecx, dword ptr [ecx + 0x18]"), ("test", "ecx, ecx"), ("je", "0x43bee5"),
                 ("mov", "eax, dword ptr [ecx]"), ("mov", "eax, dword ptr [eax + 0x14]"), ("call", "eax")],
    0x005ADAC0: [("lea", "eax, [ecx + 0x24]"), ("ret", "")],
    0x005AC4E0: [("push", "ebp"), ("mov", "ebp, esp"), ("mov", "eax, dword ptr [ebp + 8]"),
                 ("cmp", "dword ptr [0x1ecb250], eax")],
    0x005AB6F0: [("mov", "eax, dword ptr [ecx + 0x10]"), ("test", "eax, eax")],
    0x005AB705: [("add", "eax, 0x30"), ("ret", "")],
    0x0043B628: [("lea", "ecx, [esi + 0xa0]")],
})
gog_checks = dict(STEAM_GOG_UPDATE)
gog_checks.update({
    0x004F0220: [("mov", "ecx, dword ptr [edi + 0xc8]"), ("call", "0x664180"), ("lea", "ebx, [eax + 0x2c4]")],
    0x00664187: [("mov", "eax, dword ptr [esi]"), ("push", "dword ptr [0x1f9a838]"),
                 ("mov", "eax, dword ptr [eax]"), ("call", "eax")],
    0x00664C32: [("cmp", "dword ptr [ecx + 0x7c], 2"), ("sete", "al"), ("ret", "")],
    0x0043BEC0: [("mov", "ecx, dword ptr [ecx + 0x18]"), ("test", "ecx, ecx"), ("je", "0x43bed5"),
                 ("mov", "eax, dword ptr [ecx]"), ("mov", "eax, dword ptr [eax + 0x14]"), ("call", "eax")],
    0x005AEA70: [("lea", "eax, [ecx + 0x24]"), ("ret", "")],
    0x005AD490: [("push", "ebp"), ("mov", "ebp, esp"), ("mov", "eax, dword ptr [ebp + 8]"),
                 ("cmp", "dword ptr [0x1ecc704], eax")],
    0x005AC6A0: [("mov", "eax, dword ptr [ecx + 0x10]"), ("test", "eax, eax")],
    0x005AC6B5: [("add", "eax, 0x30"), ("ret", "")],
    0x0043B618: [("lea", "ecx, [esi + 0xa0]")],
})

# vtable slots read straight from the image: (vtable, offset, function), thunks followed.
mt_slots = [
    (0x00A479B0, 0x30, 0x005A7280),   # NavigatorGraphFollower: GetGlobalDest
    (0x00A479B0, 0x20, 0x005A72B0),   #                         IsRtti
    (0x00A47A60, 0x30, 0x005A4CF0),   # NavigatorPathFollower:  Navigator::GetGlobalDest
]
steam_slots = [
    (0x0079CF2C, 0xC4, 0x004F0110),   # the soldier's Controllable vtable: UpdateIndirect
    (0x0079CF2C, 0xAC, 0x004EDBD0),   #                                    Sprint
    (0x007AA820, 0x30, 0x005ADAC0),
    (0x007AA820, 0x20, 0x005AC4E0),
    (0x007AA88C, 0x30, 0x005AB6F0),
]
gog_slots = [
    (0x0079DECC, 0xC4, 0x004F0110),
    (0x0079DECC, 0xAC, 0x004EDBD0),
    (0x007AB6EC, 0x30, 0x005AEA70),
    (0x007AB6EC, 0x20, 0x005AD490),
]

builds = [
    ("modtools", "BF2_modtools_NoDVD_NoConsole.exe", mt_checks, mt_slots),
    ("steam", "BattlefrontII.exe", steam_checks, steam_slots),
    ("gog", "BattlefrontII_GoG.exe", gog_checks, gog_slots),
]
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)


def follow(image, base, address):
    """An incremental-link thunk (JMP rel32) to its target."""
    code = image[address - base:address - base + 5]
    if code[0] == 0xE9:
        return address + 5 + struct.unpack("<i", code[1:5])[0]
    return address


def rtti_global(image, base, name):
    """The global a class's RTTI hash lives in: PUSH "<name>" / MOV ECX,<global> / CALL PblHash."""
    found = []
    start = 0
    while (text := image.find(name.encode() + b"\0", start)) >= 0:   # a name can be stored twice
        push = image.find(b"\x68" + struct.pack("<I", base + text))
        if push >= 0 and image[push + 5] == 0xB9:
            found.append(struct.unpack("<I", image[push + 6:push + 10])[0])
        start = text + 1
    assert len(found) == 1, (name, found)
    return found[0]


for name, filename, checks, slots in builds:
    pe = pefile.PE(str(Path(sys.argv[1]) / filename))
    try:
        image = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        assert base == 0x400000
        for address, expected in checks.items():
            data = image[address - base:address - base + 48]
            actual = [(i.mnemonic, i.op_str) for i in decoder.disasm(data, address, count=len(expected))]
            assert actual == expected, (name, hex(address), actual, expected)
        for table, offset, function in slots:
            entry = struct.unpack("<I", image[table + offset - base:table + offset - base + 4])[0]
            assert follow(image, base, entry) == function, (name, hex(table), hex(offset), hex(entry))
        # The type tests compare against PblHash of the class names: each build makes
        # the globals it compares from those very names.
        if name == "modtools":
            assert rtti_global(image, base, "UnitController") == 0x00B8EFBC
            assert rtti_global(image, base, "NavigatorGraphFollower") == 0x00B8F0F8
        elif name == "steam":
            assert rtti_global(image, base, "UnitController") == 0x01F99384
            assert rtti_global(image, base, "NavigatorGraphFollower") == 0x01ECB250
        else:
            assert rtti_global(image, base, "UnitController") == 0x01F9A838
            assert rtti_global(image, base, "NavigatorGraphFollower") == 0x01ECC704
        print(f"{name}: {len(checks)} instruction checks, {len(slots)} vtable slots and the RTTI names passed")
    finally:
        pe.close()
