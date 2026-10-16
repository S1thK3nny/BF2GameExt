"""Read-only audit of everything the AI goals feature reads, patches or replaces (modtools only).

Usage: python tests/ai_goals_abi_tests.py PATH_TO_GAMEDATA
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
source = (SRC / "ai/ai_goals.cpp").read_text()
core = (SRC / "ai/ai_goals_core.hpp").read_text()
addrs = (SRC / "core/game_addrs.hpp").read_text()
modtools = re.search(r"namespace modtools\s*\{(.*?)\n\}\s*//\s*namespace modtools", addrs, re.S).group(1)


def addr(name):
    match = re.search(r"\b" + name + r"\s*=\s*(0x[0-9a-fA-F]+)\s*;", modtools)
    assert match, name
    return int(match.group(1), 16)


def constant(text, name):
    match = re.search(r"\b" + name + r"\s*=\s*(-?0x[0-9a-fA-F]+|-?\d+)", text)
    assert match, name
    return int(match.group(1), 0)


pe = pefile.PE(str(Path(sys.argv[1]) / "BF2_modtools_NoDVD_NoConsole.exe"))
image = pe.get_memory_mapped_image()
base = pe.OPTIONAL_HEADER.ImageBase
assert base == 0x400000
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)


def at(va, n):
    return image[va - base:va - base + n]


def code(va, count):
    return [(i.mnemonic, i.op_str) for i in decoder.disasm(at(va, 64), va, count=count)]


pool = addr("ai_goal_pool_setup")
create = addr("ai_goal_manager_create")
instance = addr("ai_goal_manager_instance")
add = addr("ai_goal_add")
delete = addr("ai_goal_delete")
clear = addr("ai_goal_clear_all")
assign = addr("ai_goal_assign_unit")
remove = addr("ai_goal_remove_unit")
listing = addr("ai_goal_print")
find = addr("ai_goal_find")
update = addr("ai_goal_update")
teams = addr("team_array_base")
operator_new = addr("engine_operator_new")

# Every count of 20 the limit replaces: the bytes in the source are the exe's, at
# the offsets the source gives, and each is the instruction it is meant to be.
counts = re.findall(r"\{a->(\w+)(?: \+ (0x[0-9A-F]+))?,\s*\"([^\"]+)\",\s*(\d+),\s*(\d+),", source)
assert len(counts) == 9, counts
names = {"ai_goal_pool_setup": pool, "ai_goal_add": add, "ai_goal_delete": delete, "ai_goal_clear_all": clear,
         "ai_goal_find": find, "ai_goal_update": update, "ai_goal_print": listing}
for name, offset, text, length, index in counts:
    va = names[name] + (int(offset, 16) if offset else 0)
    want = bytes(int(b, 16) for b in text.split(r"\x")[1:])
    assert len(want) == int(length), (name, offset)
    assert at(va, len(want)) == want, (name, hex(va))
    assert want[int(index)] == 0x14, (name, offset)
# What they are: the pool's count pushed before its size, the loops' bounds.
assert code(pool, 3) == [("push", "0x3c"), ("push", "0x14"), ("mov", "ecx, 0xb8fa38")]
assert code(add + 0x6C, 2) == [("cmp", "edi, 0x14"), ("jl", "0x5cd193")]
assert code(delete + 0x18, 2) == [("cmp", "esi, 0x14"), ("jl", "0x5cd4e0")]
assert code(clear + 0xA, 1) == [("mov", "edi, 0x14")]
assert code(find + 0x32, 2) == [("cmp", "esi, 0x14"), ("jl", "0x5cdd10")]
assert code(update + 0xC4, 1) == [("mov", "edi, 0x14")]
assert code(listing + 0xA0, 2) == [("cmp", "edx, 0x14"), ("jl", "0x5cdac0")]
assert code(listing + 0xB7, 2) == [("cmp", "edx, 0x14"), ("je", "0x5cdc19")]
assert code(listing + 0x126, 1) == [("mov", "ebx, 0x14")]
# The listing's pointer steps five slots a turn: 0x14 bytes, not a count (left alone).
assert code(listing + 0x9A, 2) == [("add", "edx, 5"), ("add", "eax, 0x14")]

# Create: the game's operator new, its constructor through a thunk, mInstance.
assert code(create, 1) == [("push", "0x6c")]
call_new = struct.unpack("<i", at(create + 3, 4))[0] + create + 7
assert call_new == operator_new
assert at(create + 7, 10) == bytes.fromhex("83c40485c0740d8bc8e8")
thunk = struct.unpack("<i", at(create + 0x11, 4))[0] + create + 0x15
assert at(thunk, 1) == b"\xE9"
ctor = struct.unpack("<i", at(thunk + 1, 4))[0] + thunk + 5
assert ctor == 0x005CD0E0
assert at(create + 0x15, 5) == b"\xA3" + struct.pack("<I", instance) and at(create + 0x1A, 1) == b"\xC3"
# The constructor clears mGoals' 20 slots (+0x1C on), which the stand-in clears again, all of them.
assert at(ctor + 0x1A, 12) == bytes.fromhex("33c08d7e1cb914000000f3ab")
assert "\\x33\\xC0\\x8D\\x7E\\x1C\\xB9\\x14\\x00\\x00\\x00\\xF3\\xAB" in source
# The manager is 0x6C bytes with mGoals last: the vptr, the Thread, the next update time, 20 slots.
assert 0x1C + 20 * 4 == 0x6C and constant(source, "kMgr_Goals") == 0x1C

# AssignUnit: 0x64 of stack (the 20 weights among it), the character's team,
# Team::sTeams, a team's member count; replaced, so only its shape is checked.
assert code(assign, 2) == [("sub", "esp, 0x64"), ("mov", "eax, dword ptr [esp + 0x68]")]
assert at(assign + 0x12, 6) == bytes.fromhex("8b8834010000")
assert at(assign + 0x85, 2) == b"\x8B\x15" and struct.unpack("<I", at(assign + 0x87, 4))[0] == teams
assert at(assign + 0x94, 3) == bytes.fromhex("8b5038")
# Its last act: the chosen goal (mGoals[i], +0x1C) given the character (its one
# argument, at +0x78 past the 0x64 of locals and four saved registers) through
# its AddUnit (vptr +0x10).
assert code(assign + 0x320, 5) == [("mov", "ecx, dword ptr [ecx + ebx*4 + 0x1c]"),
                                   ("mov", "eax, dword ptr [esp + 0x78]"), ("mov", "edx, dword ptr [ecx]"),
                                   ("push", "eax"), ("call", "dword ptr [edx + 0x10]")]
# The goal's fields, as AssignUnit reads them.
for name, value in (("kGoal_Members", 0x14), ("kGoal_Weight", 0x18), ("kGoal_Enabled", 0x1C),
                    ("kGoal_Team", 0x1D), ("kVt_AddUnit", 0x10), ("kChr_GoalList", 0x20),
                    ("kChr_PlayerId", 0x130), ("kChr_Team", 0x134), ("kTeam_Members", 0x38)):
    assert constant(source, name) == value, name
assert at(assign + 0x2C, 4) == bytes.fromhex("0fb6681d")   # movzx ebp, byte [eax + 0x1d]: the goal's team
assert at(assign + 0x34, 4) == bytes.fromhex("80781c00")   # cmp byte [eax + 0x1c], 0: enabled
assert at(assign + 0x3A, 3) == bytes.fromhex("d94018")     # fld [eax + 0x18]: its weight
# The starting "least full" is the core's.
assert at(0x00A4A3C0, 4) == struct.pack("<f", 1e11)
assert "kNoRatio = 1e11f" in core
# RemoveUnit: the character on the stack, its goal list (+0x20), the list's count (+0x10), RET 4.
assert code(remove, 2) == [("mov", "eax, dword ptr [esp + 4]"), ("mov", "ecx, dword ptr [eax + 0x20]")]
assert at(remove + 0x28, 3) == b"\xC2\x04\x00"
# FindGoal, DeleteGoal and ClearAllGoals: thiscall, one argument, RET 4.
assert at(find + 0x3C, 3) == b"\xC2\x04\x00"
assert at(delete + 0x47, 3) == b"\xC2\x04\x00"
assert at(clear + 0x3F, 3) == b"\xC2\x04\x00"
# The pool's element is 0x3C, the largest goal (Defend, Destroy, Follow).
assert at(pool, 2) == b"\x6A\x3C"
# A goal's Assign (vptr +0x1C: the order to one member, thiscall(Character*), RET 4), which
# SetUnitGoal and ClearUnitGoal call so the unit takes its new order at once: Defend's
# (vtable 0x00A4A76C) and Destroy's (0x00A4A7E0) through their thunks; AddUnit at +0x10.
assert constant(source, "kVt_Assign") == 0x1C
for vtable, assign_at, ret_at in ((0x00A4A76C, 0x005CF5C0, 0x005CF64D), (0x00A4A7E0, 0x005CF7A0, 0x005CF82D)):
    thunk = struct.unpack("<I", at(vtable + 0x1C, 4))[0]
    assert code(thunk, 1) == [("jmp", hex(assign_at))], hex(vtable)
    assert code(assign_at, 1) == [("mov", "eax, dword ptr [esp + 4]")] and at(ret_at, 3) == b"\xC2\x04\x00"
    add = struct.unpack("<I", at(vtable + 0x10, 4))[0]
    assert code(add, 1) == [("jmp", "0x5cce70")], hex(vtable)
assert "if (uint8_t* const placed = assign(mgr, chr)) command(placed, chr);" in source
print("modtools: 9 counts, Create's shape, AssignUnit's reads, the RETs, the pool and the goals' Assign passed")
