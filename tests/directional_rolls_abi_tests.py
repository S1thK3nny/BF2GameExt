"""Read-only audit of the directional rolls and the soldier animation tables
against all three game PEs.

Usage: python tests/directional_rolls_abi_tests.py PATH_TO_GAMEDATA
Requires pefile and capstone. Reads the addresses (game_addrs.hpp), the guards
the installs check (entity/directional_rolls.cpp, entity/soldier_anim_tables.cpp)
and the offsets (game/Battlefront2/Source/SoldierAnimator.h), and checks them
against each executable: SoldierAnimator::SetupPose's contract and what its
callers keep across it, that its DIVE case aims the body from mMovement and the
body's rows, the action table getters, the bank, weapon and map tables, the
class instance cell, FindAnimation's contract, the frame time SetAction reads,
and that SetAction turns a roll into DIVE. Also checks that ComboAnimIncrease
(entity/combo_anim_limit.cpp) moves the same map and bank tables and keeps the
action slots where soldier_anim_tables reads them. Never loads or executes the
game. Not an in-game behaviour test.
"""
import codecs
import re
import struct
import sys
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "PatcherDLL" / "src"
module = (SRC / "entity" / "directional_rolls.cpp").read_text()
core = (SRC / "entity" / "directional_rolls_core.hpp").read_text()
tables_src = (SRC / "entity" / "soldier_anim_tables.cpp").read_text()
combo = (SRC / "entity" / "combo_anim_limit.cpp").read_text()
combo_layout = (SRC / "entity" / "combo_anim_layout.hpp").read_text()
animator = (SRC / "game" / "Battlefront2" / "Source" / "SoldierAnimator.h").read_text()
addresses = (SRC / "core" / "game_addrs.hpp").read_text()


def relocations(pe):
    """The addresses the loader moves when it loads the exe elsewhere (HIGHLOW entries)."""
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])
    base = pe.OPTIONAL_HEADER.ImageBase
    return {base + e.rva for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", [])
            for e in block.entries if e.type == pefile.RELOCATION_TYPE["IMAGE_REL_BASED_HIGHLOW"]}


def guards():
    out = {}
    for m in re.finditer(r'guard\(base, a->(\w+), "[^"]*",\s*(.*?)\)( \|\||\))', module + tables_src, re.S):
        pair = re.match(r'modtools \? ((?:"[^"]*"\s*)+):\s*((?:"[^"]*"\s*)+),\s*'
                        r'modtools \? "([^"]*)" : "([^"]*)"', m.group(2))
        assert pair, "Update the audit if the guard changes shape: " + m.group(1)
        mt = "".join(re.findall(r'"([^"]*)"', pair.group(1)))
        rt = "".join(re.findall(r'"([^"]*)"', pair.group(2)))
        out[m.group(1)] = {"modtools": (mt, pair.group(3)), "release": (rt, pair.group(4))}
    return out


GUARDS = guards()
assert set(GUARDS) == {"soldier_animator_setup_pose", "soldier_anim_get_upper_action",
                       "soldier_anim_get_lower_action"}, GUARDS


def constant(name, text):
    m = re.search(r"constexpr \w+\s+" + name + r"\s*=\s*(0x[0-9A-Fa-f]+|\d+)", text)
    assert m, name
    return int(m.group(1), 0)


def fields(text, namespace):
    body = re.search(r"namespace " + re.escape(namespace) + r"\s*\{(.*?)\n\}\s*//\s*namespace "
                     + re.escape(namespace) + r"\b", text, re.S).group(1)
    return {m.group(1): int(m.group(2), 0) for m in
            re.finditer(r"Field<[^;]*?>\s+(\w+)\{(0x[0-9A-Fa-f]+|\d+)\}", body)}


# The roll's fields; tests/directional_jets_abi_tests.py checks the rest of the
# header and that the two audits cover all of it.
F = fields(animator, "layout::SoldierAnimator")
ROLL_FIELDS = {"mLegMatrix_right", "mLegMatrix_forward", "mOwner", "mSoldierAction",
               "mWeaponAnimationMap", "mMovement", "mAction", "mActionTime"}
assert ROLL_FIELDS <= set(F), F
F = {name: F[name] for name in ROLL_FIELDS}
DIVE = constant("kActionDive", animator)
ROLL = constant("kStateRoll", animator)
assert (DIVE, ROLL) == (24, 5)
BANK_STRIDE = constant("kBankStride", animator)
BANK_PARENT = constant("kBankParent", animator)
WEAPON_STRIDE = constant("kWeaponStride", animator)
WEAPON_PARENT = constant("kWeaponParent", animator)
assert (BANK_STRIDE, BANK_PARENT, WEAPON_STRIDE, WEAPON_PARENT) == (0x2C, 0x24, 0x30, 0x28)
assert "kMapStride      = 0x97 * 8;" in animator and "kActionTable    = 0x24;" in animator
assert constant("kMinSpeedSq", core) == 4, "SetAction's 2 m/s"
assert re.search(r"return s > 0\.0f \? Side::Left : Side::Right;", core), "the right row points left"
# The source's own claims this audit rests on.
assert "tables::find_named(map, dive_name(side), half, d.name[half], match)" in module
assert "find_animation(pbl_temp_hash(names[n]))" in tables_src
assert "s_findAnimation(cls, nullptr, hash, nullptr)" in tables_src
assert "kSoldierControllable = 0x240;" in module
assert ("side_dive_aim(moved, sa::mLegMatrix_right(self), sa::mLegMatrix_forward(self), dive->side, move);"
        in module)
# A side dive's root aims a quarter turn back from the move: the left dive's
# move turned by -90 degrees in the body's plane, the right's by +90.
assert "const float f2 = side == Side::Left ? s : -s;" in core
assert "const float s2 = side == Side::Left ? -f : f;" in core
# The stock getters are only checked, and the stock tables only read, while
# ComboAnimIncrease is not in place; with it, its own storage is read.
assert "if (!combo_anim_limit_active() &&" in tables_src
assert "combo_anim_limit_action_slot(map, action, half != 0)" in tables_src
assert "cls + sac::action_entry(map, action) + half * 4" in tables_src

# ComboAnimIncrease: each map's table starts with its actions, 38 of them in
# upper/lower pairs; the action slot reads them, and its getter shims answer
# an action (kinds 2 and 3) from the same pairs.
assert re.search(r"struct combo_anim_map \{\s*void\* action\[38\]\[2\];", combo_layout)
assert "if (index < 38)\n      result = map->action[index][lower];" in combo_layout
assert re.search(r"case 2:\s*case 3:\s*if \(args\[0\] >= 38\) return 0;\s*map = g_storage\.get\(owner, "
                 r"\(int\)args\[1\]\);\s*index = \(int\)args\[0\];", combo)
assert "return table ? &table->action[action][lower ? 1 : 0] : nullptr;" in combo
assert "if (!g_installed || !g_instance || action < 0 || action >= 38) return nullptr;" in combo
assert DIVE < 38


def combo_address(function):
    """Per build, the stock table a ComboAnimIncrease registry replaces."""
    m = re.search(r"uintptr_t " + function + r"\(GameBuild build\)\s*\{\s*return build == GameBuild::Modtools \? "
                  r"(0x[0-9A-Fa-f]+)\s*: build == GameBuild::Steam\s*\? (0x[0-9A-Fa-f]+)\s*: (0x[0-9A-Fa-f]+);",
                  combo)
    assert m, function
    return dict(zip(("modtools", "steam", "gog"), (int(v, 16) for v in m.groups())))


COMBO_MAPS = combo_address("registry_address")
COMBO_BANKS = combo_address("bank_registry_address")


def pbl_hash(s):
    h = 0x811C9DC5
    for ch in s.encode("latin1"):
        c = ch - 256 if ch >= 128 else ch
        h = ((h ^ ((c & 0xFFFFFFFF) | 0x20)) * 0x01000193) & 0xFFFFFFFF
    return h


assert pbl_hash("UseDirectionalRolls") == 0x3F77C468
assert "static_assert(kUseDirectionalRolls == 0x3F77C468u);" in module

# Per build: SetAction (and where it reads the frame time), SetupPose's callers,
# UpdateActionAnimation and SetupPose's DIVE call of it.
facts = {
    "modtools": dict(set_action=0x00575D50, dt_read=0x00575D9B, callers=(0x00536E56, 0x00536E84, 0x00674C38),
                     update_action=0x0057AFD0, dive_case=0x0057CC3F, dive_call=0x0057CC7A, end=0x0057D49B),
    "steam": dict(set_action=0x0063ED60, dt_read=0x0063ED66, callers=(0x0048E023, 0x004E357A, 0x004E35AF),
                  update_action=0x00640860, dive_case=0x0063FDDC, dive_call=0x0063FE79, end=0x00640792),
    "gog": dict(set_action=0x0063FE00, dt_read=0x0063FE06, callers=(0x0048E023, 0x004E357A, 0x004E35AF),
                update_action=0x00641900, dive_case=0x00640E7C, dive_call=0x00640F19, end=0x00641832),
}

builds = [
    ("modtools", "BF2_modtools_NoDVD_NoConsole.exe"),
    ("steam", "BattlefrontII.exe"),
    ("gog", "BattlefrontII_GoG.exe"),
]
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
decoder.detail = True
VOLATILE = {"eax", "ecx", "edx", "ax", "al", "ah", "cx", "cl", "ch", "dx", "dl", "dh"} | {
    "xmm%d" % i for i in range(8)}

for build, filename in builds:
    body = re.search(r"namespace " + build + r"\s*\{(.*?)\n\s*\}\s*//\s*namespace " + build,
                     addresses, re.S).group(1)
    table = {name: int(value, 16) for name, value in
             re.findall(r"constexpr uintptr_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;", body)}
    kind = "modtools" if build == "modtools" else "release"
    f = facts[build]
    pe = pefile.PE(str(Path(sys.argv[1]) / filename), fast_load=True)
    try:
        image = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        assert base == 0x400000
        relocs = relocations(pe)

        def at(va, n):
            return image[va - base:va - base + n]

        def dword(va):
            return struct.unpack("<I", at(va, 4))[0]

        def code(va, n):
            return list(decoder.disasm(at(va, n), va))

        def call_target(va):
            """The function a CALL at `va` reaches, through an incremental-link thunk."""
            x = code(va, 5)[0]
            assert x.mnemonic == "call", (build, hex(va), x.mnemonic)
            dest = int(x.op_str, 16)
            if at(dest, 1) == b"\xe9":
                dest = dest + 5 + struct.unpack("<i", at(dest + 1, 4))[0]
            return dest

        setup = table["soldier_animator_setup_pose"]
        cell = table["soldier_animator_class_instance"]
        upper, lower = table["soldier_anim_get_upper_action"], table["soldier_anim_get_lower_action"]
        banks, weapons, maps = table["soldier_anim_banks"], table["soldier_anim_weapons"], table["soldier_anim_maps"]
        find = table["anim_class_find_in_banks"]
        dt = table["game_client_delta_time"]
        # ComboAnimIncrease copies and replaces these same two tables.
        assert (COMBO_MAPS[build], COMBO_BANKS[build]) == (maps, banks), (build, "ComboAnimIncrease's tables")

        # The bytes the install compares, with no relocation inside them: they
        # are compared as built, and Steam and GOG always load elsewhere.
        for name, va in (("soldier_animator_setup_pose", setup), ("soldier_anim_get_upper_action", upper),
                         ("soldier_anim_get_lower_action", lower)):
            raw, mask = GUARDS[name][kind]
            want = codecs.decode(raw, "unicode_escape").encode("latin1")
            assert len(want) == len(mask), (build, name)
            got = at(va, len(want))
            assert all(m != "x" or g == w for g, w, m in zip(got, want, mask)), (build, name, got.hex())
            assert not any(va - 3 <= r < va + len(want) for r in relocs), (build, name, "relocated guard bytes")

        # The getters: thiscall(action, map), RET 8, reading
        # [class + (map * 0x97 + action) * 8 + 0x24] (upper) and + 0x28 (lower).
        for va, disp in ((upper, "0x24"), (lower, "0x28")):
            ins = [(x.mnemonic, x.op_str) for x in code(va, 0x20)]
            assert ("ret", "8") in ins, (build, hex(va))
            assert any(m == "imul" and o.endswith("0x97") for m, o in ins), (build, hex(va))
            assert ("mov", "eax, dword ptr [ecx + eax*8 + %s]" % disp) in ins, (build, hex(va), ins)

        # SetupPose: thiscall(RedPose*), every exit RET 4; its last one ends it.
        body_ins = [x for x in code(setup, f["end"] - setup) if x.address < f["end"]]
        assert body_ins[-1].mnemonic == "ret" and body_ins[-1].address + body_ins[-1].size == f["end"], build
        rets = {x.op_str for x in body_ins if x.mnemonic == "ret"}
        assert rets == {"4"}, (build, rets)
        # `this` stays in one register through the DIVE case.
        this = "esi" if build == "modtools" else "edi"
        disps = {}
        for x in body_ins:
            for op in x.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base and op.mem.index == 0:
                    if x.reg_name(op.mem.base) == this:
                        disps.setdefault(op.mem.disp, x.address)
        for name in ("mOwner", "mSoldierAction", "mWeaponAnimationMap", "mMovement", "mAction", "mActionTime"):
            assert F[name] in disps, (build, name, hex(F[name]))

        def this_disps(x):
            return {op.mem.disp for op in x.operands if op.type == capstone.x86.X86_OP_MEM and op.mem.base
                    and x.reg_name(op.mem.base) == this}

        # mOwner is the soldier's start: SetupPose reads Controllable::mPlayerId
        # (+0xD4, see ai_fairness in game_addrs.hpp) at the owner's
        # kSoldierControllable + 0xD4, for NetGame::GetJoystickIndex.
        player_id = constant("kSoldierControllable", module) + 0xD4
        owner_reads = []
        for k, x in enumerate(body_ins):
            if x.mnemonic == "mov" and F["mOwner"] in this_disps(x) and x.op_str.startswith("e"):
                reg = x.op_str.split(",")[0]
                for y in body_ins[k + 1:k + 4]:
                    if any(op.type == capstone.x86.X86_OP_MEM and op.mem.base and y.reg_name(op.mem.base) == reg
                           and op.mem.disp == player_id for op in y.operands):
                        owner_reads.append(y.address)
        assert owner_reads, (build, "owner + 0x240 + mPlayerId")

        # mActionTime is the float SetupPose adds the frame time to at its end:
        # a float load or add of it, then a float store back (retail puts a
        # few unrelated instructions between them).
        t = F["mActionTime"]
        assert any(x.mnemonic in ("fstp", "movss") and x.op_str.startswith("dword ptr [") and t in this_disps(x)
                   and any(y.mnemonic in ("fadd", "addss") for y in body_ins[max(0, k - 8):k])
                   and any(t in this_disps(y) for y in body_ins[max(0, k - 8):k])
                   for k, x in enumerate(body_ins)), (build, "mActionTime += dt")

        # The DIVE case: the legs aim at atan2(mMovement . right, mMovement .
        # forward), then UpdateActionAnimation(DIVE, dt, 1.0, that angle, ...).
        # The fix points mMovement along the forward row, making the angle 0.
        dive = [x for x in body_ins if f["dive_case"] <= x.address <= f["dive_call"]]
        assert dive[0].address == f["dive_case"] and dive[-1].address == f["dive_call"], build
        assert call_target(f["dive_call"]) == f["update_action"], build
        assert any(x.mnemonic == "push" and x.op_str == hex(DIVE) for x in dive[-4:]), (build, "push DIVE")
        read = {op.mem.disp for x in dive for op in x.operands
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base and x.reg_name(op.mem.base) == this}
        movement, fwd, right = F["mMovement"], F["mLegMatrix_forward"], F["mLegMatrix_right"]
        if build == "modtools":
            # x87: a dot helper takes &mMovement with `this` (the right row), then
            # with `this + 0x20` (the forward row); FPATAN of the two.
            assert {movement, fwd} <= read, (build, sorted(map(hex, read)))
            assert any(x.mnemonic == "push" and x.op_str == this for x in dive), (build, "right row")
            assert any(x.mnemonic == "fpatan" for x in dive), build
        else:
            want = {movement + i for i in (0, 4, 8)} | {fwd + i for i in (0, 4, 8)} | {right + i for i in (0, 4, 8)}
            assert want <= read, (build, sorted(map(hex, want - read)))

        # SetupPose's callers keep nothing in a volatile register across it, so
        # a hook in plain C++ is enough.
        for c in f["callers"]:
            assert call_target(c) == setup, (build, hex(c))
            written = set()
            for x in code(c + 5, 0x40)[:10]:
                regs_read, regs_written = x.regs_access()
                names_read = {x.reg_name(r) for r in regs_read}
                if x.mnemonic in ("xor", "sub", "xorps", "pxor") and len(x.operands) == 2 and \
                        x.op_str.split(", ")[0] == x.op_str.split(", ")[1]:
                    names_read = set()   # zeroing: a write
                assert not (names_read & VOLATILE) - written, (build, hex(c), x.mnemonic, x.op_str)
                written |= {x.reg_name(r) for r in regs_written}
                if x.mnemonic in ("call", "ret", "jmp"):
                    break

        # The class instance cell: what SetupPose loads into ECX for the
        # class's calls; on retail its operand moves with the exe.
        refs = [x for x in body_ins if x.mnemonic == "mov" and x.op_str == "ecx, dword ptr [%s]" % hex(cell)]
        assert refs, (build, "SoldierAnimatorClass::sInstance")
        if build != "modtools":
            assert all(x.address + 2 in relocs for x in refs), (build, "the cell moves")

        # FindAnimation: thiscall(hash, name), RET 8.
        ins = code(find, 0x80)
        assert ("ret", "8") in [(x.mnemonic, x.op_str) for x in ins], (build, "FindAnimation")

        # The frame time: SetAction's first read, a relocated operand.
        x = code(f["dt_read"], 10)[0]
        assert hex(dt) in x.op_str, (build, x.mnemonic, x.op_str)
        assert (x.mnemonic, x.op_str.split(",")[0]) in (("mov", "eax"), ("movss", "xmm3")), (build, x.op_str)
        assert f["dt_read"] + x.size - 4 in relocs or build == "modtools", (build, "dt relocated")
        # SetAction turns ROLL into DIVE: MOV [this + mAction], 0x18.
        sa_ins = code(f["set_action"], 0xB00)
        assert any(x.mnemonic == "mov" and x.op_str.endswith("+ %s], %s" % (hex(F["mAction"]), hex(DIVE)))
                   for x in sa_ins), (build, "ROLL -> DIVE")

        # The tables' stock contents: the human bank, its weapons and maps.
        name = lambda va: at(va, 32).split(b"\0")[0].decode("latin1")
        assert name(banks) == "human" and dword(banks + BANK_PARENT) == 0, build
        stock = [("rifle", 0), ("bazooka", 0), ("tool", 0), ("pistol", 2), ("melee", 2)]
        got = [(name(weapons + i * WEAPON_STRIDE), dword(weapons + i * WEAPON_STRIDE + WEAPON_PARENT))
               for i in range(5)]
        assert got == stock, (build, got)
        assert [struct.unpack("<II", at(maps + i * 8, 8)) for i in range(5)] == [(0, i) for i in range(5)], build
        # And the code that reads them, through relocated operands on retail.
        for va in (banks, weapons, maps):
            hits = [base + m.start() for m in re.finditer(re.escape(struct.pack("<I", va)), image)]
            assert hits, (build, hex(va))
            if build != "modtools":
                assert any(h in relocs for h in hits), (build, hex(va), "relocated")
        print("%-8s ok: SetupPose 0x%08X, getters 0x%08X/0x%08X, tables 0x%08X/0x%08X/0x%08X, dt 0x%08X"
              % (build, setup, upper, lower, banks, weapons, maps, dt))
    finally:
        pe.close()

print("directional rolls ABI: all builds ok")
