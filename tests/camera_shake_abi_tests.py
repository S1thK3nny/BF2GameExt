"""Read-only audit of the camera shake against all three game PEs.

Usage: python tests/camera_shake_abi_tests.py PATH_TO_GAMEDATA
Requires pefile and capstone. Reads the offsets, addresses and install guards
from the production sources and checks them, and the instructions they were
read from, against each executable. Also checks the two ClassParent Derive
sites entity/odf_gameext_props.cpp patches for the ODF properties. Never loads
or executes the game. Not an in-game behaviour test.
"""
import codecs
import re
import sys
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "PatcherDLL" / "src"
module = (SRC / "render" / "camera_shake.cpp").read_text()
chase = (SRC / "core" / "layout" / "chase_camera.hpp").read_text()
flyer = (SRC / "core" / "layout" / "flyer.hpp").read_text()
red_camera = (SRC / "core" / "layout" / "red_camera.hpp").read_text()
odf = (SRC / "entity" / "odf_gameext_props.cpp").read_text()
addresses = (SRC / "core" / "game_addrs.hpp").read_text()


def const(text, name):
    match = re.search(r"\b" + name + r"\s*=\s*(0x[0-9a-fA-F]+|\d+)", text)
    assert match, name
    return int(match.group(1), 0)


expected = {
    (chase, "kOwner"): 0x0C, (chase, "kMatrix"): 0x10, (chase, "kPreShakeMatrix"): 0x50,
    (chase, "kShake"): 0x94, (chase, "kShakeSuppressUntil"): 0x98,
    (chase, "kShakeCount"): 0x9C, (chase, "kShakeAmount"): 0xA0, (chase, "kShakeSlots"): 4,
    (chase, "kTrackableTracker"): 0x1C,
    (red_camera, "kZoom"): 0x140, (red_camera, "kMatrixInverse"): 0x70,
    (flyer, "kStateLanded"): 0, (flyer, "kStateFlying"): 2, (flyer, "kStateLanding"): 3,
    (flyer, "kFlagBoost"): 0x04,
    (module, "kVt_GetGameObject"): 0x1C, (module, "kVt_GetControllable"): 0x28,
    (module, "kCtrl_Trackable"): 0x18, (module, "kVt_IsRtti"): 0x00,
    (module, "kVt_GetEntityClass"): 0x28,
    # A GameObject's Damageable at +0x140 (vptr), mCurHealth and mMaxHealth after it:
    # the offsets controller_rumble.cpp (Damageable + 4 / + 8) and aim_assist.cpp read.
    (module, "kObj_Health"): 0x144, (module, "kObj_MaxHealth"): 0x148,
    (module, "kSoldierSprint"): 3, (module, "kSoldierRoll"): 5,
}
rumble = (SRC / "controller" / "controller_rumble.cpp").read_text()
assert const(rumble, "kDamageable_offset") + const(rumble, "kDmg_mCurHealth") == 0x144
assert const(rumble, "kDmg_mMaxHealth") + const(rumble, "kDamageable_offset") == 0x148

# The soldier states the landing and roll triggers read, as SoldierState in the PDB.
assert "s == 4 || s == 6 || s == 7 || s == 8" in module, "airborne states changed"
assert "s == 0 || s == 1 || s == 2 || s == 3 || s == 5 || s == 19" in module, "grounded states changed"
for (text, name), value in expected.items():
    assert const(text, name) == value, name

# velocity, state, flags, trick, cls
flyer_offsets = {
    "modtools": (0x580, 0x5A4, 0x5F4, 0x610, 0x66C),
    "release": (0x540, 0x564, 0x5B4, 0x5D0, 0x62C),
}
for name, want in (("kModtools", flyer_offsets["modtools"]), ("kRelease", flyer_offsets["release"])):
    got = re.search(r"Offsets " + name + r"\s*=\s*\{([^}]*)\}", flyer).group(1)
    assert tuple(int(v, 0) for v in got.split(",")) == want, name


def guards():
    """Every guard(...) in the module: (address name, [(bytes, mask)] per build kind)."""
    out = {}
    for m in re.finditer(r'guard\(base, g_addr->(\w+), "[^"]*",\s*(.*?)\)(?: \|\||\))', module, re.S):
        args = m.group(2)
        pair = re.match(r'modtools \? ((?:"[^"]*"\s*)+):\s*((?:"[^"]*"\s*)+),\s*'
                        r'modtools \? "([^"]*)" : "([^"]*)"', args)
        if pair:
            mt = "".join(re.findall(r'"([^"]*)"', pair.group(1)))
            rt = "".join(re.findall(r'"([^"]*)"', pair.group(2)))
            out[m.group(1)] = {"modtools": (mt, pair.group(3)), "release": (rt, pair.group(4))}
            continue
        single = re.match(r'((?:"[^"]*"\s*)+),\s*"([^"]*)"', args)
        assert single, "Update the audit if a guard changes shape: " + m.group(1)
        both = ("".join(re.findall(r'"([^"]*)"', single.group(1))), single.group(2))
        out[m.group(1)] = {"modtools": both, "release": both}
    return out


GUARDS = guards()
assert set(GUARDS) == {"chase_camera_setup_camera", "red_camera_set_matrix",
                       "tracker_is_first_person_view", "flyer_do_trick", "weapon_signal_fire",
                       "reticle_display_update"}, GUARDS

# The Derive sites and the bytes the patcher expects, by reader and build kind.
derive_bytes = {
    ("modtools", "entity"): "8B 03 52 8B CB FF 50 04",
    ("modtools", "weapon"): "8B 16 57 8B CE FF 52 04",
    ("release", "entity"): "8B 07 8B CF 52 FF 50 04",
    ("release", "weapon"): "8B 07 8B CF 56 FF 50 04",
}
for (kind, reader), text in derive_bytes.items():
    literal = ", ".join("0x" + b for b in text.split())
    assert literal in odf, (kind, reader, "derive bytes changed in odf_gameext_props.cpp")

reader_starts = {
    "modtools": {"entity": 0x004D0830, "weapon": 0x0061E3F0},
    "steam": {"entity": 0x00491CC0, "weapon": 0x0067A240},
    "gog": {"entity": 0x00491CC0, "weapon": 0x0067B2E0},
}


def table(build):
    body = re.search(r"namespace " + build + r"\s*\{(.*?)\n\s*\}\s*//\s*namespace " + build,
                     addresses, re.S).group(1)
    return {name: int(value, 16) for name, value in
            re.findall(r"constexpr uintptr_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;", body)}


# (address, [(mnemonic, operands), ...]); {name} is a game_addrs value of that build.
retail_flyer = {
    0x004B18FB: [("mov", "ecx, dword ptr [edi + 0x62c]")],                 # DoTrick: mClass
    0x004B1940: [("mov", "dword ptr [edi + 0x5d0], 0")],                    # mTrick = 0
    0x004B198F: [("test", "byte ptr [edi + 0x5b4], 3")],                    # flag byte
    0x004B1B21: [("mov", "dword ptr [edi + 0x5d0], 0xbf800000")],           # refused: -1
    0x004B1B64: [("ret", "4")],
    0x004ABC70: [("mov", "dl, byte ptr [ecx + 0x5b4]")],                    # RecalculateSpeed
    0x004ABC7F: [("cmp", "dword ptr [ecx + 0x564], 2")],                    # mState == FLYING
    0x004ABD07: [("movss", "xmm1, dword ptr [ecx + 0x544]")],               # mVelocity.y
    0x004ABD0F: [("mulss", "xmm0, dword ptr [ecx + 0x540]")],               # mVelocity.x
    0x00402AD0: [("push", "{flyer_rtti_name}")],                            # rttiHashEntityFlyer
}
sites = {
    "modtools": {
        0x004A244F: [("mov", "eax, dword ptr [ebx + 0xc]")],                # mOwner
        0x004A2461: [("call", "{red_camera_set_matrix}")],
        0x004A25C9: [("mov", "eax, dword ptr [ebx + 0xc]"), ("mov", "ecx, dword ptr [eax + 0x1c]"),
                     ("test", "ecx, ecx")],                                 # owner->mTracker
        0x004A25D3: [("call", "0x40ff6f")],
        0x0040FF6F: [("jmp", "{tracker_is_first_person_view}")],
        0x004A287F: [("fld", "dword ptr [ebx + 0x94]")],                    # mShake
        0x004A289B: [("fcomp", "dword ptr [ebx + 0x98]")],                  # mShakeSuppressUntil
        0x004A2967: [("lea", "edi, [ebx + 0x50]")],                         # mPreShakeMatrix
        0x004A2A0B: [("mov", "esi, dword ptr [ebp + 8]"), ("push", "edi"), ("mov", "ecx, esi"),
                     ("call", "{red_camera_set_matrix}")],
        0x004A2A7F: [("ret", "8")],
        0x0049FE18: [("mov", "al, byte ptr [edi + 0x14]"), ("pop", "edi"), ("pop", "esi"), ("ret", "")],
        0x004F3D19: [("mov", "ecx, dword ptr [esi + 0x66c]")],              # DoTrick: mClass
        0x004F3D68: [("mov", "dword ptr [esi + 0x610], edi")],              # mTrick = 0
        0x004F3DBF: [("test", "byte ptr [esi + 0x5f4], 3")],                # flag byte
        0x004F3F88: [("mov", "dword ptr [esi + 0x610], 0xbf800000")],       # refused: -1
        0x004F3FBF: [("ret", "4")],
        0x004F2F82: [("mov", "cl, byte ptr [edx + 0x5f4]")],                # RecalculateSpeed
        0x004F2F94: [("cmp", "dword ptr [edx + 0x5a4], 2")],                # mState == FLYING
        0x004F304A: [("fld", "dword ptr [edx + 0x588]")],                   # mVelocity.z
        0x004F306A: [("fmul", "dword ptr [edx + 0x580]")],                  # mVelocity.x
        0x00A168C0: [("push", "{flyer_rtti_name}")],                        # rttiHashEntityFlyer
        # ChaseCamera::Update: the shake queue
        0x004A2D7E: [("mov", "eax, dword ptr [ebp + 0x9c]")],               # mShakeCount
        0x004A2D9C: [("lea", "edx, [ebp + 0xa0]"), ("lea", "edi, [ebp + 0xb0]")],  # amounts, decays
        # RedCamera projection: tan = unzoomed tan / _fZoom
        0x007FEE57: [("fdiv", "dword ptr [ecx + 0x140]"), ("fld", "st(0)"),
                     ("fmul", "dword ptr [ecx + 0x138]"), ("fst", "dword ptr [ecx + 0x144]")],
        # ReticuleDisplay::Update projects the aim point through _MatrixInverse
        0x006834A0: [("mov", "ecx, ebp"), ("call", "0x413b24")],
        0x00413B24: [("jmp", "0x678520")],
        0x0067852B: [("lea", "eax, [esi + 0x70]")],
        0x00683584: [("ret", "4")],
    },
    "steam": {**retail_flyer, **{
        0x00453D14: [("cmp", "dword ptr [esi + 0xc], 0"), ("lea", "eax, [esi + 0x10]")],
        0x00453D21: [("call", "{red_camera_set_matrix}")],
        0x00453D3B: [("ret", "8")],
        0x00453EC4: [("mov", "ecx, dword ptr [eax + 0x1c]")],
        0x00453ECB: [("call", "{tracker_is_first_person_view}")],
        0x0045438E: [("movss", "xmm0, dword ptr [edi + 0x94]")],
        0x004543A7: [("comiss", "xmm0, dword ptr [edi + 0x98]")],
        0x00453855: [("cmp", "dword ptr [esi + 0x9c], edi")],               # mShakeCount
        0x00453861: [("lea", "ecx, [esi + 0xb0]"), ("lea", "ebx, [esi + 0xa0]")],  # decays, amounts
        0x006CBCEB: [("divss", "xmm1, dword ptr [edx + 0x140]")],           # _fZoom
        0x006CBD02: [("movss", "xmm0, dword ptr [edx + 0x138]")],           # unzoomed tan
        0x00630883: [("lea", "eax, [esi + 0x70]"), ("push", "eax")],        # reticule: _MatrixInverse
        0x006308A9: [("call", "0x6cbfe0")],                                  # to projection space
        0x0063098B: [("ret", "4")],
    }},
    "gog": {**retail_flyer, **{
        0x00453CF4: [("cmp", "dword ptr [esi + 0xc], 0"), ("lea", "eax, [esi + 0x10]")],
        0x00453D01: [("call", "{red_camera_set_matrix}")],
        0x00453D1B: [("ret", "8")],
        0x00453EA4: [("mov", "ecx, dword ptr [eax + 0x1c]")],
        0x00453EAB: [("call", "{tracker_is_first_person_view}")],
        0x0045436E: [("movss", "xmm0, dword ptr [edi + 0x94]")],
        0x00454387: [("comiss", "xmm0, dword ptr [edi + 0x98]")],
        0x00453835: [("cmp", "dword ptr [esi + 0x9c], edi")],
        0x00453841: [("lea", "ecx, [esi + 0xb0]"), ("lea", "ebx, [esi + 0xa0]")],
        0x006CCD8B: [("divss", "xmm1, dword ptr [edx + 0x140]")],
        0x006CCDA2: [("movss", "xmm0, dword ptr [edx + 0x138]")],
        0x00631923: [("lea", "eax, [esi + 0x70]"), ("push", "eax")],
        0x00631A2B: [("ret", "4")],
    }},
}

# ChaseCamera's vtable: Update (+0x04) sits in the slot before SetupCamera (+0x08),
# which ties the queue sites above to the class. modtools goes through thunks.
# build: (address of the SetupCamera slot, Update, SetupCamera thunk, Update thunk)
vtables = {
    "modtools": (0x00A36934, 0x004A2D70, 0x00405CA4, 0x00414367),
    "steam": (0x00797180, 0x00453820, None, None),
    "gog": (0x00798120, 0x00453800, None, None),
}

builds = [
    ("modtools", "BF2_modtools_NoDVD_NoConsole.exe"),
    ("steam", "BattlefrontII.exe"),
    ("gog", "BattlefrontII_GoG.exe"),
]
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)


def branch_targets(image, base, start, end):
    """Immediate targets of every jump and call decoded linearly from start to end."""
    targets = set()
    for ins in decoder.disasm(image[start - base:end - base], start):
        if ins.mnemonic.startswith("j") or ins.mnemonic == "call":
            if ins.op_str.startswith("0x"):
                targets.add(int(ins.op_str, 16))
    return targets


for build, filename in builds:
    addrs = table(build)
    kind = "modtools" if build == "modtools" else "release"
    pe = pefile.PE(str(Path(sys.argv[1]) / filename))
    try:
        image = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        assert base == 0x400000

        # The RTTI name the flyer hash is made from.
        rtti_push = [a for a in sites[build] if sites[build][a] == [("push", "{flyer_rtti_name}")]][0]
        name_va = int.from_bytes(image[rtti_push + 1 - base:rtti_push + 5 - base], "little")
        assert image[name_va - base:name_va - base + 12] == b"EntityFlyer\x00", (build, "RTTI name")
        after = list(decoder.disasm(image[rtti_push + 5 - base:rtti_push + 20 - base], rtti_push + 5, count=2))
        assert after[0].mnemonic == "mov" and after[0].op_str.startswith("ecx, ") and \
            after[1].mnemonic == "call", (build, "RTTI hash call")
        names = {name: hex(value) for name, value in addrs.items()}
        names["flyer_rtti_name"] = hex(name_va)

        for address_name, per_kind in GUARDS.items():
            raw, mask = per_kind[kind]
            want = codecs.decode(raw, "unicode_escape").encode("latin1")
            assert len(want) == len(mask), (build, address_name, "guard length")
            va = addrs[address_name]
            got = image[va - base:va - base + len(want)]
            assert all(m != "x" or g == w for g, w, m in zip(got, want, mask)), \
                (build, address_name, got.hex())

        for address, instructions in sites[build].items():
            want_ins = [(m, ops.format(**names)) for m, ops in instructions]
            data = image[address - base:address - base + 32]
            got_ins = [(i.mnemonic, i.op_str) for i in
                       decoder.disasm(data, address, count=len(want_ins))]
            assert got_ins == want_ins, (build, hex(address), got_ins, want_ins)

        def dword(va):
            return int.from_bytes(image[va - base:va - base + 4], "little")

        def jmp_target(va):
            ins = list(decoder.disasm(image[va - base:va - base + 5], va, count=1))[0]
            assert ins.mnemonic == "jmp", (build, hex(va), "not a thunk")
            return int(ins.op_str, 16)

        slot, update, setup_thunk, update_thunk = vtables[build]
        setup = addrs["chase_camera_setup_camera"]
        if setup_thunk is None:
            assert dword(slot) == setup and dword(slot - 4) == update, (build, "ChaseCamera vtable")
        else:
            assert dword(slot) == setup_thunk and jmp_target(setup_thunk) == setup, (build, "vtable")
            assert dword(slot - 4) == update_thunk and jmp_target(update_thunk) == update, (build, "vtable")
        queue_sites = [a for a in sites[build] if update <= a < update + 0x100]
        assert len(queue_sites) == 2, (build, "the queue sites must sit in ChaseCamera::Update")

        for reader in ("entity", "weapon"):
            va = addrs[reader + "_class_read_derive_site"]
            want = bytes.fromhex(derive_bytes[(kind, reader)])
            assert image[va - base:va - base + 8] == want, (build, reader, "derive bytes")
            call = list(decoder.disasm(image[va + 5 - base:va + 8 - base], va + 5, count=1))[0]
            assert call.mnemonic == "call" and call.op_str.endswith("+ 4]"), (build, reader, call.op_str)
            inside = {t for t in branch_targets(image, base, reader_starts[build][reader], va + 0x100)
                      if va < t < va + 8}
            assert not inside, (build, reader, "a branch lands inside the Derive site", inside)

        print(f"{build}: {len(GUARDS)} guards, {len(sites[build])} camera shake sites and "
              f"2 Derive sites passed")
    finally:
        pe.close()
