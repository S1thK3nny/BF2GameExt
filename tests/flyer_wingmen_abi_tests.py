"""Read-only audit of everything the flyer wingmen prototype reads, calls or redirects (modtools only).

Usage: python tests/flyer_wingmen_abi_tests.py PATH_TO_GAMEDATA
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
source = (SRC / "ai/flyer_wingmen.cpp").read_text()
addrs = (SRC / "core/game_addrs.hpp").read_text()
character = (SRC / "game/Battlefront2/Source/Character.h").read_text()
squads = (SRC / "ai/ai_squads.cpp").read_text()


def constant(text, name):
    match = re.search(r"\b" + name + r"\s*=\s*(-?0x[0-9a-fA-F]+|-?\d+)\s*;", text)
    assert match, name
    return int(match.group(1), 0)


for name, value in {
    "kFly_Controllable": 0x240,
    "kFly_Ctrl": 0x308,
    "kFly_State": 0x5A4,
    "kFly_Flags": 0x5F4,
    "kFly_FreeFlyTime": 0x1CDC,
    "kStateFlying": 2,
    "kFlagPaths": 0x08,
    "kFlagSliding": 0x10,
    "kUC_LowLevel": 0x2C4,
    "kLL_Dest": 0x1C,
    "kDestSize": 0x34,
    "kDest_Has": 0x00,
    "kDest_Pos": 0x04,
    "kDest_Original": 0x10,
    "kDest_Dir": 0x1C,
    "kDest_Safe": 0x28,
    "kDest_Taper": 0x2C,
    "kDest_Speed": 0x30,
    "kSafeHeightFull": 0,
    "kVt_GetVelocity": 0x44,
    "kGO_MatrixIface": 0xC,
    "kVt_GetMatrix": 0x40,
    "kCtl_Squad": 0x60,
    "kSquadPressed": 3,
    "kGO_Class": 0x8,
    "kClass_Id": 0x18,
}.items():
    assert constant(source, name) == value, name
# The reads it shares with AI squads (tests/ai_squads_abi_tests.py audits those).
for name in ("kVt_IsRtti", "kGO_Flags", "kGO_HandleId", "kGO_Team", "kCtrl_Owner", "kOwner_Trackable",
             "kVt_GetTrackable", "kNodeNext", "kNodePayload"):
    assert constant(source, name) == constant(squads, name), name
assert constant(character, "kVehicle") == 0x14C
assert 'pbl_hash("EntityFlyer")' in source and 'pbl_hash("UnitController")' in source
assert r'"\x81\xEC\x84\x00\x00\x00\x53\x55\x56\x8B\xF1", 11' in source                          # UpdateIndirect
assert r'"\x83\xEC\x28\x53\x55\x56\x8B\xE9\x8B\x85\x08\x03\x00\x00", 14' in source              # CalculateDest
assert source.count(r'"\xD9\x05\x6C\xA0\xA2\x00\xD9\x81\xFC\x05\x00\x00", 12') == 2             # the getters
assert r'"\xD9\x05\x6C\xA0\xA2\x00\x56\x8B\xF1\x8B\x8E\x6C\x06\x00\x00", 15' in source       # DoTrick
assert 'pbl_hash("WingmanClass")' in source and 'pbl_hash("WingmanCount")' in source
assert 'pbl_hash("WingmanAuto")' in source

mt = re.search(r"namespace modtools\s*\{(.*?)\n\s*\}\s*//\s*namespace modtools", addrs, re.S).group(1)
NAMES = ("flyer_update_indirect", "flyer_calc_dest_call", "flyer_calc_dest", "flyer_max_speed", "flyer_min_speed",
         "flyer_do_trick")
EXPECT = (0x004F4C10, 0x004F4D4D, 0x004F4630, 0x004F0950, 0x004F0A10, 0x004F3D10)
for name, value in zip(NAMES, EXPECT):
    assert constant(mt, name) == value, name

checks = {
    # EntityFlyer::UpdateIndirect: `this` the Controllable, the entity 0x240 below it.
    0x004F4C10: [("sub", "esp, 0x84"), ("push", "ebx"), ("push", "ebp"), ("push", "esi"), ("mov", "esi, ecx")],
    0x004F4CA9: [("lea", "edi, [esi - 0x240]"), ("mov", "ecx, edi"), ("call", "0x401528")],
    0x00401528: [("jmp", "0x4d7960")],
    # Its state test: 1 takeoff, 2 flying ...
    0x004F4C94: [("mov", "eax, dword ptr [esi + 0x364]"), ("cmp", "eax, 1")],
    # ... and its one call to CalculateDest: dt, dest, point, speed, myPos, the entity in ECX.
    0x004F4D36: [("lea", "edx, [esp + 0x30]"), ("push", "edx"), ("lea", "eax, [esp + 0x14]"), ("push", "eax"),
                 ("lea", "ecx, [esp + 0x28]"), ("push", "ecx"), ("lea", "edx, [esp + 0x54]"), ("push", "edx"),
                 ("push", "ebx"), ("mov", "ecx, edi"), ("call", "0x401726"), ("test", "al, al")],
    0x00401726: [("jmp", "0x4f4630")],
    # CalculateDest: mCtrl at +0x308, AILowLevel::mDest.mMaxSpeed at UnitController +0x310 (0x2C4 + 0x1C + 0x30).
    0x004F4630: [("sub", "esp, 0x28"), ("push", "ebx"), ("push", "ebp"), ("push", "esi"), ("mov", "ebp, ecx"),
                 ("mov", "eax, dword ptr [ebp + 0x308]"), ("push", "edi"), ("push", "eax"), ("call", "0x40f493"),
                 ("mov", "ecx, dword ptr [esp + 0x4c]"), ("mov", "esi, eax"),
                 ("mov", "eax, dword ptr [esi + 0x310]"), ("add", "esi, 0x2c4")],
    0x004F4798: [("ret", "0x14")],
    0x004F4AD9: [("ret", "0x14")],
    # The barrier slide bit, 0x10 of the flags.
    0x004F46A2: [("and", "byte ptr [ebp + 0x5f4], 0xef")],
    0x004F46A9: [("test", "byte ptr [ebp + 0x5f4], 0x10")],
    # GetFollowingPath: flies paths (8), flying (2), alive, mFreeFlyTime not above 0, no trick.
    0x004D7960: [("mov", "dl, byte ptr [ecx + 0x5f4]"), ("test", "dl, 8"), ("je", "0x4d799f"),
                 ("cmp", "dword ptr [ecx + 0x5a4], 2"), ("jne", "0x4d799f"), ("mov", "eax, dword ptr [ecx + 0x1fc]"),
                 ("shr", "eax, 3"), ("test", "al, 1"), ("je", "0x4d799f"), ("fld", "dword ptr [ecx + 0x1cdc]")],
    # The speed getters: thiscall on the entity, the class at +0x66C, the result in ST0.
    0x004F0950: [("fld", "dword ptr [0xa2a06c]"), ("fld", "dword ptr [ecx + 0x5fc]"), ("fucompp", "")],
    0x004F0993: [("ret", "")],
    0x004F0A10: [("fld", "dword ptr [0xa2a06c]"), ("fld", "dword ptr [ecx + 0x5fc]"), ("fucompp", "")],
    # DoTrick: thiscall(Trick) on the entity, RET 4; its class at +0x66C. UpdateIndirect asks
    # it for a side roll (6) through the thunk 0x00401EE7.
    0x004F3D10: [("fld", "dword ptr [0xa2a06c]"), ("push", "esi"), ("mov", "esi, ecx"),
                 ("mov", "ecx, dword ptr [esi + 0x66c]")],
    0x004F3FBF: [("ret", "4")],
    0x004F5023: [("push", "6"), ("lea", "ecx, [esi - 0x240]"), ("call", "0x401ee7")],
    0x00401EE7: [("jmp", "0x4f3d10")],
    # EntityFlyer::Update: the squad-key trigger (Controllable +0x60, 3 when pressed), read
    # only while landed (mState +0x364 from the Controllable is 0).
    0x004FE4A1: [("mov", "eax, dword ptr [ebx + 0x364]"), ("xor", "edi, edi"), ("cmp", "eax, edi"),
                 ("jne", "0x4fe4cd"), ("mov", "eax, dword ptr [ebx + 0x60]"), ("and", "eax, 3"),
                 ("cmp", "al, 3")],
    # EntityClass::Read finds a ClassParent by mId at +0x18, the PblHash of its name.
    0x004D08F0: [("cmp", "dword ptr [ecx + 0x18], edx")],
}

decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
pe = pefile.PE(str(Path(sys.argv[1]) / "BF2_modtools_NoDVD_NoConsole.exe"))
image = pe.get_memory_mapped_image()
base = pe.OPTIONAL_HEADER.ImageBase
assert base == 0x400000
for address, expected in checks.items():
    data = image[address - base:address - base + 96]
    actual = [(i.mnemonic, i.op_str) for i in decoder.disasm(data, address, count=len(expected))]
    assert actual == expected, (hex(address), actual, expected)
# The guards' bytes are the code's.
for va, text in ((0x004F4C10, "81EC84000000535556 8BF1"), (0x004F4630, "83EC2853555 68BE98B8508030000"),
                 (0x004F0950, "D9056CA0A200D981FC050000"), (0x004F0A10, "D9056CA0A200D981FC050000"),
                 (0x004F3D10, "D9056CA0A200568BF18B8E6C060000")):
    want = bytes.fromhex(text.replace(" ", ""))
    assert image[va - base:va - base + len(want)] == want, hex(va)
# The getters' 0.0, the zero the free-flight time is compared with.
assert struct.unpack("<f", image[0x00A2A06C - base:0x00A2A06C - base + 4])[0] == 0.0
print(f"modtools: {len(checks)} instruction checks, the guards and the constants passed")
