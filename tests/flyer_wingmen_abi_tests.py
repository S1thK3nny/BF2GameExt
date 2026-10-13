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
    "kFly_TargetSpeed": 0x598,
    "kFlagTrick": 0x03,
    "kFly_SideRoll": 0x5D8,
    "kFly_LastTrick": 0x5E4,
    "kFly_EnergyValue": 0x61C,
    "kFly_EnergyFlags": 0x620,
    "kTrickSideRoll": 6,
    "kTrickCount": 7,
    # AI squadrons: the game's squadron rule and its formation shapes.
    "kFly_Class": 0x66C,
    "kClass_PathClass": 0x6AC,
    "kSquad_FormationCount": 0x28,
    "kPathClass_VsFlyer": 0x78,
    "kClass_SquadronCos": 0x1104,
    "kClass_SquadronDist2": 0x1108,
    "kVt_ClassModel": 0x20,
    "kModel_Red": 0x20,
    "kRed_Radius": 0x94,
    "kPath_MaxSquad": 0x84,
    "kFly_SquadronTime": 0x1CE8,
    "kForm_Nodes": 0x18,
    "kForm_NodeCount": 0x1C,
    "kForm_Root": 0x28,
    "kForm_Flags": 0x2C,
    "kNodeSize": 0x1C,
    "kMaxClassFormations": 10,
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
assert source.count(r'"\xD9\x05\x6C\xA0\xA2\x00\xD9\x81\xFC\x05\x00\x00", 12') == 3             # the getters
assert r'"\xD9\x05\x6C\xA0\xA2\x00\x56\x8B\xF1\x8B\x8E\x6C\x06\x00\x00", 15' in source       # DoTrick
assert 'pbl_hash("WingmanClass")' in source and 'pbl_hash("WingmanCount")' in source
assert 'pbl_hash("WingmanAuto")' in source and 'pbl_hash("WingmanSlot")' in source

mt = re.search(r"namespace modtools\s*\{(.*?)\n\s*\}\s*//\s*namespace modtools", addrs, re.S).group(1)
NAMES = ("flyer_update_indirect", "flyer_calc_dest_call", "flyer_calc_dest", "flyer_max_speed", "flyer_min_speed",
         "flyer_do_trick", "flyer_mid_speed", "entity_formation_find", "flyer_update_path_follower")
EXPECT = (0x004F4C10, 0x004F4D4D, 0x004F4630, 0x004F0950, 0x004F0A10, 0x004F3D10, 0x004F09B0, 0x005E2EB0, 0x004F30C0)
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
    # Why the getters' stand-ins keep every register: EntityFlyer::Update keeps the flyer in EDX
    # across GetFlyerMaxSpeed and loads ECX from it for GetFlyerMidSpeed (a C++ stand-in that lost
    # EDX crashed here, 2026-10-13).
    0x004FED55: [("call", "0x415aff"), ("fstp", "dword ptr [esp + 0x10]"), ("mov", "ecx, edx"),
                 ("call", "0x402e64")],
    0x00415AFF: [("jmp", "0x4f0950")],
    0x00402E64: [("jmp", "0x4f09b0")],
    0x00415479: [("jmp", "0x4f0a10")],
    0x004F09B0: [("fld", "dword ptr [0xa2a06c]"), ("fld", "dword ptr [ecx + 0x5fc]"), ("fucompp", "")],
    # The throttle (the move control at [esp+0x78]) becomes the speed it heads for, at the
    # Controllable's +0x358 (the entity's +0x598): below 0 from mid down to min at -1,
    # above 0 from mid up to max at +1, all three from the getters; then kept in min..max.
    0x004FED19: [("fld", "dword ptr [esp + 0x78]"), ("mov", "ecx, edx"), ("fcomp", "dword ptr [0xa2a06c]"),
                 ("fnstsw", "ax"), ("test", "ah, 5"), ("jp", "0x4fed55"), ("call", "0x402e64"),
                 ("fstp", "dword ptr [esp + 0x2c]"), ("mov", "ecx, edx"), ("call", "0x415479"),
                 ("fstp", "dword ptr [esp + 0x10]"), ("mov", "ecx, edx"), ("call", "0x402e64"),
                 ("fsubr", "dword ptr [esp + 0x10]"), ("fmul", "dword ptr [esp + 0x78]"),
                 ("fsubr", "dword ptr [esp + 0x2c]"), ("jmp", "0x4fed7c")],
    0x004FED5E: [("mov", "ecx, edx"), ("call", "0x402e64"), ("fsubr", "dword ptr [esp + 0x10]"),
                 ("mov", "ecx, edx"), ("fmul", "dword ptr [esp + 0x78]"), ("fstp", "dword ptr [esp + 0x10]"),
                 ("call", "0x402e64"), ("fadd", "dword ptr [esp + 0x10]"), ("mov", "ecx, edx"),
                 ("fstp", "dword ptr [ebx + 0x358]"), ("call", "0x415aff"), ("fstp", "dword ptr [esp + 0x14]"),
                 ("mov", "ecx, edx"), ("call", "0x415479"), ("fld", "dword ptr [ebx + 0x358]")],
    # EBX there is the Controllable: the same function reads mState at its +0x364.
    0x004FE4A7: [("xor", "edi, edi")],
    # DoTrick: no tricks for a class with no single-tap cost (+0xD8C); a side roll whatever was
    # asked with no double-tap cost (+0xD90) ...
    0x004F3D19: [("mov", "ecx, dword ptr [esi + 0x66c]"), ("fld", "dword ptr [ecx + 0xd8c]"), ("fucompp", ""),
                 ("fnstsw", "ax"), ("test", "ah, 0x44"), ("jnp", "0x4f3fbe"), ("fld", "dword ptr [0xa2a06c]"),
                 ("push", "ebp"), ("fld", "dword ptr [ecx + 0xd90]"), ("mov", "ebp, 6"), ("fucompp", ""),
                 ("fnstsw", "ax"), ("test", "ah, 0x44"), ("jnp", "0x4f3d51"), ("mov", "ebp, dword ptr [esp + 0xc]")],
    # ... refused while in a trick (flags 3) or when SpendEnergy on the bar at +0x618 refuses ...
    0x004F3DBF: [("test", "byte ptr [esi + 0x5f4], 3"), ("mov", "dword ptr [esi + 0x614], edi"), ("jne", "0x4f3f88"),
                 ("mov", "ecx, dword ptr [esp + 0x10]"), ("push", "ecx"), ("lea", "ecx, [esi + 0x618]"),
                 ("call", "0x40d143"), ("test", "al, al"), ("je", "0x4f3f88")],
    # ... and, once it went ahead, records the trick it did at +0x5E4.
    0x004F3F01: [("mov", "dword ptr [esi + 0x5e4], ebp")],
    # EnergyBar::SpendEnergy: refuses while spent out (bit 0 of +8), else takes from the energy (+4).
    0x0040D143: [("jmp", "0x4d0050")],
    0x004D006A: [("mov", "edx, dword ptr [ecx + 8]"), ("test", "dl, 1"), ("je", "0x4d0077"), ("xor", "al, al"),
                 ("ret", "4"), ("fld", "dword ptr [ecx + 4]"), ("fsub", "dword ptr [esp + 4]")],
    # The side roll (case 6): SideRoll goes by the sign of mSideRoll.x (+0x5D8) ...
    0x004F3E07: [("mov", "ecx, esi"), ("call", "0x413cb4")],
    0x00413CB4: [("jmp", "0x4f3410")],
    0x004F3410: [("fld", "dword ptr [ecx + 0x5d8]")],
    # ... which UpdateIndirect writes at the Controllable's +0x398 before its own side roll.
    0x004F500C: [("lea", "ecx, [esi + 0x398]")],
    # RollAdd and FlipAdd: in a trick, flags bit 0 rolling, bit 1 flipping.
    0x004F0A74: [("mov", "al, byte ptr [ecx + 0x5f4]"), ("fadd", "dword ptr [ecx + 0x5c0]"), ("or", "al, 1")],
    0x004F0B54: [("mov", "al, byte ptr [ecx + 0x5f4]"), ("fadd", "dword ptr [ecx + 0x5c8]"), ("or", "al, 2")],
    # ---- AI squadrons: the game's rule (EntityFlyer::UpdatePathFollower) ----
    # Placed on its path and not in a squadron, a flyer counts mSquadronFormationTime (+0x1CE8)
    # down by dt; ResetSquadronTime, once it is not above 0, sets it to 7 give or take 1.5 s and
    # says to try ...
    0x004F31F4: [("fld", "dword ptr [esi + 0x1ce8]"), ("mov", "ecx, esi"), ("fsub", "dword ptr [esp + 0x60]"),
                 ("fstp", "dword ptr [esi + 0x1ce8]"), ("call", "0x40158c"), ("test", "al, al"), ("je", "0x4f332f")],
    0x0040158C: [("jmp", "0x4f08c0")],
    0x004F08C0: [("push", "esi"), ("mov", "esi, ecx"), ("test", "byte ptr [esi + 0x78], 1"), ("jne", "0x4f08f8"),
                 ("fld", "dword ptr [esi + 0x1ce8]"), ("fcomp", "dword ptr [0xa2a06c]"), ("fnstsw", "ax"),
                 ("test", "ah, 0x41"), ("jp", "0x4f08f8"), ("push", "0x40400000"), ("push", "0x40e00000"),
                 ("call", "0x40f74f"), ("fstp", "dword ptr [esi + 0x1ce8]")],
    # ... its reach: the class's squared squadron distance (+0x1108) where not 0, else its model's
    # bounding radius (GetModel at the class's vptr +0x20, then +0x20, +0x94) times 30, squared ...
    0x004F3213: [("mov", "ecx, dword ptr [esi + 0x66c]"), ("fld", "dword ptr [0xa2a06c]"),
                 ("fld", "dword ptr [ecx + 0x1108]"), ("fucompp", ""), ("fnstsw", "ax"), ("test", "ah, 0x44"),
                 ("jnp", "0x4f323a"), ("mov", "eax, dword ptr [ecx + 0x1108]")],
    0x004F323A: [("mov", "edx, dword ptr [ecx]"), ("call", "dword ptr [edx + 0x20]"), ("mov", "eax, dword ptr [eax + 0x20]"),
                 ("fld", "dword ptr [eax + 0x94]"), ("fmul", "dword ptr [0xa37e18]"), ("fld", "st(0)"),
                 ("fmul", "st(1)")],
    # ... a flyby first (offline), else a squadron from the closest, with the class's angle (+0x1104).
    0x004F325F: [("call", "0x40423c"), ("test", "eax, eax"), ("jne", "0x4f332f")],
    0x004F32A6: [("mov", "edx, dword ptr [ecx + 0x1104]")],
    0x004F32BE: [("call", "0x40fc6d")],
    0x0040FC6D: [("jmp", "0x5e8f70")],
    # CreateSquadronFromClosestEntities: off when networked; only a FlyerVsFlyer path class
    # (+0x78); only with formations (the follower's squadron class, +0x28) or in a squadron
    # already; only on a path (+0xA8); candidates from the follower list ([0x00AD3528]) ...
    0x005E8F70: [("mov", "al, byte ptr [0xadabc2]"), ("sub", "esp, 0x74"), ("push", "ebp"), ("test", "al, al"),
                 ("mov", "al, byte ptr [0xbe14f1]"), ("mov", "ebp, ecx"), ("jne", "0x5e8f89"),
                 ("mov", "al, byte ptr [0xbe14f0]"), ("test", "al, al"), ("jne", "0x5e8fc7"),
                 ("mov", "eax, dword ptr [ebp]"), ("lea", "ecx, [esp + 0x2c]"), ("push", "ecx"), ("mov", "ecx, ebp"),
                 ("call", "dword ptr [eax + 0x1c]"), ("mov", "eax, dword ptr [eax]"), ("mov", "cl, byte ptr [eax + 0x78]"),
                 ("test", "cl, cl"), ("je", "0x5e8fc7"), ("mov", "edx, dword ptr [ebp]"), ("mov", "ecx, ebp"),
                 ("call", "dword ptr [edx + 4]"), ("mov", "ecx, dword ptr [eax + 0x28]"), ("test", "ecx, ecx"),
                 ("jne", "0x5e8fbd")],
    0x005E8FBD: [("mov", "eax, dword ptr [ebp + 0xa8]"), ("test", "eax, eax"), ("jne", "0x5e8fd0")],
    0x005E9012: [("mov", "esi, dword ptr [0xad3528]"), ("mov", "ecx, dword ptr [esi + 0xc]")],
    # ... within the reach (squared, [esp+0x90]) and, with an angle above -1, heading within it ...
    0x005E90A6: [("fld", "dword ptr [esp + 0x14]"), ("fcomp", "dword ptr [esp + 0x90]"), ("fnstsw", "ax"),
                 ("test", "ah, 0x41"), ("jp", "0x5e9158"), ("fld", "dword ptr [esp + 0x94]"),
                 ("fcomp", "dword ptr [0xa2a9e4]")],
    # ... and as many as the path's MaxSquadSize (+0x84) lets.
    0x005E9172: [("mov", "eax, dword ptr [ebp + 0xa8]"), ("mov", "eax, dword ptr [eax + 0x84]")],
    # The follower's squadron class (its vptr +4): the class's +0x6AC, where the
    # EntitySquadronClass begins.
    0x00413791: [("jmp", "0x4f1d90")],
    0x004F1D90: [("mov", "eax, dword ptr [ecx + 0x2a0]"), ("test", "eax, eax"), ("je", "0x4f1da0"),
                 ("add", "eax, 0x6ac"), ("ret", "")],
    # EntitySquadronClass::SelectFormation: its names (+0) and count (+0x28); each found
    # by EntityFormation::Find and kept when GetNumSlots is the size asked for.
    0x005EDC2A: [("mov", "ebx, ecx"), ("mov", "ebp, dword ptr [ebx + 0x28]")],
    0x005EDC51: [("mov", "eax, dword ptr [ebx + esi*4]"), ("push", "eax"), ("call", "0x4054ac"), ("mov", "edi, eax"),
                 ("add", "esp, 4")],
    0x005EDC91: [("mov", "ecx, edi"), ("call", "0x414f74"), ("cmp", "dword ptr [esp + 0x20], eax")],
    0x004054AC: [("jmp", "0x5e2eb0")],
    0x00414F74: [("jmp", "0x5e27c0")],
    # EntityFormation::Find, cdecl(name): its list from [0x00AD33C4], each one's name at +0x14.
    0x005E2EB0: [("push", "esi"), ("mov", "esi, dword ptr [0xad33c4]"), ("mov", "ecx, dword ptr [esi + 0xc]"),
                 ("test", "ecx, ecx"), ("push", "edi"), ("je", "0x5e2ed6"), ("mov", "edi, dword ptr [esp + 0xc]"),
                 ("call", "0x820190"), ("cmp", "eax, edi"), ("je", "0x5e2edb"), ("mov", "esi, dword ptr [esi + 4]"),
                 ("mov", "ecx, dword ptr [esi + 0xc]"), ("test", "ecx, ecx"), ("jne", "0x5e2ec3"), ("pop", "edi"),
                 ("xor", "eax, eax"), ("pop", "esi"), ("ret", "")],
    0x00820190: [("mov", "eax, dword ptr [ecx + 0x14]"), ("ret", "")],
    # GetNumSlots: its points (+0x18's count, +0x1C), one fewer when the root (+0x28) is no
    # slot (+0x2C bit 0 clear); the nodes 0x1C bytes each, from +0x18.
    0x005E27C0: [("push", "esi"), ("mov", "esi, ecx"), ("call", "0x8201a0"), ("test", "byte ptr [esi + 0x2c], 1"),
                 ("pop", "esi"), ("jne", "0x5e27d0"), ("dec", "eax"), ("ret", "")],
    0x008201A0: [("add", "ecx, 0x18"), ("jmp", "0x820140")],
    0x00820140: [("mov", "eax, dword ptr [ecx + 4]"), ("ret", "")],
    0x00820150: [("mov", "eax, dword ptr [esp + 4]"), ("mov", "edx, dword ptr [ecx]"), ("imul", "eax, eax, 0x1c"),
                 ("add", "eax, edx"), ("ret", "4")],
    0x005E27E0: [("mov", "al, byte ptr [ecx + 0x2c]"), ("and", "al, 1"), ("ret", "")],
    0x005E27F0: [("mov", "eax, dword ptr [ecx + 0x28]"), ("ret", "")],
    # Its rootisslot property sets bit 0 of +0x2C.
    0x005E2830: [("mov", "ecx, dword ptr [esp + 8]"), ("cmp", "dword ptr [ecx], 0xc1e3fa81")],
    0x005E285F: [("mov", "eax, dword ptr [esp + 0xc]"), ("mov", "dl, byte ptr [eax + 0x2c]")],
    # EntityFormation::Read: each point made relative to the root's ([esp+0xc..0x14] after the
    # GetNode call); the root's rotation turned through RotateVector into a local and dropped.
    0x005E2CD4: [("push", "esi"), ("mov", "ecx, edi"), ("call", "0x820160"), ("fld", "dword ptr [eax]"),
                 ("fsub", "dword ptr [esp + 0xc]"), ("push", "eax"), ("lea", "ecx, [esp + 0x1c]"),
                 ("fstp", "dword ptr [eax]"), ("fld", "dword ptr [eax + 4]"), ("fsub", "dword ptr [esp + 0x14]"),
                 ("fstp", "dword ptr [eax + 4]"), ("fld", "dword ptr [eax + 8]"), ("fsub", "dword ptr [esp + 0x18]"),
                 ("fstp", "dword ptr [eax + 8]"), ("lea", "eax, [esp + 0x60]"), ("push", "eax"), ("call", "0x803f20"),
                 ("inc", "esi"), ("cmp", "esi, ebx")],
    # EntityFlyerClass's constructor: the squadron angle and distance 0 by default (EBX, the
    # 0 it also gives mBoostFlyerSpeed, +0x898, which Phantom's constructor sets to 0.0).
    0x004F9BB6: [("mov", "dword ptr [esi + 0x898], ebx")],
    0x004FA01E: [("mov", "dword ptr [esi + 0x1104], ebx"), ("mov", "dword ptr [esi + 0x1108], ebx")],
}
# The stand-ins are naked and keep EDX and the XMM registers.
for name in ("max_speed_mt", "min_speed_mt", "do_trick_mt"):
    body = re.search(r"__declspec\(naked\) void " + name + r"\(\)\s*\{(.*?)\n\}", source, re.S)
    assert body, name
    asm = body.group(1)
    assert "movups [esp + 0x70], xmm7" in asm and "movups xmm7, [esp + 0x70]" in asm, name
    assert ("push   edx" in asm and "pop    edx" in asm) or ("pushad" in asm and "popad" in asm), name
for name, stand_in in (("s_maxSpeed", "max_speed_mt"), ("s_minSpeed", "min_speed_mt"), ("s_doTrick", "do_trick_mt")):
    assert f"DetourAttach(&(PVOID&){name}, {stand_in})" in source, name
# DoTrick's stand-in hands trick_allowed the trick, its one argument above the return address.
trick_body = re.search(r"__declspec\(naked\) void do_trick_mt\(\)\s*\{(.*?)\n\}", source, re.S).group(1)
assert "push   dword ptr [ebp + 8]" in trick_body and "add    esp, 8" in trick_body
assert "bool __cdecl trick_allowed(uint8_t* flyer, int trick)" in source

decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
pe = pefile.PE(str(Path(sys.argv[1]) / "BF2_modtools_NoDVD_NoConsole.exe"))
image = pe.get_memory_mapped_image()
base = pe.OPTIONAL_HEADER.ImageBase
assert base == 0x400000
for address, expected in checks.items():
    data = image[address - base:address - base + 96]
    actual = [(i.mnemonic, i.op_str) for i in decoder.disasm(data, address, count=len(expected))]
    assert actual == expected, (hex(address), actual, expected)
# DoTrick's switch on the trick (ebp*4 + 0x004F3FC4) sends SIDE_ROLL (6) to the SideRoll case.
assert int.from_bytes(image[0x004F3FC4 + 6 * 4 - base:0x004F3FC4 + 7 * 4 - base], "little") == 0x004F3E07
# The guards' bytes are the code's.
for va, text in ((0x004F4C10, "81EC84000000535556 8BF1"), (0x004F4630, "83EC2853555 68BE98B8508030000"),
                 (0x004F0950, "D9056CA0A200D981FC050000"), (0x004F0A10, "D9056CA0A200D981FC050000"),
                 (0x004F3D10, "D9056CA0A200568BF18B8E6C060000"), (0x004F09B0, "D9056CA0A200D981FC050000")):
    want = bytes.fromhex(text.replace(" ", ""))
    assert image[va - base:va - base + len(want)] == want, hex(va)
# The getters' 0.0, the zero the free-flight time is compared with.
assert struct.unpack("<f", image[0x00A2A06C - base:0x00A2A06C - base + 4])[0] == 0.0
# The squadron rule's constants: 30 bounding radii, an angle of -1 meaning any heading.
assert struct.unpack("<f", image[0x00A37E18 - base:0x00A37E18 - base + 4])[0] == 30.0
assert struct.unpack("<f", image[0x00A2A9E4 - base:0x00A2A9E4 - base + 4])[0] == -1.0
# The squadron guards' bytes are the code's, at the offsets install_squadrons checks them.
upf = 0x004F30C0
for offset, text in ((0x134, r"\xD9\x86\xE8\x1C\x00\x00"),
                     (0x153, r"\x8B\x8E\x6C\x06\x00\x00\xD9\x05\x6C\xA0\xA2\x00\xD9\x81\x08\x11\x00\x00"),
                     (0x17A, r"\x8B\x11\xFF\x52\x20\x8B\x40\x20\xD9\x80\x94\x00\x00\x00\xD8\x0D\x18\x7E\xA3\x00"),
                     (0x1E6, r"\x8B\x91\x04\x11\x00\x00")):
    want = bytes(int(b, 16) for b in text.split(r"\x")[1:])
    assert image[upf + offset - base:upf + offset - base + len(want)] == want, hex(offset)
    assert f'upf + 0x{offset:X}, "{text}"' in source, hex(offset)
find_guard = r"\x56\x8B\x35\xC4\x33\xAD\x00\x8B\x4E\x0C\x85\xC9"
assert image[0x005E2EB0 - base:0x005E2EB0 - base + 12] == bytes(int(b, 16) for b in find_guard.split(r"\x")[1:])
assert f'"{find_guard}", 12' in source
print(f"modtools: {len(checks)} instruction checks, the guards and the constants passed")
