"""Read-only audit of the directional jets against all three game PEs.

Usage: python tests/directional_jets_abi_tests.py PATH_TO_GAMEDATA
Requires pefile and capstone. Reads the addresses (game_addrs.hpp), the guards
the install checks (entity/directional_jets.cpp) and the offsets
(game/Battlefront2/Source/SoldierAnimator.h, Zephyr.h, EntitySoldierClass.h),
and checks them against each executable: SetupPose's one call of
ApplyProceduralAnimationAndBuildWorldMatrices and how each build passes it the
frame time, the contracts of ZephyrPoseDyn<32>::SetAnimation and
ZephyrPoseStatic<32>::Blend and the engine's own calls of them for the lower
body, every offset the blend reads or writes, the soldier class's jet hover
speeds where EntitySoldier::Update reads them, and that SetAction plays JET for
both jet states. Never loads or executes the game. Not an in-game behaviour
test.
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
module = (SRC / "entity" / "directional_jets.cpp").read_text()
core = (SRC / "entity" / "directional_jets_core.hpp").read_text()
animator = (SRC / "game" / "Battlefront2" / "Source" / "SoldierAnimator.h").read_text()
zephyr = (SRC / "game" / "Battlefront2" / "Source" / "Zephyr.h").read_text()
soldier_class = (SRC / "game" / "Battlefront2" / "Source" / "EntitySoldierClass.h").read_text()
layout = (SRC / "core" / "entity_layout.hpp").read_text()
addresses = (SRC / "core" / "game_addrs.hpp").read_text()


def relocations(pe):
    """The addresses the loader moves when it loads the exe elsewhere (HIGHLOW entries)."""
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])
    base = pe.OPTIONAL_HEADER.ImageBase
    return {base + e.rva for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", [])
            for e in block.entries if e.type == pefile.RELOCATION_TYPE["IMAGE_REL_BASED_HIGHLOW"]}


def guards():
    out = {}
    for m in re.finditer(r'guard\(base, a->(\w+),\s*"[^"]*",\s*(.*?)\)( \|\||\))', module, re.S):
        pair = re.match(r'modtools \? ((?:"[^"]*"\s*)+):\s*((?:"[^"]*"\s*)+),\s*'
                        r'modtools \? "([^"]*)" : "([^"]*)"', m.group(2))
        assert pair, "Update the audit if the guard changes shape: " + m.group(1)
        mt = "".join(re.findall(r'"([^"]*)"', pair.group(1)))
        rt = "".join(re.findall(r'"([^"]*)"', pair.group(2)))
        out[m.group(1)] = {"modtools": (mt, pair.group(3)), "release": (rt, pair.group(4))}
    return out


GUARDS = guards()
assert set(GUARDS) == {"soldier_animator_apply_procedural", "zephyr_pose_dyn_set_anim",
                       "zephyr_pose_static_blend_masked"}, GUARDS


def constant(name, text):
    m = re.search(r"constexpr \w+\s+" + name + r"\s*=\s*(0x[0-9A-Fa-f]+|\d+)", text)
    assert m, name
    return int(m.group(1), 0)


def fields(text, namespace):
    """{name: (debug, release)} for each Field in the namespace."""
    body = re.search(r"namespace " + re.escape(namespace) + r"\s*\{(.*?)\n\}\s*//\s*namespace "
                     + re.escape(namespace) + r"\b", text, re.S).group(1)
    out = {}
    for m in re.finditer(r"Field<[^;]*?>\s+(\w+)\{(0x[0-9A-Fa-f]+|\d+)(?:,\s*(0x[0-9A-Fa-f]+|\d+))?\}", body):
        dbg = int(m.group(2), 0)
        out[m.group(1)] = (dbg, int(m.group(3), 0) if m.group(3) else dbg)
    return out


# SoldierAnimator: the directional rolls audit checks the roll's fields; this
# one checks the rest, so between them every field in the header is read off
# all three builds.
SA = fields(animator, "layout::SoldierAnimator")
ROLL_FIELDS = {"mLegMatrix_right", "mLegMatrix_forward", "mOwner", "mSoldierAction", "mWeaponAnimationMap",
               "mMovement", "mAction", "mActionTime"}
JET_FIELDS = {"mLowerBodyAnimMask", "mZephyrSkeleton", "mZephyrPoseStatic", "mZephyrPoseDynLower", "m_pAnimLower"}
assert set(SA) == ROLL_FIELDS | JET_FIELDS, sorted(set(SA) ^ (ROLL_FIELDS | JET_FIELDS))
assert all(d == r for d, r in SA.values()), "SoldierAnimator does not move between layouts"
F = {k: v[0] for k, v in SA.items()}
JET = constant("kActionJet", animator)
assert JET == 25
assert JET < 38, "ComboAnimIncrease keeps actions 0..37"

ZD = fields(zephyr, "layout::ZephyrPoseDyn")
assert set(ZD) == {"m_pkAnim", "m_pSkel", "m_fCurT", "m_bLoop", "m_bInterpolate"}, ZD
ZD = {k: v[0] for k, v in ZD.items()}
ZD_SIZE = constant("kSize", zephyr)
ZA = {k: v[0] for k, v in fields(zephyr, "layout::ZephyrAnim").items()}
assert set(ZA) == {"m_u16NumFrames", "m_u16NumJoints"}, ZA
assert constant("kMaxJoints", zephyr) == 32
ZS = {k: v[0] for k, v in fields(zephyr, "layout::ZephyrSkeleton").items()}
assert ZS == {"m_pShared": 0}, ZS
SC = fields(soldier_class, "layout::EntitySoldierClass")
assert set(SC) == {"mMaxSpeed", "mMaxStrafeSpeed", "mThrustFactorJet", "mStrafeFactorJet"}, SC

# The source's own claims this audit rests on.
assert "constexpr uint32_t kSoldierControllable = 0x240;" in module
assert "soldier + kSoldierControllable + g_soldier->classPtr" in module
assert "tables::action_animation(map, sa::kActionJet, kLower)" in module
assert "sa::m_pAnimLower(animator) == hover" in module
assert "tables::find_named(map, anim_name(d), kLower, set.name[d], match)" in module
assert "constexpr int kLower = 1;" in module
assert "top_speeds(sc::mMaxSpeed(cls), sc::mMaxStrafeSpeed(cls), sc::mThrustFactorJet(cls),\n" in module
assert "s_setAnimation(dyn, nullptr, anim, kAnimFps);" in module
assert "zpd::m_fCurT(dyn) = phase;" in module
assert ("s_blend(&sa::mZephyrPoseStatic(animator), nullptr, dyn, &sa::mLowerBodyAnimMask(animator), t);"
        in module)
assert "zpd::m_pSkel(dyn) = &sa::mZephyrSkeleton(animator);" in module
assert "alignas(16) uint8_t dyn[zpd::kSize];" in module and "std::memset(dyn, 0, sizeof dyn);" in module
assert "zpd::m_bLoop" not in module, "a player of ours never loops (Zephyr.h)"
assert "float phase = zpd::m_fCurT(&sa::mZephyrPoseDynLower(animator));" in module
assert "const float s = dot3(move, sa::mLegMatrix_right(animator)) / dt;" in module
assert "const float dt = *s_frameTime;" in module
# The stand-in keeps every general and XMM register and the stack, then jumps
# to the function the call was going to.
stand_in = re.search(r"__declspec\(naked\) void apply_procedural_stand_in\(\)\s*\{\s*__asm \{(.*?)\}\s*\}",
                     module, re.S).group(1)
lines = [ln.split("//")[0].strip() for ln in stand_in.splitlines() if ln.split("//")[0].strip()]
assert lines[0] == "pushad" and lines[-2] == "popad" and lines[-1] == "jmp    [s_applyProcedural]", lines
for i in range(8):
    assert "movups [esp + 0x%02x], xmm%d" % (i * 0x10, i) in lines, i
    assert "movups xmm%d, [esp + 0x%02x]" % (i, i * 0x10) in lines, i
assert lines.count("sub    esp, 0x80") == 1 and lines.count("add    esp, 0x80") == 1
assert "push   ecx" in lines and "call   blend_legs_guarded" in lines and "add    esp, 4" in lines
# Left is + along the body's right row, as for the directional rolls.
assert "l.s = over(s, top.side);" in core and "out.w[lean.s > 0.0f ? Left : Right]" in core
assert "constexpr float kFollowRate = 7.5f;" in core


def pbl_hash(s):
    h = 0x811C9DC5
    for ch in s.encode("latin1"):
        c = ch - 256 if ch >= 128 else ch
        h = ((h ^ ((c & 0xFFFFFFFF) | 0x20)) * 0x01000193) & 0xFFFFFFFF
    return h


assert pbl_hash("UseDirectionalJets") == 0xECB123E4
assert "static_assert(kUseDirectionalJets == 0xECB123E4u);" in module
# ControlSpeed's posture names: "jet" is index 6 (EntitySoldierClass::SetProperty).
assert pbl_hash("ControlSpeed") == 0x47BF39C1 and pbl_hash("jet") == 0xD7E330F4


def class_ptr(build):
    name = "kSoldierModtools" if build == "modtools" else "kSoldierRelease"
    body = re.search(name + r" = \{(.*?)\};", layout, re.S).group(1)
    return int(re.search(r"/\* classPtr \*/ (0x[0-9A-Fa-f]+)", body).group(1), 16)


# Per build: SetupPose and its end, SetAction, UpdateActionAnimation (its
# lower-body Blend call), UpdateLowerBodyAnimation (its SetAnimation call),
# ZephyrAnimInst<32>::SetAnim, ZephyrPoseDyn<32>::GetJointTransform, and
# EntitySoldier::Update's JET_HOVER branch (the state test, the class load,
# the four class reads and the MoveJetHover call).
facts = {
    "modtools": dict(setup=0x0057C490, setup_end=0x0057D49B, set_action=0x00575D50, update_action=0x0057AFD0,
                     lower_blend_call=0x0057B2D4, update_lower=0x00579060, set_anim_call=0x005794AF,
                     set_anim_inst=0x00862580, joint_transform=0x0082A5B0,
                     hover_state=0x00548AC1, hover_class=0x00548AD9, hover_reads=(0x00548AE1, 0x00548AE9,
                                                                                   0x00548AF7, 0x00548B05),
                     hover_call=0x00548B33),
    "steam": dict(setup=0x0063FAA0, setup_end=0x00640792, set_action=0x0063ED60, update_action=0x00640860,
                  lower_blend_call=0x00640B5F, update_lower=0x006420D0, set_anim_call=0x0064227E,
                  set_anim_inst=0x0072F3F0, joint_transform=0x0072CDC0,
                  hover_state=0x004EBA64, hover_class=0x004EBA6E, hover_reads=(0x004EBA85, 0x004EBA8F,
                                                                                0x004EBABE, 0x004EBAB6),
                  hover_call=0x004EBAE0),
    "gog": dict(setup=0x00640B40, setup_end=0x00641832, set_action=0x0063FE00, update_action=0x00641900,
                lower_blend_call=0x00641BFF, update_lower=0x00643170, set_anim_call=0x0064331E,
                set_anim_inst=0x007304C0, joint_transform=0x0072DE90,
                hover_state=0x004EBA64, hover_class=0x004EBA6E, hover_reads=(0x004EBA85, 0x004EBA8F,
                                                                              0x004EBABE, 0x004EBAB6),
                hover_call=0x004EBAE0),
}

builds = [
    ("modtools", "BF2_modtools_NoDVD_NoConsole.exe"),
    ("steam", "BattlefrontII.exe"),
    ("gog", "BattlefrontII_GoG.exe"),
]
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
decoder.detail = True

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
        text = [s for s in pe.sections if s.Name.rstrip(b"\0") == b".text"][0]
        text_lo, text_hi = base + text.VirtualAddress, base + text.VirtualAddress + text.Misc_VirtualSize

        def at(va, n):
            return image[va - base:va - base + n]

        def code(va, n):
            return list(decoder.disasm(at(va, n), va))

        def thunk(va):
            while at(va, 1) == b"\xe9":
                va = va + 5 + struct.unpack("<i", at(va + 1, 4))[0]
            return va

        def call_target(va):
            """The function a CALL at `va` reaches, through incremental-link thunks."""
            ins = code(va, 16)
            assert ins and ins[0].mnemonic == "call" and ins[0].op_str.startswith("0x"), (build, hex(va), "not a CALL")
            return thunk(int(ins[0].op_str, 16))

        def function(va, limit=0x1000):
            """A function's instructions, to its last RET before padding."""
            out = []
            for x in code(va, limit):
                out.append(x)
                if x.mnemonic == "ret" and at(x.address + x.size, 1) in (b"\xcc", b"\x90"):
                    break
            return out

        def before(start, va, n):
            """The n instructions that end at `va`, decoded from the start of
            the function holding it."""
            ins = [x for x in code(start, va - start + 16) if x.address < va]
            assert ins[-1].address + ins[-1].size == va, (build, hex(va), "not an instruction start")
            return ins[-n:]

        def mem(x):
            return [(x.reg_name(op.mem.base) if op.mem.base else None,
                     x.reg_name(op.mem.index) if op.mem.index else None, op.mem.disp)
                    for op in x.operands if op.type == capstone.x86.X86_OP_MEM]

        apply = table["soldier_animator_apply_procedural"]
        site = table["soldier_animator_apply_procedural_call"]
        set_anim = table["zephyr_pose_dyn_set_anim"]
        blend = table["zephyr_pose_static_blend_masked"]
        setup = table["soldier_animator_setup_pose"]
        assert setup == f["setup"], build

        # The bytes the install compares: as built, apart from the stack
        # cookie's address, which is masked and moves with the exe.
        for name, va in (("soldier_animator_apply_procedural", apply), ("zephyr_pose_dyn_set_anim", set_anim),
                         ("zephyr_pose_static_blend_masked", blend)):
            raw, mask = GUARDS[name][kind]
            want = codecs.decode(raw, "unicode_escape").encode("latin1")
            assert len(want) == len(mask), (build, name)
            got = at(va, len(want))
            assert all(m != "x" or g == w for g, w, m in zip(got, want, mask)), (build, name, got.hex())
            for k, m in enumerate(mask):
                moved = any(va + k - 3 <= r <= va + k for r in relocs)
                assert moved == (m == "?"), (build, name, k, "masked bytes are exactly the relocated ones")

        # SetupPose's one call of ApplyProceduralAnimationAndBuildWorldMatrices,
        # in its body, with ECX `this`; nothing else in the exe calls it.
        this = "esi" if build == "modtools" else "edi"
        assert setup < site < f["setup_end"], build
        assert call_target(site) == apply, (build, "the call")
        lead = before(setup, site, 2)
        assert (lead[-1].mnemonic, lead[-1].op_str) == ("mov", "ecx, " + this), (build, lead[-1].op_str)
        if build == "modtools":
            # thiscall(float dt), RET 4: the frame time pushed.
            assert lead[0].mnemonic == "push", (build, lead[0].mnemonic)
            rets = {x.op_str for x in function(apply, 0x800) if x.mnemonic == "ret"}
            assert rets == {"4"}, (build, rets)
        else:
            # ECX and XMM1, plain RET: XMM1 set just before, stored at entry.
            assert (lead[0].mnemonic, lead[0].op_str.split(",")[0]) == ("movaps", "xmm1"), (build, lead[0].op_str)
            rets = {x.op_str for x in function(apply, 0x900) if x.mnemonic == "ret"}
            assert rets == {""}, (build, rets)
        blob = image[text_lo - base:text_hi - base]
        callers = []
        for m in re.finditer(rb"\xe8", blob):
            va = text_lo + m.start()
            dest = va + 5 + struct.unpack("<i", blob[m.start() + 1:m.start() + 5])[0]
            if text_lo <= dest < text_hi and thunk(dest) == apply:
                callers.append(va)
        assert callers == [site], (build, [hex(c) for c in callers])

        # UpdateActionAnimation lays the lower body's player on the pose with
        # this Blend: ECX this + mZephyrPoseStatic, then the player
        # (this + mZephyrPoseDynLower), the mask (this + mLowerBodyAnimMask) and
        # the factor, pushed last to first.
        assert call_target(f["lower_blend_call"]) == blend, build
        lead = before(f["update_action"], f["lower_blend_call"], 5)
        reg = "esi"
        assert (lead[-1].mnemonic, lead[-1].op_str) == ("lea", "ecx, [%s + %s]" % (reg, hex(F["mZephyrPoseStatic"]))), \
            (build, lead[-1].op_str)
        assert (lead[-2].mnemonic, lead[-3].mnemonic) == ("push", "lea"), build
        assert lead[-3].op_str.endswith("[%s + %s]" % (reg, hex(F["mZephyrPoseDynLower"]))), (build, lead[-3].op_str)
        assert (lead[-4].mnemonic, lead[-5].mnemonic) == ("push", "lea"), build
        assert lead[-5].op_str.endswith("[%s + %s]" % (reg, hex(F["mLowerBodyAnimMask"]))), (build, lead[-5].op_str)
        # Blend: thiscall(dyn, mask, float), RET 0xC; it finds the animation's
        # joint for each skeleton joint in the player's m_kAnim, and walks the
        # pose's joints through the pose's m_pSkel -> m_pShared -> joint count.
        ins = function(blend, 0x200)
        assert {x.op_str for x in ins if x.mnemonic == "ret"} == {"0xc"}, build
        assert any(d == 0x940 and idx for x in ins for _, idx, d in mem(x)), (build, "m_piAnimJointIdx")
        assert any(x.mnemonic in ("cmp", "ucomiss") and ("0x49742400" in x.op_str or "dword ptr [" in x.op_str)
                   for x in ins[:60]), build
        first = [(x.mnemonic, x.op_str) for x in ins[:20]]
        if build == "modtools":
            assert ("mov", "eax, dword ptr [edi]") in first and ("mov", "ecx, dword ptr [eax]") in first and \
                ("mov", "eax, dword ptr [ecx + 4]") in first, first
        else:
            assert ("mov", "eax, dword ptr [ecx]") in first and ("mov", "eax, dword ptr [eax]") in first and \
                ("cmp", "dword ptr [eax + 4], edi") in first, first
            # Every argument from the stack: the player, the mask and the factor.
            assert ("mov", "edx, dword ptr [ebp + 8]") in first and \
                ("movss", "xmm0, dword ptr [ebp + 0x10]") in first, first
            assert any(x.op_str == "ecx, dword ptr [ebp + 0xc]" for x in ins[:40]), build

        # UpdateLowerBodyAnimation starts the lower body's player with
        # SetAnimation(anim, 30.0): ECX this + mZephyrPoseDynLower.
        assert call_target(f["set_anim_call"]) == set_anim, build
        lead = before(f["update_lower"], f["set_anim_call"], 6)
        ops = [(x.mnemonic, x.op_str) for x in lead]
        assert any(o[1].endswith("0x41f00000") for o in ops), (build, ops, "30.0")
        assert any(o[0] == "lea" and o[1].endswith("+ %s]" % hex(F["mZephyrPoseDynLower"])) for o in ops), (build, ops)
        # SetAnimation: thiscall(anim, fps), RET 8: it hands SetAnim the
        # player's m_pSkel, zeroes m_fCurT and stores the rate.
        ins = function(set_anim, 0x100)
        assert {x.op_str for x in ins if x.mnemonic == "ret"} == {"8"}, build
        k = next(i for i, x in enumerate(ins) if x.mnemonic == "call" and x.op_str.startswith("0x")
                 and thunk(int(x.op_str, 16)) == f["set_anim_inst"])
        # SetAnim(anim, skeleton): the skeleton is the first thing pushed, read
        # straight from the player's m_pSkel (modtools through ECX).
        skel = "dword ptr [esi + %s]" % hex(ZD["m_pSkel"])
        lead = [(x.mnemonic, x.op_str) for x in ins[max(0, k - 6):k]]
        assert ("push", skel) in lead or (("mov", "ecx, " + skel) in lead and ("push", "ecx") in lead), (build, lead)
        assert any(x.op_str.startswith("dword ptr [esi + %s], " % hex(ZD["m_fCurT"])) for x in ins), build
        assert any(x.op_str.startswith("dword ptr [esi + 0x998], ") for x in ins), (build, "m_fAnimSpeed")
        # SetAnim compares and keeps m_kAnim.m_pkAnim and reads the animation's
        # joint count.
        ins = function(f["set_anim_inst"], 0x200)
        assert any(d == ZD["m_pkAnim"] and x.mnemonic == "cmp" for x in ins for _, _, d in mem(x)), build
        assert any(d == ZA["m_u16NumJoints"] and x.mnemonic == "cmp" and "word ptr" in x.op_str
                   for x in ins for _, _, d in mem(x)), build
        # GetJointTransform: m_kAnim.m_pkAnim's frame count, m_bInterpolate and
        # m_bLoop.
        ins = function(f["joint_transform"], 0x300)
        k = next(i for i, x in enumerate(ins) if any(d == ZD["m_pkAnim"] for _, _, d in mem(x)))
        reg = ins[k].op_str.split(",")[0]
        assert any(x.mnemonic == "movzx" and "word ptr [%s + %d]" % (reg, ZA["m_u16NumFrames"]) in x.op_str
                   for x in ins[k:k + 6]), (build, "m_u16NumFrames")
        for name in ("m_bInterpolate", "m_bLoop"):
            assert any(d == ZD[name] and "byte ptr" in x.op_str for x in ins for _, _, d in mem(x)), (build, name)

        # The lower body's player inside the animator, at the same offsets:
        # UpdateActionAnimation reads its animation and time, and
        # UpdateLowerBodyAnimation its m_bLoop; the upper body's player sits
        # just before it, so a player is kSize long.
        ua = function(f["update_action"], 0x700)
        ul = function(f["update_lower"], 0x600)
        ua_disps = {d for x in ua for _, _, d in mem(x)}
        ul_disps = {d for x in ul for _, _, d in mem(x)}
        lower = F["mZephyrPoseDynLower"]
        assert {lower + ZD["m_pkAnim"], lower + ZD["m_fCurT"], F["m_pAnimLower"]} <= ua_disps, build
        assert {lower + ZD["m_bLoop"], F["m_pAnimLower"]} <= ul_disps, build
        assert lower - ZD_SIZE in ua_disps, (build, "the upper body's player, one kSize before")
        # SetupPose hands ConvertFromZephyrPose the skeleton right after the call.
        after = code(site + 5, 0x20)
        assert any(x.mnemonic == "lea" and x.op_str.endswith("[%s + %s]" % (this, hex(F["mZephyrSkeleton"])))
                   for x in after[:4]), (build, [(x.mnemonic, x.op_str) for x in after[:4]])

        # SetAction plays JET for JET_JUMP and JET_HOVER: MOV [this + mAction], 0x19.
        sa_ins = code(f["set_action"], 0xB00)
        assert any(x.mnemonic == "mov" and x.op_str.endswith("+ %s], %s" % (hex(F["mAction"]), hex(JET)))
                   for x in sa_ins), (build, "JET")

        # EntitySoldier::Update, JET_HOVER (state 7): the class from the
        # Controllable part's classPtr, then MaxSpeed (forward) or
        # MaxStrafeSpeed, thrust[jet], strafe[jet] x MaxStrafeSpeed for the
        # MoveJetHover call.
        x = code(f["hover_state"], 8)[0]
        assert x.mnemonic == "cmp" and x.op_str.endswith(", 7"), (build, x.op_str)
        x = code(f["hover_class"], 8)[0]
        assert x.mnemonic == "mov" and x.op_str.endswith("+ %s]" % hex(class_ptr(build))), (build, x.op_str)
        cls_reg = x.op_str.split(",")[0]
        col = 0 if build == "modtools" else 1
        expect = [SC["mMaxSpeed"][col], SC["mMaxStrafeSpeed"][col], SC["mThrustFactorJet"][col],
                  SC["mStrafeFactorJet"][col]]
        for va, disp in zip(f["hover_reads"], expect):
            x = code(va, 10)[0]
            assert mem(x) == [(cls_reg, None, disp)], (build, hex(va), x.op_str, hex(disp))
        assert code(f["hover_call"], 5)[0].mnemonic == "call", build
        assert f["hover_state"] < f["hover_class"] < min(f["hover_reads"]) < f["hover_call"], build

        print("%-8s ok: SetupPose's call 0x%08X -> 0x%08X, SetAnimation 0x%08X, Blend 0x%08X, "
              "class fields %s" % (build, site, apply, set_anim, blend, ", ".join(hex(d) for d in expect)))
    finally:
        pe.close()

print("directional jets ABI: all builds ok")
