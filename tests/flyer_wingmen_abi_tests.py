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
    "kForm_Name": 0x14,
    "kForm_Nodes": 0x18,
    "kForm_NodeCount": 0x1C,
    "kForm_Root": 0x28,
    "kForm_Flags": 0x2C,
    "kNodeSize": 0x1C,
    "kNode_Rotation": 0x0C,
    "kList_Next": 0x04,
    "kList_Payload": 0x0C,
    # Orders: the player's targets, what an AI's attack is aimed at, the comm keys' codes.
    "kCtl_Reticule": 0x164,
    "kCtl_ReticuleHandle": 0x168,
    "kReticuleChannels": 2,
    "kUC_Agent": 0x2C0,
    "kAgent_AttackTarget": 0x10,
    "kAgent_AttackHandle": 0x14,
    "kCommAttack": 6,
    "kCommDefend": 7,
    "kSafeHeightTaper": 1,
    "kStateLanded": 0,
    "kStateTakeoff": 1,
    "kStateLanding": 3,
    "kGroundMask": 0x190,
    # The primary weapon's ranges, and the fly agent's controller.
    "kFly_Weapons": 0x6B8,
    "kFlyWeapons": 4,
    "kFly_WeaponIndex": 0x6C8,
    "kWeapon_Class": 0x64,
    "kWClass_MinRange": 0x100,
    "kWClass_OptimalRange": 0x104,
    "kWClass_MaxRange": 0x108,
    "kAgent_Ctrl": 0x0C,
}.items():
    assert constant(source, name) == value, name
assert 'pbl_hash("EntitySoldier")' in source
assert 'static_assert(pbl_hash("game.squadcommand.follow") == 0x731FD493u' in source
# The orders' guards, as install_orders checks them.
for text in (r'"\x56\x8B\xF1\x57\x8B\xBE\xD4\x00\x00\x00", 10', r'"\x8B\x86\x38\x01\x00\x00", 6',
             r'"\x8B\x81\xC0\x02\x00\x00\x85\xC0\x74\x08\x8D\x48\x10\xE9", 14',
             r'"\x53\x55\x56\x8B\xF1\x8B\x86\x34\x02\x00\x00", 11',
             r'"\x8B\x44\x24\x0C\x83\xEC\x10\x56\x8B\x74\x24\x18\x69\xF6\x28\x03\x00\x00", 18',
             r'"\x8B\x44\x24\x04\x50\x68\x00\x80\x00\x00", 10',
             r'"\x83\xEC\x20\x8B\x41\x08\x55\x56\x57\x33\xFF\x69\xC0\x02\x01\x00\x00"',
             r'call - 0x13, "\x50\x57\x6A\x00\x6A\x00\x6A\x00", 8', r'call - 3, "\x50\x6A\x00", 3',
             r'call + 5, "\x83\xC4\x1C", 3', r'"\x55\x8B\xEC\x83\xEC\x20\x0F\xB6\x45\x08", 10',
             # Landing's: Land (or a detour's JMP), TakeOff, RayHit.
             r'"\x56\x8B\xF1\x8B\x86\xA4\x05\x00\x00", 9',
             r'"\x51\x56\x8B\xF1\x8B\x86\xA4\x05\x00\x00\x85\xC0\x57", 13',
             r'"\xD9\x05\x90\xA2\xA2\x00\x8B\x54\x24\x10", 10',
             # Waiting aboard: the fly agent's CALLs and SetExitVehicle.
             r'hint - 2, "\x8B\xCE", 2', r'hint + 5, "\x84\xC0\x74\x10\x8B\x16\x6A\x05", 8',
             r'"\x51\x56\x8B\xF1\xE8", 5', r'up1 - 0xF, "\x83\xB8\xA4\x05\x00\x00\x02", 7',
             r'up1 + 5, "\x5F\x5E\x5B\x8B\xE5\x5D\xC3", 7', r'up2 - 0x16, "\x83\xB8\xA4\x05\x00\x00\x02", 7',
             r'up2 + 5, "\x5F\x5E\x5B\x8B\xE5\x5D\xC3", 7',
             r'"\x80\x49\x74\x08\x8B\x44\x24\x04\x89\x41\x78\xC2\x04\x00", 14'):
    assert text in source, text
core = (SRC / "ai/flyer_wingmen_core.hpp").read_text()
assert constant(core, "kMaxClassFormations") == 10 and constant(core, "kMaxShapePoints") == 16
assert 'pbl_hash("WingmanFormation")' in source
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
         "flyer_do_trick", "flyer_mid_speed", "entity_formation_find", "flyer_update_path_follower",
         "entity_formation_list", "controllable_locked_target", "uc_force_attack_target", "object_affiliation",
         "hud_message", "hud_message_color", "localize_find", "team_order_input", "team_order_chat_call",
         "add_chat_message", "entity_flyer_take_off", "entity_flyer_land", "collision_ray_hit",
         "fly_agent_land_hint_call", "fly_agent_land_hint", "fly_agent_take_off_call", "fly_agent_take_off_call2",
         "ai_set_exit_vehicle")
EXPECT = (0x004F4C10, 0x004F4D4D, 0x004F4630, 0x004F0950, 0x004F0A10, 0x004F3D10, 0x004F09B0, 0x005E2EB0, 0x004F30C0,
          0x00AD33C0, 0x005E0C70, 0x0059EC20, 0x0055F840, 0x006AECD0, 0x00AE2170, 0x007F3720, 0x00745D00, 0x00745DC3,
          0x0070C260, 0x004F8B70, 0x004F1380, 0x0042E230, 0x005AFDD3, 0x005AED00, 0x005AF154, 0x005AF323,
          0x005A2170)
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
    # Which way a shape faces. EntityFormation::Read reads its nodes through
    # RedPath::ReadScope, whose Nodes reader (0x00820690) reads each node (0x00820470):
    # Position x, y and z NEGATED; Rotation's first number to +0x18 (s), the rest to
    # +0xC, +0x10, +0x14 (x, y, z).
    0x005E2C5B: [("mov", "ecx, esi"), ("call", "0x820840")],
    0x008209F7: [("call", "0x820690")],
    0x0082069E: [("cmp", "dword ptr [esi], 0x514e905a")],
    0x008207C7: [("call", "0x820470")],
    0x0082048E: [("cmp", "eax, 0x21ac415f"), ("je", "0x8204db"), ("cmp", "eax, 0x934f4e0a"), ("je", "0x8204a9")],
    0x008204A9: [("push", "0"), ("mov", "ecx, esi"), ("call", "0x401b72"), ("fstp", "dword ptr [edi]"), ("push", "1"),
                 ("mov", "ecx, esi"), ("call", "0x401b72"), ("fstp", "dword ptr [edi + 4]"), ("push", "2"),
                 ("mov", "ecx, esi"), ("call", "0x401b72"), ("fchs", ""), ("fstp", "dword ptr [edi + 8]")],
    0x008204DB: [("push", "0"), ("mov", "ecx, esi"), ("call", "0x401b72"), ("fstp", "dword ptr [edi + 0x18]"),
                 ("push", "1"), ("mov", "ecx, esi"), ("call", "0x401b72"), ("fstp", "dword ptr [edi + 0xc]"),
                 ("push", "2"), ("mov", "ecx, esi"), ("call", "0x401b72"), ("fstp", "dword ptr [edi + 0x10]"),
                 ("push", "3"), ("mov", "ecx, esi"), ("call", "0x401b72"), ("fstp", "dword ptr [edi + 0x14]")],
    # Read then takes the root node (+0x28) and its rotation's conjugate (x, y, z negated,
    # s kept) for the turn it drops.
    0x005E2C88: [("mov", "eax, dword ptr [esi + 0x28]"), ("lea", "edi, [esi + 0x18]"), ("push", "eax"),
                 ("mov", "ecx, edi"), ("call", "0x820160"), ("fld", "dword ptr [eax + 0x14]"), ("fchs", "")],
    0x005E2CB7: [("mov", "edx, dword ptr [eax + 0x18]")],
    # EntityFlyerClass's constructor: the squadron angle and distance 0 by default (EBX, the
    # 0 it also gives mBoostFlyerSpeed, +0x898, which Phantom's constructor sets to 0.0).
    0x004F9BB6: [("mov", "dword ptr [esi + 0x898], ebx")],
    0x004FA01E: [("mov", "dword ptr [esi + 0x1104], ebx"), ("mov", "dword ptr [esi + 0x1108], ebx")],
    # ---- Orders --------------------------------------------------------------------------
    # Controllable::GetTargetLockedObj, thiscall() on a Controllable: a player's (mPlayerId
    # +0xD4 not below 0) from its LockOnManager, kept at +0x138 with its handle at +0x13C,
    # the GameObject in EAX, plain RET.
    0x005E0C70: [("push", "esi"), ("mov", "esi, ecx"), ("push", "edi"), ("mov", "edi, dword ptr [esi + 0xd4]"),
                 ("test", "edi, edi"), ("jl", "0x5e0ceb")],
    0x005E0CEB: [("mov", "eax, dword ptr [esi + 0x138]"), ("test", "eax, eax"), ("je", "0x5e0d17"),
                 ("mov", "ecx, dword ptr [eax + 0x204]"), ("cmp", "ecx, dword ptr [esi + 0x13c]"), ("je", "0x5e0d19")],
    0x005E0D17: [("xor", "eax, eax"), ("pop", "edi"), ("pop", "esi"), ("ret", "")],
    # The Controllable's constructor clears the lock and both reticule targets (+0x164, its
    # handle +0x168; the second channel 8 bytes on).
    0x005E0492: [("mov", "dword ptr [esi + 0x138], ebx"), ("mov", "dword ptr [esi + 0x13c], ebx")],
    0x005E04B5: [("mov", "dword ptr [esi + 0x164], ebx"), ("mov", "dword ptr [esi + 0x168], ebx"),
                 ("lea", "eax, [esi + 0x164]"), ("add", "eax, 8"), ("mov", "dword ptr [eax], ebx"),
                 ("mov", "dword ptr [eax + 4], ebx")],
    # The trooper squad command reads the reticule target the same way, and asks
    # GetAffiliationToMe (thiscall(other) on its soldier) about it: below 0 an enemy, which
    # it will not recruit.
    0x0054985F: [("mov", "edi, dword ptr [ebx + 0x164]"), ("test", "edi, edi"), ("je", "0x549d07"),
                 ("mov", "eax, dword ptr [edi + 0x204]"), ("cmp", "eax, dword ptr [ebx + 0x168]")],
    0x0054988E: [("push", "edi"), ("mov", "ecx, esi"), ("call", "0x415e10"), ("test", "eax, eax"), ("jl", "0x549d07")],
    0x00415E10: [("jmp", "0x55f840")],
    0x0055F840: [("push", "ebx"), ("push", "ebp"), ("push", "esi"), ("mov", "esi, ecx"),
                 ("mov", "eax, dword ptr [esi + 0x234]")],
    0x0055F88D: [("ret", "4")],
    # UnitController::ForceSetAttackHelperTarget, thiscall(GameObject*), RET 4: its agent
    # (+0x2C0), whose AttackHelper (+0x10) AttackHelper::SetVisibleTarget sets: the target at
    # +0, its handle at +4.
    0x0059EC20: [("mov", "eax, dword ptr [ecx + 0x2c0]"), ("test", "eax, eax"), ("je", "0x59ec32"),
                 ("lea", "ecx, [eax + 0x10]"), ("jmp", "0x4166d0"), ("ret", "4")],
    0x004166D0: [("jmp", "0x5b43d0")],
    0x005B43D0: [("sub", "esp, 0xc"), ("push", "esi"), ("push", "edi"), ("mov", "esi, ecx"), ("call", "0x407aa9"),
                 ("mov", "edi, dword ptr [esp + 0x18]"), ("test", "edi, edi"), ("mov", "dword ptr [esi], edi"),
                 ("je", "0x5b43ee"), ("mov", "eax, dword ptr [edi + 0x204]")],
    0x005B43F0: [("push", "edi"), ("mov", "ecx, esi"), ("mov", "dword ptr [esi + 4], eax")],
    0x005B441B: [("ret", "4")],
    # The trooper squad command's message: "game.squadcommand.follow" looked up (cdecl(hash,
    # bool), the caller pops) ...
    0x00549A04: [("push", "0"), ("push", "0x731fd493"), ("call", "0x7f3720"), ("add", "esp, 8")],
    0x007F3720: [("mov", "eax, dword ptr [esp + 4]"), ("push", "eax"), ("push", "0x8000"), ("push", "0xd2d88c"),
                 ("call", "0x7e1a40"), ("add", "esp, 0xc"), ("test", "eax, eax"), ("jne", "0x7f374b"),
                 ("mov", "cl, byte ptr [esp + 8]"), ("test", "cl, cl"), ("setne", "al"), ("dec", "eax"),
                 ("and", "eax, 0xa73e0c"), ("ret", "")],
    # ... shown with its colour (the four bytes at 0x00AE2170, by value) ...
    0x00549A35: [("mov", "dl, byte ptr [0xae2170]"), ("push", "ecx"), ("mov", "eax, esp"),
                 ("mov", "byte ptr [eax], dl"), ("mov", "cl, byte ptr [0xae2171]"), ("mov", "byte ptr [eax + 1], cl"),
                 ("mov", "dl, byte ptr [0xae2172]"), ("mov", "byte ptr [eax + 2], dl"),
                 ("mov", "edx, dword ptr [esp + 0x18]"), ("mov", "cl, byte ptr [0xae2173]"), ("push", "edx"),
                 ("push", "0"), ("push", "esi"), ("mov", "byte ptr [eax + 3], cl"), ("call", "0x40c3bf"),
                 ("add", "esp, 8"), ("mov", "ecx, eax"), ("call", "0x40e5f7")],
    0x0040C3BF: [("jmp", "0x67e970")],
    0x0040E5F7: [("jmp", "0x67ebb0")],
    # ... through the player's HUD object, which for its first slot calls the message line
    # (0x00401EFB to 0x006AECD0) with the viewport, the text and the colour's address.
    0x0067EBEA: [("mov", "ecx, dword ptr [esp + 0xc]"), ("mov", "edx, dword ptr [esp]"),
                 ("lea", "eax, [esp + 0x10]"), ("push", "eax"), ("push", "ecx"), ("push", "edx"),
                 ("call", "0x401efb"), ("add", "esp, 0xc")],
    0x00401EFB: [("jmp", "0x6aecd0")],
    # The message line, cdecl(int player, const wchar_t* text, const RedColor* color): the
    # colour's event (+0x1A0 of the player's 0x328 bytes from 0x00BA3B68) sent when there is
    # one, then the text's (+0x19C), plain RET.
    0x006AECD0: [("mov", "eax, dword ptr [esp + 0xc]"), ("sub", "esp, 0x10"), ("push", "esi"),
                 ("mov", "esi, dword ptr [esp + 0x18]"), ("imul", "esi, esi, 0x328"), ("add", "esi, 0xba3b68"),
                 ("test", "eax, eax"), ("je", "0x6aed0f"), ("push", "eax"), ("mov", "eax, dword ptr [esi + 0x1a0]")],
    0x006AED0F: [("mov", "ecx, dword ptr [esp + 0x1c]"), ("mov", "edx, dword ptr [esi + 0x19c]"), ("push", "ecx"),
                 ("push", "edx"), ("lea", "ecx, [esp + 0x14]"), ("call", "0x414231"),
                 ("lea", "ecx, [esp + 0xc]"), ("call", "0x4098b3")],
    0x004098B3: [("jmp", "0x6ada90")],
    0x006AED3A: [("ret", "")],
    # HUD::GameEvents::Open registers those two: player%d.message (a string, 8) at
    # 0x00BA3D04 and player%d.message.color (7) at 0x00BA3D08.
    0x006AF75E: [("push", "1"), ("push", "0xa5f258"), ("push", "8"), ("mov", "dword ptr [0xba3d00], eax"),
                 ("call", "0x40a394"), ("add", "esp, 0x48"), ("push", "1"), ("push", "0xa5f23c"), ("push", "7"),
                 ("mov", "dword ptr [0xba3d04], eax"), ("call", "0x40a394"), ("push", "1"), ("push", "0xa5f21c"),
                 ("push", "1"), ("mov", "dword ptr [0xba3d08], eax")],
    # RawControllerInputs::TeamOrderInputProcess (thiscall, RET): the comm keys (rows 0x14 on,
    # 6 bytes each, in the control mode's 0x102 bytes) ...
    0x00745D00: [("sub", "esp, 0x20"), ("mov", "eax, dword ptr [ecx + 8]"), ("push", "ebp"), ("push", "esi"),
                 ("push", "edi"), ("xor", "edi, edi"), ("imul", "eax, eax, 0x102")],
    0x00745D51: [("lea", "esi, [eax + ecx + 0x2134]")],
    0x00745D8B: [("inc", "edi"), ("add", "esi, 6"), ("cmp", "edi, 8"), ("jl", "0x745d58")],
    # ... and for the one pressed, its text looked up and NetGame::AddChatMessageToQue called
    # with (false, the local index, 0, 0, the code, the text, the mask); the caller pops all
    # seven and keeps nothing in a register past the call.
    0x00745D9B: [("mov", "eax, dword ptr [0xadc760]"), ("mov", "ecx, dword ptr [esp + edi*4 + 0xc]"),
                 ("push", "eax"), ("push", "0"), ("push", "ecx"), ("call", "0x7f3720"), ("add", "esp, 8"),
                 ("push", "eax"), ("push", "edi"), ("push", "0"), ("push", "0"), ("push", "0"),
                 ("call", "0x413395"), ("add", "esp, 4"), ("push", "eax"), ("push", "0"), ("call", "0x410690"),
                 ("add", "esp, 0x1c"), ("pop", "edi"), ("pop", "esi"), ("pop", "ebp"), ("add", "esp, 0x20"),
                 ("ret", "")],
    0x00410690: [("jmp", "0x70c260")],
    0x0070C260: [("push", "ebp"), ("mov", "ebp, esp"), ("sub", "esp, 0x20"), ("movzx", "eax, byte ptr [ebp + 8]")],
    # ---- Landing with the player ----------------------------------------------------------
    # EntityFlyer::Land, thiscall() on the entity: flying, taking off or landing (2, 1, 3)
    # becomes landing (3); it ends in a tail call through its vtable (no arguments) ...
    0x004F1380: [("push", "esi"), ("mov", "esi, ecx"), ("mov", "eax, dword ptr [esi + 0x5a4]"), ("cmp", "eax, 2"),
                 ("push", "edi"), ("je", "0x4f1399"), ("cmp", "eax, 1"), ("je", "0x4f1399"), ("cmp", "eax, 3"),
                 ("jne", "0x4f13a3"), ("mov", "dword ptr [esi + 0x5a4], 3")],
    0x004F13EE: [("pop", "edi"), ("mov", "dword ptr [esi + 0x6d0], 0"), ("pop", "esi"),
                 ("jmp", "dword ptr [eax + 4]")],
    # ... and EntityFlyer::TakeOff, thiscall() on the entity, plain RET: from landed or landing
    # (0, 3) to taking off (1), its free-flight (+0x1CDC) and trick times zeroed. Lua's
    # EntityFlyerLand and EntityFlyerTakeOff call them on the entity.
    0x004F8B70: [("push", "ecx"), ("push", "esi"), ("mov", "esi, ecx"), ("mov", "eax, dword ptr [esi + 0x5a4]"),
                 ("test", "eax, eax"), ("push", "edi")],
    0x004F8B84: [("mov", "eax, dword ptr [esi + 0x5a4]"), ("test", "eax, eax"), ("je", "0x4f8b97"), ("cmp", "eax, 3"),
                 ("jne", "0x4f8c34")],
    0x004F8C00: [("mov", "eax, 1"), ("test", "al, cl"), ("mov", "dword ptr [esi + 0x5a4], eax"), ("jne", "0x4f8c23"),
                 ("mov", "dword ptr [esi + 0x1cdc], 0"), ("mov", "dword ptr [esi + 0x1ce0], 0")],
    0x004F8C82: [("pop", "edi"), ("pop", "esi"), ("pop", "ecx"), ("ret", "")],
    0x00472A42: [("mov", "ecx, esi"), ("call", "0x41306b")],
    0x0041306B: [("jmp", "0x4f1380")],
    0x004729D2: [("mov", "ecx, esi"), ("call", "0x408602")],
    0x00408602: [("jmp", "0x4f8b70")],
    # The flyer's own landing ray: RayHit (through 0x00407581) straight down 1024 m from it,
    # itself ignored, types 0x190, not ordnance only.
    0x004FEA8C: [("push", "edi"), ("push", "0x190")],
    0x004FEAD7: [("push", "0x44800000"), ("lea", "edx, [esp + 0x50]"), ("push", "edx"), ("push", "eax"),
                 ("call", "0x407581")],
    0x00407581: [("jmp", "0x42e230")],
    0x0042E230: [("fld", "dword ptr [0xa2a290]"), ("mov", "edx, dword ptr [esp + 0x10]"),
                 ("fld", "dword ptr [0xa2a06c]"), ("mov", "dword ptr [edx], 0")],
    # ---- The primary weapon's ranges ------------------------------------------------------
    # EntityFlyer::GetWeaponIndex(channel) and GetWeaponPtr(index), thiscall on its
    # Controllable (entity +0x240): mWeaponIndex at the entity's +0x6C8, mWeapon[] at +0x6B8.
    0x004D7880: [("mov", "eax, dword ptr [esp + 4]"), ("mov", "eax, dword ptr [ecx + eax*4 + 0x488]"), ("ret", "4")],
    0x004D78A0: [("mov", "eax, dword ptr [esp + 4]"), ("mov", "eax, dword ptr [ecx + eax*4 + 0x478]"), ("ret", "4")],
    # CombatUtil::GetWeaponOptimalRange and GetWeaponMinRange: the current weapon's class
    # (Weapon +0x64), its OptimalRange (+0x104) and MinRange (+0x100) ...
    0x005947B0: [("mov", "ecx, dword ptr [esp + 4]"), ("call", "0x416081"), ("test", "eax, eax"),
                 ("je", "0x5947c7"), ("mov", "eax, dword ptr [eax + 0x64]"), ("fld", "dword ptr [eax + 0x104]"),
                 ("ret", "")],
    0x00594810: [("mov", "ecx, dword ptr [esp + 4]"), ("call", "0x416081"), ("test", "eax, eax"),
                 ("je", "0x594827"), ("mov", "eax, dword ptr [eax + 0x64]"), ("fld", "dword ptr [eax + 0x100]"),
                 ("ret", "")],
    # ... as WeaponClass::SetProperty reads them from the ODF: MinRange at +0x100, MaxRange at
    # +0x108, OptimalRange at +0x104.
    0x0061EAD3: [("cmp", "edi, 0x1ef25b6c"), ("je", "0x61eb16")],
    0x0061EB16: [("add", "esi, 0x100")],
    0x0061EE54: [("cmp", "edi, 0x43bcebe6"), ("je", "0x61ef36")],
    0x0061EF36: [("mov", "edx, dword ptr [esp + 0x160]"), ("lea", "ecx, [esi + 0x108]")],
    0x0061F498: [("cmp", "edi, 0x778f9aae"), ("jne", "0x620594"), ("mov", "edx, dword ptr [esp + 0x160]"),
                 ("add", "esi, 0x104")],
    # ---- Waiting aboard -------------------------------------------------------------------
    # UnitFlyAgent::UpdateStatePostVision's FLY state: near a land hint (IsNearLandingPt,
    # thiscall() on the agent, AL) it goes to LAND (5) ...
    0x005AFDD1: [("mov", "ecx, esi"), ("call", "0x40e74b"), ("test", "al, al"), ("je", "0x5afdec"),
                 ("mov", "edx, dword ptr [esi]"), ("push", "5"), ("mov", "ecx, esi"), ("call", "dword ptr [edx + 0x1c]")],
    0x0040E74B: [("jmp", "0x5aed00")],
    0x005AED00: [("push", "ecx"), ("push", "esi"), ("mov", "esi, ecx"), ("call", "0x40dcd3"),
                 ("fcomp", "dword ptr [esi + 0x50]")],
    # ... (the agent's controller at +0xC) ...
    0x005AED1E: [("mov", "eax, dword ptr [esi + 0xc]")],
    # ... and LAND, once its flyer is down (state 0), to DROPOFF (6), which gets the pilot out.
    0x005AFE46: [("call", "0x40d0fd"), ("mov", "ecx, dword ptr [eax + 0x5a4]"), ("test", "ecx, ecx"),
                 ("jne", "0x5afe5e"), ("mov", "eax, dword ptr [esi]"), ("push", "6"), ("mov", "ecx, esi"),
                 ("call", "dword ptr [eax + 0x1c]")],
    # UnitFlyAgent::EnterState's TAKEOFF and FLY states take a flyer not flying up: TakeOff,
    # the flyer in ECX, the agent returning straight after.
    0x005AF145: [("cmp", "dword ptr [eax + 0x5a4], 2"), ("je", "0x5af512"), ("mov", "ecx, eax"),
                 ("call", "0x408602"), ("pop", "edi"), ("pop", "esi"), ("pop", "ebx"), ("mov", "esp, ebp"),
                 ("pop", "ebp"), ("ret", "")],
    0x005AF30D: [("cmp", "dword ptr [eax + 0x5a4], 2"), ("je", "0x5af512"), ("mov", "ecx, esi"),
                 ("call", "0x40d0fd"), ("mov", "ecx, eax"), ("call", "0x408602"), ("pop", "edi"), ("pop", "esi"),
                 ("pop", "ebx"), ("mov", "esp, ebp"), ("pop", "ebp"), ("ret", "")],
    # AILowLevel::SetExitVehicle, thiscall(CommandPost*), RET 4: the exit flag (8) and the post;
    # UnitController::UpdateHighLevel's end-of-game exit passes no post, on UnitController +0x2C4.
    0x005A2170: [("or", "byte ptr [ecx + 0x74], 8"), ("mov", "eax, dword ptr [esp + 4]"),
                 ("mov", "dword ptr [ecx + 0x78], eax"), ("ret", "4")],
    0x005A079E: [("push", "0"), ("lea", "ecx, [esi + 0x2c4]"), ("call", "0x40c342")],
    0x0040C342: [("jmp", "0x5a2170")],
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
# EntityFormation::Find's guard, built from the list's head: PUSH ESI; MOV ESI, [head + 4]; ...
find_guard = bytes([0x56, 0x8B, 0x35]) + (0x00AD33C0 + 4).to_bytes(4, "little") + bytes([0x8B, 0x4E, 0x0C, 0x85, 0xC9])
assert image[0x005E2EB0 - base:0x005E2EB0 - base + 12] == find_guard
assert "uint8_t find[12] = {0x56, 0x8B, 0x35, 0, 0, 0, 0, 0x8B, 0x4E, 0x0C, 0x85, 0xC9};" in source
assert "(uint32_t)(uintptr_t)(list + kList_Next)" in source
# The formations' list head as the exe starts: empty, its next and back its own address,
# no formation, none counted.
assert [int.from_bytes(image[0x00AD33C0 - base + 4 * k:0x00AD33C0 - base + 4 * k + 4], "little") for k in range(5)] == \
    [0x00AD33C0, 0x00AD33C0, 0x00AD33C0, 0, 0]
# Orders: the message line's events by name, the trooper commands' colour, and the
# localisation keys hashed as the game hashes its own.
def cstring(va):
    raw = image[va - base:va - base + 64]
    return raw[:raw.index(0)].decode("ascii")
assert cstring(0x00A5F258) == "player%d.message" and cstring(0x00A5F23C) == "player%d.message.color"
assert image[0x00AE2170 - base:0x00AE2170 - base + 4] == bytes.fromhex("dfdf20ff")
def pbl_hash(text):
    h = 0x811C9DC5
    for b in text.encode("ascii"):
        h = ((h ^ ((b if b < 0x80 else b - 0x100) & 0xFFFFFFFF | 0x20)) * 0x01000193) & 0xFFFFFFFF
    return h
assert pbl_hash("game.squadcommand.follow") == 0x731FD493
for key in ("formup", "engage", "attack", "cover", "notarget", "none", "targetdown", "targetlost"):
    assert f'pbl_hash("game.wingmen.{key}")' in source, key
assert 'pbl_hash("WingmanExitOnLand")' in source and 'pbl_hash("WingmanLand")' in source
# The keys a mission sets, as no stock ODF key hashes: none of their hashes is in
# the exe at all.
for key in ("Wingman", "WingmanRole", "WingmanStartOrder", "WingmanEngageRange"):
    assert f'pbl_hash("{key}")' in source, key
    assert struct.pack("<I", pbl_hash(key)) not in image, key
# A mission script's names: _GetEntity<GameObject> (0x00470FF0) searches
# EntityEx::mIdMap (0x00B7AD3C, 0x800 slots) with PblHashTableCode::_Find
# (0x007E1A40), as install_names calls it; _Find's prologue is install_names' guard.
assert constant(addrs, "entityex_id_map") == 0x00B7AD3C and constant(addrs, "pbl_hash_table_find") == 0x007E1A40
get_entity = [(i.mnemonic, i.op_str) for i in decoder.disasm(image[0x00471040 - base:0x00471040 - base + 16], 0x00471040, count=4)]
assert get_entity == [("push", "eax"), ("push", "0x800"), ("push", "0xb7ad3c"), ("call", "0x7e1a40")], get_entity
assert constant(source, "kIdMapSlots") == 0x800
find_guard = r"\x8B\x54\x24\x08\xD1\xFA\x56\x8B\x74\x24\x10"
assert image[0x007E1A40 - base:0x007E1A40 - base + 11] == bytes(int(b, 16) for b in find_guard.split(r"\x")[1:])
assert f'a->pbl_hash_table_find, "{find_guard}", 11' in source
# GameObject's RTTI name, which the game's own lookup checks the same way.
assert 'pbl_hash("GameObject")' in source
# The script's events are engine events of the character family.
events = (SRC / "lua/lua_events.cpp").read_text()
for name in ("WingmanJoin", "WingmanLost", "WingmenOrder", "WingmanLanded"):
    assert re.search(r'\{ "' + name + r'",\s+LuaEventFamily::CharacterGameObject \}', events), name
print(f"modtools: {len(checks)} instruction checks, the guards and the constants passed")
