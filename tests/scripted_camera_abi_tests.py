"""Read-only audit of the scripted camera against all three game PEs.

Usage: python tests/scripted_camera_abi_tests.py PATH_TO_GAMEDATA
Requires pefile and capstone. Reads the addresses from game_addrs.hpp and the
install guards from the module, and checks them, and the instructions the
design rests on (docs/RE/ScriptedCamera.md), against each executable. Never
loads or executes the game. Not an in-game behaviour test.
"""
import codecs
import re
import struct
import sys
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parents[1]
module = (ROOT / "PatcherDLL/src/render/scripted_camera.cpp").read_text()
addresses = (ROOT / "PatcherDLL/src/core/game_addrs.hpp").read_text()
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
decoder.detail = False


def pbl_hash(s):
    """PblHash, as core/pbl_hash.hpp computes it: FNV-1a over the lowercased bytes."""
    h = 0x811C9DC5
    for c in s.encode():
        h ^= c | 0x20
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


LITERALS = r'((?:"[^"]*"\s*)+)'


def joined(literals):
    text = "".join(re.findall(r'"([^"]*)"', literals))
    return codecs.decode(text, "unicode_escape").encode("latin1")


def guards():
    """Each guard(base, g_addr->X, "...", bytes, mask) in the install: per kind."""
    out = {}
    for m in re.finditer(r'guard\(base, g_addr->(\w+), "[^"]*",\s*(.*?)\)( \|\||\))', module, re.S):
        pair = re.match(r'modtools \? ' + LITERALS + r':\s*' + LITERALS + r',\s*'
                        r'modtools \? ' + LITERALS + r':\s*' + LITERALS + r'$', m.group(2).strip(), re.S)
        if pair:
            out[m.group(1)] = {"modtools": (joined(pair.group(1)), joined(pair.group(3)).decode()),
                               "retail": (joined(pair.group(2)), joined(pair.group(4)).decode())}
            continue
        one = re.match(LITERALS + r',\s*' + LITERALS + r'$', m.group(2).strip(), re.S)
        assert one, "Update the audit if a guard changes shape: " + m.group(1)
        both = (joined(one.group(1)), joined(one.group(2)).decode())
        out[m.group(1)] = {"modtools": both, "retail": both}
    return out


GUARDS = guards()
assert set(GUARDS) == {"red_camera_set_fov", "trackable_check_first_person", "tracker_is_first_person_view"}, GUARDS
for g in GUARDS.values():
    for b, mask in g.values():
        assert len(b) == len(mask), (b, mask)

# The module's own constants, as the design reads them.
for name, value in [("kObjId", 0x04), ("kObjMatrix", 0xF0), ("kObjHandle", 0x204), ("kCtrlTrackable", 0x18),
                    ("kVtGetGameObject", 0x20), ("kIdMapSlots", 0x800), ("kShakeDecay", 0xB0),
                    ("kCamTanW", 0x138), ("kCamTanH", 0x13C), ("kListFirst", 0x4), ("kListNext", 0x4),
                    ("kListPath", 0xC), ("kPathHash", 0x14), ("kPathNodes", 0x18), ("kPathCount", 0x1C),
                    ("kNodeSize", 0x1C)]:
    m = re.search(r"constexpr (?:uint32_t|int)\s+" + name + r"\s*=\s*(0x[0-9A-Fa-f]+)", module)
    assert m and int(m.group(1), 16) == value, (name, m and m.group(1))
assert 'pbl_hash("EntityGeometry")' in module
# The hash as the game's RTTI uses it: camera shake checks the soldier's.
assert pbl_hash("EntitySoldier") == 0x5E8739F4

# Read off the disassembly before recording.
facts = {
    "modtools": dict(setup_calls_set_fov=0x004A2A74, apply_shake=0x004A0690, luaB_type=0x007BCEF0,
                     matrix_push=0x004701A0, rebuild=0x007FEE50),
    "steam": dict(setup_calls_set_fov=0x0045459D, apply_shake=0x0044F4C0, luaB_type=0x0069FF20,
                  matrix_push=0x00591200, rebuild=0x006CC090),
    "gog": dict(setup_calls_set_fov=0x0045457D, apply_shake=0x0044F4A0, luaB_type=0x006A0FB0,
                matrix_push=0x005921A0, rebuild=0x006CD130),
}

builds = [
    ("modtools", "BF2_modtools_NoDVD_NoConsole.exe"),
    ("steam", "BattlefrontII.exe"),
    ("gog", "BattlefrontII_GoG.exe"),
]
for build, filename in builds:
    body = re.search(r"namespace " + build + r"\s*\{(.*?)\n\s*\}\s*//\s*namespace " + build,
                     addresses, re.S).group(1)
    table = {name: int(value, 16) for name, value in
             re.findall(r"constexpr uintptr_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;", body)}
    f = facts[build]
    mt = build == "modtools"
    kind = "modtools" if mt else "retail"
    pe = pefile.PE(str(Path(sys.argv[1]) / filename), fast_load=True)
    try:
        image = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        assert base == 0x400000
        text = [s for s in pe.sections if s.Name.rstrip(b"\0") == b".text"][0]
        text_lo, text_hi = base + text.VirtualAddress, base + text.VirtualAddress + text.Misc_VirtualSize

        def at(va, n):
            return image[va - base:va - base + n]

        def code(va, n):
            return list(decoder.disasm(at(va, n), va))

        def thunk(va):
            while at(va, 1) == b"\xE9":
                va = va + 5 + struct.unpack("<i", at(va + 1, 4))[0]
            return va

        def call_target(va):
            assert at(va, 1) == b"\xE8", (build, hex(va), at(va, 5).hex())
            return thunk(va + 5 + struct.unpack("<i", at(va + 1, 4))[0])

        def function(va, limit=0x800, first_ret=False):
            """A function's instructions, to its last RET before padding (or its
            first RET, for a leaf the next function follows straight on)."""
            out = []
            for x in code(va, limit):
                out.append(x)
                if x.mnemonic == "ret" and (first_ret or at(x.address + x.size, 1) in (b"\xcc", b"\x90")):
                    break
            return out

        def lua_binding(name):
            """The function a {name, fn} luaL_reg entry names."""
            for m in re.finditer(b"\x00" + name.encode() + b"\x00", image):
                sva = base + m.start() + 1
                for r in re.finditer(re.escape(struct.pack("<I", sva)), image):
                    fn = struct.unpack_from("<I", image, r.start() + 4)[0]
                    if text_lo <= fn < text_hi:
                        return fn
            raise AssertionError((build, name, "no binding"))

        # The guards, as the module reads them.
        for name, by_kind in GUARDS.items():
            want, mask = by_kind[kind]
            got = at(table[name], len(mask))
            assert all(m != "x" or a == b for a, b, m in zip(got, want, mask)), (build, name, got.hex())

        # RedCamera::SetFOV: thiscall(fov, aspect), RET 8. It writes tan(fov / 2)
        # to +0x138, that times the aspect (the second argument) to +0x13C, and
        # rebuilds the projection. SetupCamera eases the FOV through it.
        set_fov = table["red_camera_set_fov"]
        ins = function(set_fov, 0x60, first_ret=True)
        text_ins = " | ".join("%s %s" % (x.mnemonic, x.op_str) for x in ins)
        assert "0x138]" in text_ins and "0x13c]" in text_ins, (build, text_ins)
        assert ("[esp + 8]" in text_ins) if mt else ("[ebp + 0xc]" in text_ins), (build, text_ins)
        assert ins[-1].mnemonic == "ret" and ins[-1].op_str == "8", (build, ins[-1].op_str)
        assert any(x.mnemonic == "call" and int(x.op_str, 16) == f["rebuild"] for x in ins
                   if x.op_str.startswith("0x")), build
        assert call_target(f["setup_calls_set_fov"]) == set_fov, build
        setup = table["chase_camera_setup_camera"]
        assert setup < f["setup_calls_set_fov"] < setup + 0x1000, build
        # SetMatrix, which the module calls with the view it draws: copies 16
        # dwords to +0x30 (movsd on modtools, SSE on retail), RET 4.
        sm = function(table["red_camera_set_matrix"], 0x80, first_ret=True)
        sm_text = " | ".join("%s %s" % (x.mnemonic, x.op_str) for x in sm)
        assert ("0x30]" in sm_text or "eax + 0x30" in sm_text), (build, sm_text[:200])
        assert sm[-1].op_str == "4", build

        # The projection rebuild reads the zoom the module writes before its
        # SetFOV call: +0x140.
        rb = function(f["rebuild"], 0x100, first_ret=True)
        assert any("0x140]" in x.op_str for x in rb), build

        # Trackable::CheckFirstPerson: thiscall(int playerId), every RET is
        # RET 4; it tests the controllable class's view lock (+0x14C) against 1,
        # then 2.
        cfp = table["trackable_check_first_person"]
        ins = function(cfp, 0x200)
        rets = [x for x in ins if x.mnemonic == "ret"]
        assert rets and all(x.op_str == "4" for x in rets), (build, [x.op_str for x in rets])
        lock = [x for x in ins if "0x14c]" in x.op_str]
        assert len(lock) >= 2 and lock[0].op_str.endswith(", 1") and lock[1].op_str.endswith(", 2"), \
            (build, [x.op_str for x in lock])

        # Tracker::IsFirstPersonView: the tracker's +0x10 trackable and its +0x14
        # flag, plain RET.
        ifp = function(table["tracker_is_first_person_view"], 0x80)
        assert any(x.op_str.endswith("[edi + 0x14]") for x in ifp), build
        assert all(x.op_str == "" for x in ifp if x.mnemonic == "ret"), build

        # The path list: GetPathPoint hands it to the search in ECX; the search
        # walks +0x4 and +0xC and compares each path's hash (+0x14).
        gpp = lua_binding("GetPathPoint")
        ins = code(gpp, 0x80)
        mov = [x for x in ins if x.mnemonic == "mov" and x.op_str == "ecx, 0x%x" % table["path_list"]]
        assert mov, (build, "GetPathPoint does not load the path list")
        after = ins[ins.index(mov[0]) + 1:ins.index(mov[0]) + 4]   # retail pushes the hash in between
        nxt = [x for x in after if x.mnemonic == "call"]
        assert nxt and nxt[0].op_str.startswith("0x"), (build, [(x.mnemonic, x.op_str) for x in after])
        find = thunk(int(nxt[0].op_str, 16))
        fins = code(find, 0x40)
        ftext = " | ".join("%s %s" % (x.mnemonic, x.op_str) for x in fins)
        assert "[ecx + 4]" in ftext and "+ 0xc]" in ftext, (build, ftext)
        hash_getter = [thunk(int(x.op_str, 16)) for x in fins if x.mnemonic == "call" and x.op_str.startswith("0x")]
        assert hash_getter and at(hash_getter[0], 4) == b"\x8b\x41\x14\xc3", (build, at(hash_getter[0], 4).hex())
        if not mt:
            # Retail moves the image: the list's address in that MOV is relocated,
            # and the module takes it through resolve().
            pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])
            relocs = {base + e.rva for blk in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []) for e in blk.entries
                      if e.type == pefile.RELOCATION_TYPE["IMAGE_REL_BASED_HIGHLOW"]}
            assert mov[0].address + 1 in relocs, (build, "the path list operand is relocated")
        # The node accessor: +0x18 then 0x1C a node (modtools keeps it out of line).
        if mt:
            assert at(0x008201D0, 3) == b"\x83\xc1\x18", build
            assert at(0x00820150, 12) == bytes.fromhex("8b4424048b116bc01c03c2c2"), build
            assert at(0x008201A0, 3) == b"\x83\xc1\x18" and at(0x00820140, 4) == b"\x8b\x41\x04\xc3", build
        else:
            shift = 0 if build == "steam" else 0x10A0
            # The node array: +0 the nodes and +4 how many, which its free zeroes.
            assert at(0x006DE030 + shift, 7) == bytes.fromhex("568bf18b0685c0"), build
            assert at(0x006DE042 + shift, 7) == bytes.fromhex("c7460400000000"), build
            # A node: the array (+0x18 of the path), then index * 7 * 4.
            node = 0x006DE0A0 + shift
            assert at(node, 8) == bytes.fromhex("558bec83c1185de9"), (build, at(node, 8).hex())
            elem = node + 12 + struct.unpack("<i", at(node + 8, 4))[0]
            assert at(elem, 24) == bytes.fromhex("558bec8b45088d14c5000000002bd08b018d04905dc20400"), \
                (build, at(elem, 24).hex())

        # lua_type: luaB_type's second call; lua_newuserdata: the matrix push
        # helper's call with 0x40.
        assert lua_binding("type") == f["luaB_type"], build
        calls = [thunk(int(x.op_str, 16)) for x in code(f["luaB_type"], 0x30)
                 if x.mnemonic == "call" and x.op_str.startswith("0x")]
        assert len(calls) >= 2 and calls[1] == table["lua_type"], (build, [hex(c) for c in calls])
        mp = code(f["matrix_push"], 0x20)
        pushes = [x for x in mp if x.mnemonic == "push" and x.op_str == "0x40"]
        call = [x for x in mp if x.mnemonic == "call"]
        assert pushes and call and thunk(int(call[0].op_str, 16)) == table["lua_newuserdata"], build
        assert any(x.mnemonic == "call" and x.op_str.startswith("0x") and thunk(int(x.op_str, 16)) == f["matrix_push"]
                   for x in code(gpp, 0x200)), (build, "GetPathPoint pushes its matrix through the helper")

        # The shake queue the module appends to, as CameraManager::ApplyShake
        # does: the chase camera at the manager's +0x28, count +0x9C (four
        # slots), amounts +0xA0, decays +0xB0.
        ash = " | ".join("%s %s" % (x.mnemonic, x.op_str) for x in function(f["apply_shake"], 0x40, first_ret=True))
        assert "+ 0x28]" in ash and "+ 0x9c]" in ash and "*4 + 0xa0]" in ash and "*4 + 0xb0]" in ash, (build, ash)
        assert table["camera_manager_apply_shake"] == f["apply_shake"], build

        # A character's unit: GetCharacterUnit reads +0x148 and calls the
        # Trackable's (+0x18) GetGameObject (+0x20).
        gcu = " | ".join("%s %s" % (x.mnemonic, x.op_str) for x in code(lua_binding("GetCharacterUnit"), 0xA0))
        assert "+ 0x148]" in gcu and ("+ 0x18]" in gcu or "+ 0x18" in gcu) and "call dword ptr [" in gcu \
            and "+ 0x20]" in gcu, (build, gcu)

        # The RTTI name the module asks an object for is one the game hashes at
        # start-up (its rttiHash initialisers hash the class names).
        assert image.find(b"\x00EntityGeometry\x00") >= 0, (build, "EntityGeometry RTTI name")

        print(f"{build}: 3 guards, SetFOV and SetupCamera's call, SetMatrix, the zoom read, CheckFirstPerson, "
              f"IsFirstPersonView, the path list and nodes, lua_type, lua_newuserdata, the shake queue, "
              f"GetCharacterUnit and the RTTI hash passed")
    finally:
        pe.close()
