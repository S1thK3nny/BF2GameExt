"""Read-only audit of everything squad formations read, call or redirect in all three supported game PEs.

Usage: python tests/squad_formation_abi_tests.py PATH_TO_GAMEDATA
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
source = (SRC / "ai/squad_formation.cpp").read_text()
core = (SRC / "ai/squad_formation_core.hpp").read_text()
addrs = (SRC / "core/game_addrs.hpp").read_text()
goclass = (SRC / "game/Battlefront2/Source/GameObjectClass.h").read_text()


def constant(text, name):
    match = re.search(r"\b" + name + r"\s*=\s*(-?0x[0-9a-fA-F]+|-?\d+)\s*;", text)
    assert match, name
    return int(match.group(1), 0)


for name, value in {
    "kSingleFile": 1,
    "kStockOpen": 6,
    "kGO_Class": 0x8,
    "kGO_HandleId": 0x204,
    "kSoldierControllable": 0x240,
    "kCtrl_Owner": 0x4,
    "kVt_IsRtti": 0x00,
    "kVt_GetVelocity": 0x44,
    "kNav_FollowTarget": 0x44,
    "kNav_FollowTargetId": 0x48,
    "kNav_FollowOffset": 0x4C,
    "kEv_Type": 0x34,
    "kEv_Object": 0x38,
    "kFollowChr": 0x10,
    "kFollowChrLoose": 0x11,
}.items():
    assert constant(source, name) == value, name
assert re.search(r"\bkAgentScale\s*=\s*2\.0f\s*;", core)
assert 'pbl_hash("EntitySoldier")' in source
for key in ("SquadLeader", "SquadSize", "SquadFormation", "SquadSpacing", "SquadFollower"):
    assert f'pbl_hash("{key}")' in source, key
assert constant(goclass, "kHeroClass") == 0x02
assert "mTargetFlags{0x36C, 0x328}" in goclass
# The slot and the turn go in together, as do the count and the recruit.
assert "if (!calls(offsetAt, s_reserveOffset) || !calls_through(turnAt, pointer))" in source
assert "if (!s_sendEvent || !calls(countAt, s_count) || !calls(recruitAt, s_sendEvent))" in source
assert "redirect(s_turnSite, turnAt, (const void*)&follow_transform, s_modtools ? 5 : 6);" in source

NAMES = ("follow_reserve_offset_call", "reserve_offset", "follow_target_transform_call",
         "follow_target_transform_ptr", "auto_follow_count_call", "reservation_count_for_object",
         "auto_follow_recruit_call", "unit_controller_send_event")


def namespace(text, name):
    body = re.search(r"namespace %s\s*\{(.*?)\n\s*\}\s*//\s*namespace %s" % (name, name), text, re.S).group(1)
    return {n: constant(body, n) for n in NAMES}


expected_addrs = {
    "modtools": (0x005B0AB1, 0x005C6840, 0x005A6A49, 0x00ADFC58, 0x0059ADE9, 0x005C6560, 0x0059AFC3, 0x0059F8A0),
    "steam": (0x006687BC, 0x0062FA70, 0x005ACC31, 0x0076B54C, 0x0061C1B2, 0x0062F760, 0x0061C3E1, 0x006640D0),
    "gog": (0x0066985C, 0x00630B10, 0x005ADBE1, 0x0076C548, 0x0061D222, 0x00630800, 0x0061D451, 0x00665170),
}
for build, values in expected_addrs.items():
    assert namespace(addrs, build) == dict(zip(NAMES, values)), build

mt_checks = {
    # UnitFollowAgent, tight follow: the scale (2.0, 2.2 under 4 m/s, 1.7 jet-hovering) ...
    0x005B0A51: [("mov", "dword ptr [esp + 0x14], 0x40000000")],
    0x005B0A5E: [("mov", "edx, dword ptr [eax]"), ("mov", "ecx, eax"), ("call", "dword ptr [edx + 0x44]")],
    0x005B0A7B: [("mov", "dword ptr [esp + 0x14], 0x400ccccd")],
    0x005B0A87: [("mov", "dword ptr [esp + 0x14], 0x3fd9999a")],
    # ... the layout helper, then ReserveOffset(out, leader, follower, type), cdecl ...
    0x005B0A8F: [("mov", "eax, dword ptr [esi + 0xc]"), ("mov", "ecx, dword ptr [eax + 4]"),
                 ("add", "ecx, 0x18"), ("call", "0x5afee0"), ("push", "eax"),
                 ("mov", "eax, dword ptr [ecx]"), ("call", "dword ptr [eax + 0x20]"), ("push", "eax"),
                 ("mov", "ecx, edi"), ("call", "0x40ddfa"), ("push", "eax"),
                 ("lea", "ecx, [esp + 0x48]"), ("push", "ecx"), ("call", "0x406e29")],
    # ... and the slot scaled by it, the four arguments and the scale's three popped together.
    0x005B0AB6: [("mov", "edx, dword ptr [esp + 0x24]"), ("push", "eax"), ("push", "edx"),
                 ("lea", "eax, [esp + 0x30]"), ("push", "eax"), ("call", "0x40f461")],
    0x005B0AC9: [("add", "esp, 0x1c")],
    # The layout helper: controller in EAX, 6 (wedge) above 10 m of open radius, else 2.
    0x005AFEE0: [("fld", "dword ptr [eax + 0x48]"), ("fcomp", "dword ptr [0xa33220]"), ("fnstsw", "ax"),
                 ("test", "ah, 0x41"), ("mov", "eax, 6"), ("je", "0x5afefa"), ("mov", "eax, 2"), ("ret", "")],
    0x00406E29: [("jmp", "0x5c6840")],
    # ReserveOffset: cdecl; Reserve(leader, follower, RT_FORMATION) first.
    0x005C6840: [("sub", "esp, 0x84"), ("push", "ebx"), ("push", "ebp"), ("mov", "ebp, dword ptr [esp + 0x98]"),
                 ("push", "esi"), ("push", "edi"), ("mov", "edi, dword ptr [esp + 0x9c]"), ("push", "3"),
                 ("push", "ebp"), ("push", "edi")],
    # The slot number: EBX takes the count of the leader's formation reservations before this
    # follower's own, and goes to [ESP+0x14] for the switch.
    0x005C68E5: [("cmp", "ebp, eax"), ("jne", "0x5c68ed"), ("mov", "ebx, dword ptr [esp + 0x3c]"),
                 ("inc", "dword ptr [esp + 0x3c]")],
    0x005C6917: [("mov", "dword ptr [esp + 0x14], ebx"), ("xor", "esi, esi"),
                 ("mov", "eax, dword ptr [esp + 0xa4]"), ("cmp", "eax, 6"), ("ja", "0x5c6e1c"),
                 ("jmp", "dword ptr [eax*4 + 0x5c6e38]")],
    # Single file: out = slot * (0, 0, -1).
    0x005C69FB: [("fild", "dword ptr [esp + 0x14]"), ("mov", "esi, dword ptr [esp + 0x98]"), ("push", "ecx"),
                 ("lea", "eax, [esp + 0x28]"), ("fstp", "dword ptr [esp]"), ("push", "eax"), ("push", "esi"),
                 ("mov", "dword ptr [esp + 0x30], 0"), ("mov", "dword ptr [esp + 0x34], 0"),
                 ("mov", "dword ptr [esp + 0x38], 0xbf800000")],
    # NavigatorFollowTarget::UpdateTarget: the leader handle (+0x44 object, +0x48 id), its matrix
    # from the +0xC interface, slot +0x40, and D3DXVec3TransformCoord(out, &mFollowOffset, matrix)
    # through the linked-in D3DX's dispatch pointer.
    0x005A6A23: [("mov", "ecx, dword ptr [eax + 0x204]"), ("cmp", "ecx, dword ptr [esi + 0x48]"),
                 ("je", "0x5a6a36"), ("mov", "dword ptr [esi + 0x44], ebx"), ("mov", "dword ptr [esi + 0x48], ebx"),
                 ("xor", "eax, eax"), ("mov", "edx, dword ptr [eax + 0xc]"), ("lea", "ecx, [eax + 0xc]"),
                 ("call", "dword ptr [edx + 0x40]"), ("push", "eax"), ("lea", "eax, [esi + 0x4c]"),
                 ("push", "eax"), ("lea", "ecx, [esp + 0x38]"), ("push", "ecx"), ("call", "0x7cb7b5")],
    0x007CB7B5: [("jmp", "dword ptr [0xadfc58]")],
    # CheckForAutoFollow: Controller::mOwner +4; the player must be a soldier (IsRtti, slot 0);
    # stop at two formation reservations; ask, then recruit with type 0x11.
    0x0059AD8A: [("mov", "eax, dword ptr [edi + 4]")],
    0x0059ADB4: [("mov", "ecx, dword ptr [0xb7e098]"), ("mov", "esi, eax"), ("mov", "eax, dword ptr [esi]"),
                 ("push", "ecx"), ("mov", "ecx, esi"), ("call", "dword ptr [eax]")],
    0x0059ADE2: [("push", "3"), ("sar", "edx, 0x1c"), ("push", "edx"), ("push", "esi"), ("call", "0x40200e"),
                 ("add", "esp, 0xc"), ("cmp", "eax, 2"), ("jge", "0x59b027")],
    0x0040200E: [("jmp", "0x5c6560")],
    0x0059AF8E: [("push", "esi"), ("push", "0xf"), ("lea", "ecx, [esp + 0x50]"), ("call", "0x407c61"),
                 ("lea", "eax, [esp + 0x48]"), ("push", "eax"), ("mov", "ecx, edi"), ("call", "0x40c216")],
    0x0059AFAA: [("push", "esi"), ("push", "0x11"), ("lea", "ecx, [esp + 0xa8]"), ("call", "0x407c61"),
                 ("lea", "ecx, [esp + 0xa0]"), ("push", "ecx"), ("mov", "ecx, edi"), ("call", "0x40c216"),
                 ("cmp", "eax, 1")],
    0x0040C216: [("jmp", "0x59f8a0")],
    0x0059F8A0: [("sub", "esp, 0x34"), ("push", "esi"), ("mov", "esi, ecx")],
    # EventInfo: the constructor stores the type at +0x34 and the object at +0x38 / +0x3C.
    0x0052AE1E: [("mov", "dword ptr [esi + 0x34], ecx"), ("mov", "dword ptr [esi + 0x38], eax")],
    0x0052AE30: [("mov", "dword ptr [esi + 0x3c], eax")],
    # UnitAgent::HandleSquadCommandEvent: 0x10 and 0x11 both follow; tight when 0x10.
    0x005A9EE9: [("cmp", "eax, 0x10"), ("je", "0x5aa05c"), ("cmp", "eax, 0x11"), ("je", "0x5aa059")],
    0x005AA059: [("cmp", "eax, 0x10"), ("mov", "edx, dword ptr [esi + 0x3c]"), ("sete", "al")],
    # Team::AddSpecialClass: the hero class bit.
    0x00662EE0: [("mov", "eax, dword ptr [ecx + 0x48]"), ("cmp", "eax, dword ptr [ecx + 0x44]"),
                 ("jge", "0x662f1d"), ("mov", "eax, dword ptr [esp + 4]"), ("or", "byte ptr [eax + 0x36c], 2")],
}
mt_floats = {0x00A33220: 10.0, 0x00A35A1C: 16.0}   # 16: the squared speed of 4 m/s
mt_tables = {(0x005C6E38, 1): 0x005C69FB}
mt_rtti = {"EntitySoldier": 0x00B7E098}


def retail(follow, helper, deref, reserve, table, single, xmm6, zero, turn, iat, check, count, recruit, ctor,
           send, squad, helper_ten, scales, soldier, entry, special):
    """The checks Steam and GOG share, at each build's addresses."""
    return {
        # Tight follow: the scale from constants, the leader's velocity at GameObject vptr +0x44 ...
        follow - 0x97: [("lea", "edi, [ebx + 0x218]"), ("movss", f"xmm0, dword ptr [{scales[0]:#x}]")],
        follow - 0x7C: [("mov", "ecx, eax"), ("mov", "edx, dword ptr [eax]"), ("call", "dword ptr [edx + 0x44]")],
        follow - 0x46: [("movss", f"xmm0, dword ptr [{scales[1]:#x}]")],
        follow - 0x31: [("movss", f"xmm0, dword ptr [{scales[2]:#x}]")],
        # ... the helper (controller in ECX; EDX, the follower's Controllable, live across it) and
        # ReserveOffset with ECX = out, EDX = leader, follower and type pushed, the caller popping.
        follow - 0x23: [("mov", "ecx, dword ptr [ebx + 0xc]"), ("mov", "edx, dword ptr [ecx + 4]"),
                        ("call", f"{helper:#x}"), ("push", "eax"), ("mov", "eax, dword ptr [edx + 0x18]"),
                        ("lea", "ecx, [edx + 0x18]"), ("call", "dword ptr [eax + 0x20]"), ("push", "eax"),
                        ("mov", "ecx, edi"), ("call", f"{deref:#x}"), ("mov", "edx, eax"),
                        ("lea", "ecx, [esp + 0x2c]"), ("call", f"{reserve:#x}"),
                        ("movss", "xmm1, dword ptr [esp + 0x1c]"), ("add", "esp, 8")],
        helper: [("movss", "xmm0, dword ptr [ecx + 0x48]"), ("xor", "eax, eax"),
                 ("comiss", f"xmm0, dword ptr [{helper_ten:#x}]"), ("seta", "al"),
                 ("lea", "eax, [eax*4 + 2]"), ("ret", "")],
        reserve: [("push", "ebp"), ("mov", "ebp, esp"), ("sub", "esp, 0x28"), ("push", "esi"), ("mov", "esi, edx"),
                  ("mov", "edx, dword ptr [ebp + 8]"), ("push", "edi"), ("mov", "edi, ecx")],
        reserve + 0x12: [("push", "3"), ("mov", "ecx, esi")],
        reserve + 0x26: [("movss", f"xmm6, dword ptr [{xmm6:#x}]")],
        # The slot number: the count of the leader's formation reservations before this
        # follower's own ([ebp - 4]), out of all of them ([ebp - 0x10]).
        reserve + 0xAE: [("cmp", "dword ptr [ebp + 8], ecx"), ("mov", "eax, dword ptr [ebp - 4]"),
                         ("mov", "ecx, dword ptr [ebp - 0x10]"), ("cmove", "eax, ecx"), ("inc", "ecx"),
                         ("mov", "dword ptr [ebp - 4], eax"), ("mov", "dword ptr [ebp - 0x10], ecx")],
        # At the switch: ECX the slot number, EDX the type.
        reserve + 0xF0: [("mov", "ecx, dword ptr [ebp - 4]"), ("mov", "edi, dword ptr [ebp - 0x18]"),
                         ("mov", "esi, dword ptr [ebp - 0x14]"), ("mov", "eax, dword ptr [ebp - 0x10]"),
                         ("mov", "edx, dword ptr [ebp + 0xc]"), ("cmp", "edx, 6"), ("ja", f"{reserve + 0x41D:#x}"),
                         ("jmp", f"dword ptr [edx*4 + {table:#x}]")],
        # Single file: (slot * 0, slot * 0, slot * -1) into out, which comes back in EAX.
        single: [("movd", "xmm1, ecx"), ("mov", "eax, edi"), ("cvtdq2ps", "xmm1, xmm1"), ("movaps", "xmm0, xmm1"),
                 ("mulss", "xmm1, xmm6"), ("mulss", f"xmm0, dword ptr [{zero:#x}]"),
                 ("movss", "dword ptr [edi + 8], xmm1"), ("movss", "dword ptr [edi], xmm0"),
                 ("movss", "dword ptr [edi + 4], xmm0"), ("pop", "edi"), ("pop", "esi"), ("mov", "esp, ebp"),
                 ("pop", "ebp"), ("ret", "")],
        # UpdateTarget: the leader handle (+0x44 object, +0x48 id), its matrix, and the import
        # D3DXVec3TransformCoord(out, &mFollowOffset, matrix).
        turn - 0x31: [("test", "ecx, ecx"), ("je", f"{turn - 0x14:#x}"), ("mov", "eax, dword ptr [ecx + 0x204]"),
                      ("cmp", "eax, dword ptr [edi + 0x48]"), ("je", f"{turn - 0x12:#x}"),
                      ("mov", "dword ptr [edi + 0x44], 0"), ("mov", "dword ptr [edi + 0x48], 0"),
                      ("xor", "ecx, ecx"), ("mov", "eax, dword ptr [ecx + 0xc]"), ("add", "ecx, 0xc"),
                      ("call", "dword ptr [eax + 0x40]"), ("push", "eax"), ("lea", "eax, [edi + 0x4c]"),
                      ("push", "eax"), ("lea", "eax, [ebp - 0x1c]"), ("push", "eax"),
                      ("call", f"dword ptr [{iat:#x}]")],
        # CheckForAutoFollow: Controller::mOwner +4; the player must be a soldier; its count with
        # ECX = obj, EDX = team, the type pushed and popped by the caller; then the stop.
        entry + 0x13: [("mov", "ecx, dword ptr [esi + 4]")],
        entry + 0x3A: [("push", f"dword ptr [{soldier:#x}]"), ("mov", "ebx, eax"), ("mov", "ecx, ebx"),
                       ("mov", "edx, dword ptr [ebx]"), ("mov", "edx, dword ptr [edx]"), ("call", "edx")],
        check: [("push", "3"), ("call", f"{count:#x}"), ("add", "esp, 4"), ("cmp", "eax, 2"),
                ("jge", f"{check + 0x252:#x}")],
        check - 0xE: [("mov", "edx, dword ptr [ebx + 0x234]"), ("mov", "ecx, ebx")],
        count: [("push", "ebp"), ("mov", "ebp, esp"), ("sub", "esp, 0xc"), ("mov", "dword ptr [ebp - 8], ecx")],
        count + 0x13: [("mov", "dword ptr [ebp - 0xc], edx")],
        # Ask whether it already follows (0xf), then recruit with 0x11 through SendEvent.
        recruit - 0x37: [("push", "ebx"), ("push", "0xf"), ("lea", "ecx, [ebp - 0xb0]"), ("call", f"{ctor:#x}"),
                         ("lea", "eax, [ebp - 0xb0]"), ("mov", "ecx, esi"), ("push", "eax"), ("call", f"{send:#x}")],
        recruit - 0x17: [("push", "ebx"), ("push", "0x11"), ("lea", "ecx, [ebp - 0x108]"), ("call", f"{ctor:#x}"),
                         ("lea", "eax, [ebp - 0x108]"), ("mov", "ecx, esi"), ("push", "eax"), ("call", f"{send:#x}"),
                         ("cmp", "eax, 1")],
        send: [("push", "ebp"), ("mov", "ebp, esp"), ("sub", "esp, 0x34"), ("push", "esi")],
        send + 0x8D: [("ret", "4")],
        # EventInfo: the type at +0x34, the object at +0x38 and its id at +0x3C; thiscall, RET 8.
        ctor + 0x4A: [("mov", "eax, dword ptr [ebp + 8]"), ("mov", "dword ptr [ecx + 0x34], eax"),
                      ("mov", "eax, dword ptr [ebp + 0xc]"), ("mov", "dword ptr [ecx + 0x38], eax")],
        ctor + 0x64: [("mov", "dword ptr [ecx + 0x3c], eax")],
        ctor + 0x96: [("ret", "8")],
        # UnitAgent::HandleSquadCommandEvent: 0x10 and 0x11 both follow; tight when 0x10.
        squad: [("cmp", "eax, 0x10"), ("je", f"{squad + 0x180:#x}"), ("cmp", "eax, 0x11"),
                ("je", f"{squad + 0x17D:#x}")],
        squad + 0x17D: [("cmp", "eax, 0x10"), ("push", "dword ptr [esi + 0x3c]"),
                        ("sete", "byte ptr [ebp + 8]")],
        # Team::AddSpecialClass: the hero class bit.
        special + 0x15: [("or", "byte ptr [edx + 0x328], 2")],
    }


steam_checks = retail(follow=0x006687BC, helper=0x00667FC0, deref=0x0043A150, reserve=0x0062FA70,
                      table=0x0062FEAC, single=0x0062FBF1, xmm6=0x007B253C, zero=0x007B1E5C,
                      turn=0x005ACC31, iat=0x0076B54C, check=0x0061C1B0, count=0x0062F760, recruit=0x0061C3E1,
                      ctor=0x0043EDF0, send=0x006640D0, squad=0x0065E8A8, helper_ten=0x007B228C,
                      scales=(0x007B21D8, 0x007B21F0, 0x007B2188), soldier=0x01EBC054, entry=0x0061C150,
                      special=0x00653F00)
gog_checks = retail(follow=0x0066985C, helper=0x00669060, deref=0x0043A140, reserve=0x00630B10,
                    table=0x00630F4C, single=0x00630C91, xmm6=0x007B34B4, zero=0x007B2DD4,
                    turn=0x005ADBE1, iat=0x0076C548, check=0x0061D220, count=0x00630800, recruit=0x0061D451,
                    ctor=0x0043EDE0, send=0x00665170, squad=0x0065F948, helper_ten=0x007B3204,
                    scales=(0x007B3150, 0x007B3168, 0x007B3100), soldier=0x01EBD558, entry=0x0061D1C0,
                    special=0x00654FA0)
steam_floats = {0x007B21D8: 2.0, 0x007B21F0: 2.2, 0x007B2188: 1.7, 0x007B22B8: 16.0, 0x007B228C: 10.0,
                0x007B253C: -1.0, 0x007B1E5C: 0.0}
gog_floats = {0x007B3150: 2.0, 0x007B3168: 2.2, 0x007B3100: 1.7, 0x007B3230: 16.0, 0x007B3204: 10.0,
              0x007B34B4: -1.0, 0x007B2DD4: 0.0}
steam_tables = {(0x0062FEAC, 1): 0x0062FBF1}
gog_tables = {(0x00630F4C, 1): 0x00630C91}
# The import UpdateTarget turns the slot with.
steam_imports = {0x0076B54C: "D3DXVec3TransformCoord"}
gog_imports = {0x0076C548: "D3DXVec3TransformCoord"}
steam_rtti = {"EntitySoldier": 0x01EBC054}
gog_rtti = {"EntitySoldier": 0x01EBD558}

builds = [
    ("modtools", "BF2_modtools_NoDVD_NoConsole.exe", mt_checks, mt_floats, mt_tables, {}, mt_rtti),
    ("steam", "BattlefrontII.exe", steam_checks, steam_floats, steam_tables, steam_imports, steam_rtti),
    ("gog", "BattlefrontII_GoG.exe", gog_checks, gog_floats, gog_tables, gog_imports, gog_rtti),
]
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)


def rtti_globals(image, base, name):
    """The globals PblHash is built into from a class name: PUSH "<name>" / MOV ECX,<global> / CALL.
    Modtools hashes "EntitySoldier" into two, which hold the same value."""
    found = []
    start = 0
    while (text := image.find(name.encode() + b"\0", start)) >= 0:   # a name can be stored twice
        push = 0
        while (push := image.find(b"\x68" + struct.pack("<I", base + text), push)) >= 0:   # and pushed for more
            if image[push + 5] == 0xB9:
                found.append(struct.unpack("<I", image[push + 6:push + 10])[0])
            push += 1
        start = text + 1
    return found


for name, filename, checks, floats, tables, imports, rtti in builds:
    pe = pefile.PE(str(Path(sys.argv[1]) / filename))
    try:
        image = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        assert base == 0x400000
        for address, expected in checks.items():
            data = image[address - base:address - base + 128]
            actual = [(i.mnemonic, i.op_str) for i in decoder.disasm(data, address, count=len(expected))]
            assert actual == expected, (name, hex(address), actual, expected)
        for address, value in floats.items():
            actual = struct.unpack("<f", image[address - base:address - base + 4])[0]
            assert abs(actual - value) < 1e-6, (name, hex(address), actual, value)
        for (table, index), target in tables.items():
            entry = struct.unpack("<I", image[table + 4 * index - base:table + 4 * index - base + 4])[0]
            assert entry == target, (name, hex(table), index, hex(entry))
        if imports:
            found = {imp.address: imp.name.decode() for dll in pe.DIRECTORY_ENTRY_IMPORT
                     for imp in dll.imports if imp.name}
            for address, symbol in imports.items():
                assert found.get(address) == symbol, (name, hex(address), found.get(address))
        for cls, address in rtti.items():
            assert address in rtti_globals(image, base, cls), (name, cls)
        print(f"{name}: {len(checks)} instruction checks, {len(floats)} constants, "
              f"{len(tables)} jump table entries, {len(imports)} imports and the RTTI name passed")
    finally:
        pe.close()
