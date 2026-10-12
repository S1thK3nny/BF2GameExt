"""Read-only audit of everything AI squads read, call or detour in all three supported game PEs.

Usage: python tests/ai_squads_abi_tests.py PATH_TO_GAMEDATA
Requires pefile and capstone. Does not execute the game or modify its files.
These are instruction/layout regression checks, not an in-game behaviour test.
The follow events themselves are audited by tests/squad_formation_abi_tests.py.
"""
import re
import struct
import sys
from pathlib import Path

import capstone
import pefile


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "PatcherDLL/src"
source = (SRC / "ai/ai_squads.cpp").read_text()
formation = (SRC / "ai/squad_formation.cpp").read_text()
addrs = (SRC / "core/game_addrs.hpp").read_text()


def constant(text, name):
    match = re.search(r"\b" + name + r"\s*=\s*(-?0x[0-9a-fA-F]+|-?\d+)\s*;", text)
    assert match, name
    return int(match.group(1), 0)


for name, value in {
    "kNodeNext": 0x04,
    "kNodePayload": 0x0C,
    "kVt_IsRtti": 0x00,
    "kCtrl_Owner": 0x04,
    "kOwner_Trackable": 0x18,
    "kVt_GetTrackable": 0x20,
    "kUC_State": 0x7C,
    "kUC_Agent": 0x2C0,
    "kGO_MatrixTrans": 0x120,
    "kGO_CurHealth": 0x144,
    "kGO_Flags": 0x1FC,
    "kGO_HandleId": 0x204,
    "kGO_Team": 0x234,
    "kStateCombat": 2,
    "kFollowChr": 0x10,
    "kIsFollowingChr": 0x0F,
}.items():
    assert constant(source, name) == value, name
for cls in ("UnitController", "UnitFollowAgent", "EntitySoldier"):
    assert f'pbl_hash("{cls}")' in source, cls
assert "(*(const uint32_t*)(obj + kGO_Flags) >> 3) & 1u" in source           # alive: bit 3
assert "(int32_t)(*(const uint32_t*)(obj + kGO_Team) << 28) >> 28" in source  # team: the low 4 bits, signed
# The prologues the install guards, as the source spells them.
assert r'"\x55\x8B\xEC\x83\xE4\xF8\x83\xEC\x0C\x53\x56\x8B\x35\x00\x00\x00\x00\x57\xF3\x0F\x11\x44\x24\x0C"' in source
assert '"xxxxxxxxxxxxx????xxxxxxx"' in source
assert r'"\x51\xA1\x00\x00\x00\x00\x53\x55\x56\x8B\x70\x04", "xx????xxxxxx"' in source
assert r'"\x83\xEC\x34\x56\x8B\xF1\x8B\x8E\xC0\x02\x00\x00", 12' in formation
assert r'"\x83\xEC\x0C\x56\x57\x33\xFF\x83\xEC\x08", 10' in formation
assert r'"\x55\x8B\xEC\x83\xEC\x34\x56\x8B\xF1\x8B\x8E\xC0\x02\x00\x00", 15' in formation
assert r'"\x55\x8B\xEC\x83\xEC\x10\x83\xEC\x08", 9' in formation
assert constant(formation, "kEventInfoSize") == 0x58

NAMES = ("controller_manager_update", "controller_list", "event_info_ctor", "unit_controller_send_event")


def namespace(text, name):
    body = re.search(r"namespace %s\s*\{(.*?)\n\s*\}\s*//\s*namespace %s" % (name, name), text, re.S).group(1)
    return {n: constant(body, n) for n in NAMES}


assert namespace(addrs, "modtools") == dict(zip(NAMES, (0x005997A0, 0x00B8EE6C, 0x0052ADE0, 0x0059F8A0)))
assert namespace(addrs, "steam") == dict(zip(NAMES, (0x00486370, 0x01E30930, 0x0043EDF0, 0x006640D0)))
assert namespace(addrs, "gog") == dict(zip(NAMES, (0x00486370, 0x01E31DDC, 0x0043EDE0, 0x00665170)))

mt_checks = {
    # ControllerManager::Update, cdecl(float): every controller in the list at [0xB8EE6C], the
    # first node at +4, each node's controller at +0xC and the next at +4, updated through
    # vptr +0x14 with dt.
    0x005997A0: [("push", "ecx"), ("mov", "eax, dword ptr [0xb8ee6c]"), ("push", "ebx"), ("push", "ebp"),
                 ("push", "esi"), ("mov", "esi, dword ptr [eax + 4]"), ("mov", "ecx, dword ptr [esi + 0xc]"),
                 ("test", "ecx, ecx"), ("je", "0x5997c7"), ("mov", "eax, dword ptr [esp + 0x14]"),
                 ("mov", "edx, dword ptr [ecx]"), ("mov", "esi, dword ptr [esi + 4]"), ("push", "eax"),
                 ("call", "dword ptr [edx + 0x14]"), ("mov", "ecx, dword ptr [esi + 0xc]"),
                 ("test", "ecx, ecx"), ("jne", "0x5997b3")],
    # UnitController::UpdateHighLevel: the soldier through the owner's +0x18 interface, vptr
    # +0x20, and bit 3 of +0x1FC (the Damageable at +0x140, its word at +0xBC) for alive.
    0x005A03B1: [("mov", "edx, dword ptr [eax + 0x18]"), ("lea", "ecx, [eax + 0x18]"),
                 ("call", "dword ptr [edx + 0x20]"), ("add", "eax, 0x140"), ("mov", "eax, dword ptr [eax + 0xbc]"),
                 ("shr", "eax, 3")],
    # Lua GetObjectHealth: mCurHealth +0x144.
    0x004720E1: [("mov", "eax, dword ptr [esi + 0x144]")],
    # UnitController::IsInCombat: state +0x7C is 2.
    0x0059EA10: [("mov", "edx, dword ptr [ecx + 0x7c]"), ("xor", "eax, eax"), ("cmp", "edx, 2"),
                 ("sete", "al"), ("ret", "")],
    # SendEvent: the agent at +0x2C0, its EventHandler at vptr +0x50.
    0x0059F8A0: [("sub", "esp, 0x34"), ("push", "esi"), ("mov", "esi, ecx"), ("mov", "ecx, dword ptr [esi + 0x2c0]"),
                 ("xor", "eax, eax"), ("test", "ecx, ecx"), ("je", "0x59f943"), ("mov", "eax, dword ptr [ecx]"),
                 ("push", "ebx"), ("push", "edi"), ("mov", "edi, dword ptr [esp + 0x44]"), ("push", "edi"),
                 ("call", "dword ptr [eax + 0x50]")],
    # EventInfo's constructor: thiscall, RET 8.
    0x0052ADE0: [("sub", "esp, 0xc"), ("push", "esi"), ("push", "edi"), ("xor", "edi, edi"), ("sub", "esp, 8")],
    0x0052AE45: [("pop", "edi"), ("mov", "eax, esi"), ("pop", "esi"), ("add", "esp, 0xc"), ("ret", "8")],
    # UnitFollowAgent::IsRtti, slot 0 of its vtable.
    0x005B00C0: [("mov", "eax, dword ptr [esp + 4]"), ("cmp", "dword ptr [0xb8f20c], eax")],
    0x0040D5B2: [("jmp", "0x5b00c0")],
    # CheckForAutoFollow: the team, the low 4 bits of +0x234, signed.
    0x0059ADD9: [("mov", "edx, dword ptr [esi + 0x234]"), ("shl", "edx, 0x1c"), ("push", "3"), ("sar", "edx, 0x1c")],
}
mt_slots = [(0x00A48810, 0x00, 0x005B00C0)]
mt_rtti = {"UnitController": 0x00B8EFBC, "UnitFollowAgent": 0x00B8F20C, "EntitySoldier": 0x00B7E098}


def retail(update, list_global, high_level, health, combat, send, ctor, follow_rtti, follow_global, team):
    return {
        # ControllerManager::Update: dt in XMM0, no stack argument; the same list walk.
        update: [("push", "ebp"), ("mov", "ebp, esp"), ("and", "esp, 0xfffffff8"), ("sub", "esp, 0xc"),
                 ("push", "ebx"), ("push", "esi"), ("mov", f"esi, dword ptr [{list_global:#x}]"), ("push", "edi"),
                 ("movss", "dword ptr [esp + 0xc], xmm0"), ("mov", "esi, dword ptr [esi + 4]"),
                 ("mov", "ecx, dword ptr [esi + 0xc]"), ("test", "ecx, ecx"), ("je", f"{update + 0x3D:#x}"),
                 ("mov", "eax, dword ptr [ecx]"), ("mov", "esi, dword ptr [esi + 4]"), ("push", "ecx"),
                 ("movss", "dword ptr [esp], xmm0"), ("call", "dword ptr [eax + 0x14]"),
                 ("mov", "ecx, dword ptr [esi + 0xc]")],
        high_level: [("mov", "ecx, dword ptr [esi + 4]"), ("add", "ecx, 0x18"), ("mov", "eax, dword ptr [ecx]"),
                     ("call", "dword ptr [eax + 0x20]"), ("mov", "eax, dword ptr [eax + 0x1fc]"), ("shr", "eax, 3"),
                     ("test", "al, 1")],
        health: [("movss", "xmm0, dword ptr [esi + 0x144]")],
        combat: [("cmp", "dword ptr [ecx + 0x7c], 2"), ("sete", "al"), ("ret", "")],
        send: [("push", "ebp"), ("mov", "ebp, esp"), ("sub", "esp, 0x34"), ("push", "esi"), ("mov", "esi, ecx"),
               ("mov", "ecx, dword ptr [esi + 0x2c0]"), ("test", "ecx, ecx"), ("je", f"{send + 0x90:#x}"),
               ("mov", "eax, dword ptr [ecx]"), ("push", "ebx"), ("mov", "ebx, dword ptr [ebp + 8]"), ("push", "edi"),
               ("push", "ebx"), ("call", "dword ptr [eax + 0x50]")],
        ctor: [("push", "ebp"), ("mov", "ebp, esp"), ("sub", "esp, 0x10"), ("sub", "esp, 8")],
        follow_rtti: [("push", "ebp"), ("mov", "ebp, esp"), ("mov", "eax, dword ptr [ebp + 8]"),
                      ("cmp", f"dword ptr [{follow_global:#x}], eax")],
        team: [("mov", "edx, dword ptr [ebx + 0x234]"), ("mov", "ecx, ebx"), ("shl", "edx, 0x1c"),
               ("sar", "edx, 0x1c")],
    }


steam_checks = retail(update=0x00486370, list_global=0x01E30930, high_level=0x006639A7, health=0x0058E59D,
                      combat=0x00663B92, send=0x006640D0, ctor=0x0043EDF0, follow_rtti=0x00667F00,
                      follow_global=0x01F993E8, team=0x0061C1A2)
gog_checks = retail(update=0x00486370, list_global=0x01E31DDC, high_level=0x00664A47, health=0x0058F53D,
                    combat=0x00664C32, send=0x00665170, ctor=0x0043EDE0, follow_rtti=0x00668FA0,
                    follow_global=0x01F9A89C, team=0x0061D212)
steam_slots = [(0x007AFB30, 0x00, 0x00667F00)]
gog_slots = [(0x007B0AA8, 0x00, 0x00668FA0)]
steam_rtti = {"UnitController": 0x01F99384, "UnitFollowAgent": 0x01F993E8, "EntitySoldier": 0x01EBC054}
gog_rtti = {"UnitController": 0x01F9A838, "UnitFollowAgent": 0x01F9A89C, "EntitySoldier": 0x01EBD558}

builds = [
    ("modtools", "BF2_modtools_NoDVD_NoConsole.exe", mt_checks, mt_slots, mt_rtti),
    ("steam", "BattlefrontII.exe", steam_checks, steam_slots, steam_rtti),
    ("gog", "BattlefrontII_GoG.exe", gog_checks, gog_slots, gog_rtti),
]
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

# The install guards' bytes against each image, masked where an address sits.
UPDATE_RT = (b"\x55\x8B\xEC\x83\xE4\xF8\x83\xEC\x0C\x53\x56\x8B\x35\x00\x00\x00\x00\x57\xF3\x0F\x11\x44\x24\x0C",
             "xxxxxxxxxxxxx????xxxxxxx")
SEND_RT = (b"\x55\x8B\xEC\x83\xEC\x34\x56\x8B\xF1\x8B\x8E\xC0\x02\x00\x00", "x" * 15)
CTOR_RT = (b"\x55\x8B\xEC\x83\xEC\x10\x83\xEC\x08", "x" * 9)
guards = {
    "modtools": [(0x005997A0, b"\x51\xA1\x00\x00\x00\x00\x53\x55\x56\x8B\x70\x04", "xx????xxxxxx"),
                 (0x0059F8A0, b"\x83\xEC\x34\x56\x8B\xF1\x8B\x8E\xC0\x02\x00\x00", "x" * 12),
                 (0x0052ADE0, b"\x83\xEC\x0C\x56\x57\x33\xFF\x83\xEC\x08", "x" * 10)],
    "steam": [(0x00486370,) + UPDATE_RT, (0x006640D0,) + SEND_RT, (0x0043EDF0,) + CTOR_RT],
    "gog": [(0x00486370,) + UPDATE_RT, (0x00665170,) + SEND_RT, (0x0043EDE0,) + CTOR_RT],
}


def follow(image, base, address):
    """An incremental-link thunk (JMP rel32) to its target."""
    code = image[address - base:address - base + 5]
    if code[0] == 0xE9:
        return address + 5 + struct.unpack("<i", code[1:5])[0]
    return address


def rtti_globals(image, base, name):
    """The globals PblHash is built into from a class name: PUSH "<name>" / MOV ECX,<global> / CALL."""
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


for name, filename, checks, slots, rtti in builds:
    pe = pefile.PE(str(Path(sys.argv[1]) / filename))
    try:
        image = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        assert base == 0x400000
        for address, expected in checks.items():
            data = image[address - base:address - base + 128]
            actual = [(i.mnemonic, i.op_str) for i in decoder.disasm(data, address, count=len(expected))]
            assert actual == expected, (name, hex(address), actual, expected)
        for table, offset, function in slots:
            entry = struct.unpack("<I", image[table + offset - base:table + offset - base + 4])[0]
            assert follow(image, base, entry) == function, (name, hex(table), hex(offset), hex(entry))
        for cls, address in rtti.items():
            assert address in rtti_globals(image, base, cls), (name, cls)
        for address, pattern, mask in guards[name]:
            code = image[address - base:address - base + len(mask)]
            assert all(m != "x" or code[i] == pattern[i] for i, m in enumerate(mask)), (name, hex(address))
        print(f"{name}: {len(checks)} instruction checks, {len(slots)} vtable slots, the RTTI names "
              f"and {len(guards[name])} install guards passed")
    finally:
        pe.close()
