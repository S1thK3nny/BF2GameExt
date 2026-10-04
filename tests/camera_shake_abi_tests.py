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
import struct
import sys
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "PatcherDLL" / "src"
module = (SRC / "render" / "camera_shake.cpp").read_text()
core = (SRC / "render" / "camera_shake_core.hpp").read_text()
GAME = SRC / "game" / "Battlefront2" / "Source"
chase = (GAME / "ChaseCamera.h").read_text()
flyer = (GAME / "EntityFlyer.h").read_text()
walker = (GAME / "EntityWalker.h").read_text()
hover = (GAME / "EntityHover.h").read_text()
collision = (GAME / "CollisionObject.h").read_text()
red_camera = (GAME / "RedCamera.h").read_text()
weapon = (GAME / "Weapon.h").read_text()
odf = (SRC / "entity" / "odf_gameext_props.cpp").read_text()
addresses = (SRC / "core" / "game_addrs.hpp").read_text()


def const(text, name):
    match = re.search(r"\b" + name + r"\s*=\s*(0x[0-9a-fA-F]+|\d+)", text)
    assert match, name
    return int(match.group(1), 0)


def namespace_body(text, namespace):
    return re.search(r"namespace " + re.escape(namespace) + r"\s*\{(.*?)\n\}\s*//\s*namespace "
                     + re.escape(namespace) + r"\b", text, re.S).group(1)


def ns_const(text, namespace, name):
    """A constant inside one namespace, for names more than one namespace uses."""
    return const(namespace_body(text, namespace), name)


def fields(text, namespace):
    """A namespace's Field<> members: name -> (modtools, release); one offset is both."""
    body = namespace_body(text, namespace)
    out = {}
    for m in re.finditer(r"Field<[^;]*?>\s+(\w+)\{(0x[0-9A-Fa-f]+|\d+)(?:,\s*(0x[0-9A-Fa-f]+|\d+))?\}", body):
        dbg = int(m.group(2), 0)
        out[m.group(1)] = (dbg, int(m.group(3), 0) if m.group(3) else dbg)
    return out


expected = {
    (chase, "kOwner"): 0x0C, (chase, "kMatrix"): 0x10, (chase, "kPreShakeMatrix"): 0x50,
    (chase, "kShake"): 0x94, (chase, "kShakeSuppressUntil"): 0x98,
    (chase, "kShakeCount"): 0x9C, (chase, "kShakeAmount"): 0xA0, (chase, "kShakeSlots"): 4,
    (chase, "kTrackableTracker"): 0x1C,
    (red_camera, "kZoom"): 0x140, (red_camera, "kMatrixInverse"): 0x70,
    (flyer, "kLanded"): 0, (flyer, "kTakeoff"): 1, (flyer, "kFlying"): 2, (flyer, "kLanding"): 3,
    (flyer, "kFlagRoll"): 0x01, (flyer, "kFlagFlip"): 0x02, (flyer, "kFlagBoost"): 0x04,
    # EntityWalker::sStateTable's states (Phantom 0x00A8F0B0) and its flag bits.
    (walker, "kStateTurnLeft"): 1, (walker, "kStateTurnRight"): 2, (walker, "kStateDying"): 3,
    (walker, "kStateDead"): 4, (walker, "kFlagJumping"): 0x80, (walker, "kBoosting"): 0x01,
    (walker, "kMaxFeet"): 6,
    (module, "kVt_GetGameObject"): 0x1C, (module, "kVt_GetControllable"): 0x28,
    (module, "kCtrl_Trackable"): 0x18, (module, "kVt_IsRtti"): 0x00,
    (module, "kVt_GetEntityClass"): 0x28, (module, "kVt_GetVelocity"): 0x44,
    # The hover's jump bit, and its collision callback's `this` (the CollisionObject part).
    (hover, "kFlagJumping"): 0x02, (hover, "kCollisionPart"): 0x0C,
    # A GameObject's Damageable at +0x140 (vptr), mCurHealth and mMaxHealth after it:
    # the offsets controller_rumble.cpp (Damageable + 4 / + 8) and aim_assist.cpp read.
    (module, "kObj_Health"): 0x144, (module, "kObj_MaxHealth"): 0x148,
    (module, "kSoldierSprint"): 3, (module, "kSoldierRoll"): 5,
    # GameObject::mTeam's bits, as spawn_vehicle_list.cpp reads them.
    (module, "kObj_Team"): 0x234,
    # Melee: the Weapon vtable slots and WeaponMelee's list of what a swing struck.
    (weapon, "kVt_Deflect"): 0x48, (weapon, "kVt_SignalFire"): 0x4C, (weapon, "kVt_IsMelee"): 0x54,
    (weapon, "kVt_UpdateFire"): 0xA4,
    (weapon, "kDamageDataModtools"): 0x1D8, (weapon, "kDamageDataRelease"): 0x1A8,
    (weapon, "kHitCount"): 0x08, (weapon, "kHitObjects"): 0x0C, (weapon, "kHitNext"): 0x2C,
    (weapon, "kHitMax"): 8,
}
spawn_list = (SRC / "render" / "spawn_vehicle_list.cpp").read_text()
assert const(spawn_list, "kGO_TeamBitfield") == 0x234, "the team bits moved"
rumble = (SRC / "controller" / "controller_rumble.cpp").read_text()
assert const(rumble, "kDamageable_offset") + const(rumble, "kDmg_mCurHealth") == 0x144
assert const(rumble, "kDmg_mMaxHealth") + const(rumble, "kDamageable_offset") == 0x148

# The soldier states the landing and roll triggers read, as SoldierState in the PDB.
assert "s == 4 || s == 6 || s == 7 || s == 8" in module, "airborne states changed"
assert "s == 0 || s == 1 || s == 2 || s == 3 || s == 5 || s == 19" in module, "grounded states changed"
for (text, name), value in expected.items():
    assert const(text, name) == value, name

# What a hover's collision callback reads of the other object and the contact,
# where BF2's own callback reads it (the sites below).
expected_collision = {
    ("layout::CollisionObject", "kTreeGrid"): 0x04, ("layout::CollisionObject", "kVt_GetGameObject"): 0x38,
    ("layout::CollisionObject", "kTypeSoft"): 2,
    ("layout::TreeGridObject", "kStackPtr"): 0x00, ("layout::TreeGridObject", "kStackIdx"): 0x04,
    ("layout::TreeGridObject", "kData"): 0x1C, ("layout::TreeGridStack", "kData"): 0x04,
    ("layout::CollisionResult", "kSeparationNormal"): 0x0C,
}
for (namespace, name), value in expected_collision.items():
    assert ns_const(collision, namespace, name) == value, (namespace, name)
assert 'kHoverRtti   = pbl_hash("EntityHover")' in module, "the hover RTTI name"

# The flyer, walker and hover fields camera shake reads, (modtools, release). The
# sites below are where each was read.
expected_fields = {
    ("layout::EntityFlyer", flyer): {
        "mMatrix_forward": (0x110, 0x110), "mControlMove": (0x2C0, 0x2C0), "mControlStrafe": (0x2C4, 0x2C4),
        "mVelocity": (0x580, 0x540), "mState": (0x5A4, 0x564), "mFlags": (0x5F4, 0x5B4),
        "mGetSpeedSpeed": (0x5F8, 0x5B8), "mInLandingRegionFactor": (0x5FC, 0x5BC), "mTrick": (0x610, 0x5D0),
        "mClass": (0x66C, 0x62C),
    },
    ("layout::EntityFlyerClass", flyer): {
        "mMinSpeed": (0x88C, 0x7C4), "mMidSpeed": (0x890, 0x7C8), "mMaxSpeed": (0x894, 0x7CC),
        "mBoostSpeed": (0x898, 0x7D0), "mPitchRate": (0x8A0, 0x7D8), "mTurnRate": (0x8A4, 0x7DC),
    },
    ("layout::EntityWalker", walker): {
        "mClass": (0x498, 0x460), "mVelocity": (0x4A0, 0x468), "mFlags": (0x2060, 0x2020),
        "mFootState": (0x20A4, 0x2064), "m_fGroundedTimer": (0x20B4, 0x2074), "mState": (0x20B8, 0x2078),
        "mBoost": (0x212C, 0x20EC),
    },
    ("layout::EntityWalkerClass", walker): {
        "mMaxSpeed": (0x768, 0x6A0), "mBoostSpeed": (0x76C, 0x6A4), "mNumFeet": (0xD22, 0xC5A),
    },
    ("layout::EntityHover", hover): {
        "mMatrix_right": (0xF0, 0xF0), "mMatrix_up": (0x100, 0x100), "mMatrix_forward": (0x110, 0x110),
        "mVelocity": (0x498, 0x460), "mGroundRatio": (0x4B0, 0x478), "mClass": (0x4C4, 0x48C),
        "mBoost": (0x1D40, 0x1D00), "mFlags": (0x1D41, 0x1D01),
    },
    ("layout::EntityHoverClass", hover): {
        "mForwardSpeed": (0x6C8, 0x600), "mBoostSpeed": (0xEC0, 0xDEC),
    },
}
for (namespace, text), want in expected_fields.items():
    got = fields(text, namespace)
    for name, offsets in want.items():
        assert got.get(name) == offsets, (namespace, name, got.get(name))


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
                       "reticle_display_update", "camera_manager_apply_shake",
                       "weapon_melee_update_fire", "weapon_melee_deflect", "hover_collision_callback"}, GUARDS

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
    0x004ABCA6: [("mov", "eax, dword ptr [ecx + 0x62c]"),                   # the class's BoostSpeed
                 ("movss", "xmm2, dword ptr [eax + 0x7d0]")],
    # mVelocity along the matrix's forward axis (+ 0x110) into mGetSpeedSpeed.
    0x004ABCFF: [("movss", "xmm0, dword ptr [ecx + 0x110]"), ("movss", "xmm1, dword ptr [ecx + 0x544]"),
                 ("mulss", "xmm0, dword ptr [ecx + 0x540]"), ("mulss", "xmm1, dword ptr [ecx + 0x114]")],
    0x004ABD1F: [("addss", "xmm1, xmm0"), ("movss", "xmm0, dword ptr [ecx + 0x548]"),
                 ("mulss", "xmm0, dword ptr [ecx + 0x118]"), ("addss", "xmm1, xmm0"),
                 ("movss", "dword ptr [ecx + 0x5b8], xmm1")],
    0x00402AD0: [("push", "{flyer_rtti_name}")],                            # rttiHashEntityFlyer
    # GetFlyerMaxSpeed, GetFlyerMidSpeed and GetFlyerMinSpeed (ECX the flyer, the
    # speed back in XMM0): mInLandingRegionFactor, then the class's MaxSpeed,
    # MidSpeed and MinSpeed, capped while it is non-zero (the caps are per build).
    0x004AC3A0: [("movss", "xmm0, dword ptr [ecx + 0x5bc]")],
    0x004AC3B3: [("mov", "eax, dword ptr [ecx + 0x62c]"), ("movss", "xmm0, dword ptr [eax + 0x7cc]")],
    0x004AC3F3: [("mov", "eax, dword ptr [ecx + 0x62c]"), ("movss", "xmm0, dword ptr [eax + 0x7c8]")],
    0x004AC433: [("mov", "eax, dword ptr [ecx + 0x62c]"), ("movss", "xmm0, dword ptr [eax + 0x7c4]")],
    # EntityFlyer::Update works from the flyer's Controllable part, the flyer +
    # 0x240 (EDI; the flyer in ESI): the throttle, mControlMove (+ 0x80, kept at
    # [ESP + 0x88]), and outside a landing region (+ 0x37C, the flyer's + 0x5BC)
    # the roll, mControlStrafe (+ 0x84).
    0x004AC460: [("push", "ebp"), ("mov", "ebp, esp"), ("and", "esp, 0xfffffff0"), ("sub", "esp, 0x2b8"),
                 ("push", "esi"), ("push", "edi"), ("mov", "edi, ecx")],
    0x004AC470: [("lea", "eax, [ebp + 8]"), ("push", "eax"), ("mov", "dword ptr [esp + 0x100], edi"),
                 ("lea", "esi, [edi - 0x240]")],
    0x004ACACB: [("mov", "ecx, esi"), ("call", "0x4abc70")],               # RecalculateSpeed(flyer)
    0x004ACD81: [("movss", "xmm0, dword ptr [edi + 0x37c]"), ("xorps", "xmm1, xmm1"),
                 ("movss", "xmm7, dword ptr [edi + 0x80]"), ("ucomiss", "xmm0, xmm1"),
                 ("movss", "dword ptr [esp + 0x88], xmm7")],
    0x004ACDA6: [("test", "byte ptr [edi + 0x54], 1"), ("je", "0x4acdb6"),
                 ("movss", "xmm2, dword ptr [edi + 0x88]"), ("jmp", "0x4acdc3"),
                 ("movss", "xmm2, dword ptr [edi + 0x84]")],
    # The class (+ 0x3EC, the flyer's + 0x62C): BoostSpeed and MaxSpeed, then
    # TurnRate and PitchRate.
    0x004AD113: [("mov", "edx, dword ptr [edi + 0x3ec]"), ("movss", "xmm0, dword ptr [edx + 0x7d0]"),
                 ("movss", "xmm1, dword ptr [edx + 0x7cc]"), ("comiss", "xmm1, xmm0")],
    0x004AD12C: [("movss", "dword ptr [esp + 0x20], xmm0"), ("movss", "dword ptr [esp + 0x90], xmm1"),
                 ("ja", "0x4ad146"), ("movss", "dword ptr [esp + 0x90], xmm0")],
    0x004AD146: [("movss", "xmm6, dword ptr [edx + 0x7dc]"), ("movss", "xmm7, dword ptr [edx + 0x7d8]")],
    # The speed it steers toward, into mSetSpeed (+ 0x318, the flyer's + 0x558):
    # the class's BoostSpeed (XMM3; every way into 0x004AE671 passes 0x004AE145)
    # while boosting (flag bit 0x04) with one; else MidSpeed + (MidSpeed -
    # MinSpeed) x the throttle held back, MidSpeed + (MaxSpeed - MidSpeed) x the
    # throttle held forward, through the capped getters.
    0x004AE145: [("mov", "edx, dword ptr [edi + 0x3ec]"), ("movss", "xmm3, dword ptr [edx + 0x7d0]")],
    0x004AE671: [("test", "byte ptr [edi + 0x374], 4"), ("je", "0x4ae690"), ("ucomiss", "xmm3, xmm7"),
                 ("lahf", ""), ("test", "ah, 0x44"), ("jnp", "0x4ae690"),
                 ("movss", "dword ptr [edi + 0x318], xmm3")],
    0x004AE690: [("movss", "xmm5, dword ptr [esp + 0x88]"), ("mov", "ecx, esi"), ("comiss", "xmm7, xmm5"),
                 ("jbe", "0x4ae6cb"), ("call", "0x4ac3e0"), ("movaps", "xmm3, xmm0"), ("call", "0x4ac420")],
    0x004AE6B0: [("call", "0x4ac3e0"), ("subss", "xmm2, xmm0"), ("mulss", "xmm2, xmm5"),
                 ("subss", "xmm3, xmm2"), ("movss", "dword ptr [edi + 0x318], xmm3")],
    0x004AE6CB: [("call", "0x4ac3a0"), ("mov", "ecx, esi"), ("movaps", "xmm2, xmm0"), ("call", "0x4ac3e0"),
                 ("subss", "xmm2, xmm0"), ("mulss", "xmm2, xmm5"), ("call", "0x4ac3e0")],
    0x004AE6E7: [("addss", "xmm2, xmm0"), ("movss", "dword ptr [edi + 0x318], xmm2")],
}
# EntityWalker on Steam and GOG, which have it at the same addresses.
retail_walker = {
    # UpdateState: mClass and its MaxSpeed; the flags, then ground contact (0x40)
    # zeroing m_fGroundedTimer and ending a jump (0x80); BoostSpeed over MaxSpeed;
    # the boost bit.
    0x005029FD: [("mov", "eax, dword ptr [edi + 0x460]"), ("movss", "xmm0, dword ptr [eax + 0x6a0]"),
                 ("mov", "eax, dword ptr [edi + 0x2020]")],
    0x00502A17: [("test", "al, 0x40"), ("je", "0x502a32"), ("and", "eax, 0xffffff7f"),
                 ("mov", "dword ptr [edi + 0x2074], 0"), ("mov", "dword ptr [edi + 0x2020], eax")],
    0x00502AF1: [("movss", "xmm0, dword ptr [eax + 0x6a4]"), ("comiss", "xmm0, dword ptr [eax + 0x6a0]")],
    0x00502B46: [("test", "byte ptr [edi + 0x20ec], 1")],
    0x005034D8: [("cmp", "dword ptr [edi + 0x2078], 3")],                  # mState == dying
    # DoFootImpactEffects: the class's mNumFeet, mVelocity for the sounds, and
    # the bit of the foot that landed set in mFootState.
    0x00500736: [("movzx", "eax, byte ptr [eax + 0xc5a]")],
    0x00500905: [("lea", "eax, [edi + 0x468]")],
    0x0050091E: [("or", "dword ptr [edi + 0x2064], eax")],
    # Jump, from the Controllable part (the walker + 0x240): mClass, JumpVerticalSpeed,
    # then the flags (jumping, no ground contact) and m_fGroundedTimer.
    0x00500D57: [("mov", "eax, dword ptr [edx + 0x220]")],
    0x00500D65: [("movss", "xmm0, dword ptr [eax + 0x6a8]")],
    0x00500DFC: [("mov", "eax, dword ptr [edx + 0x1de0]"), ("and", "eax, 0xffffffbf"),
                 ("mov", "dword ptr [edx + 0x1e34], 0"), ("or", "eax, 0x80"),
                 ("mov", "dword ptr [edx + 0x1de0], eax")],
}
# EntityHover on Steam and GOG, which have it at the same addresses.
retail_hover = {
    # CollisionCallback (this = the hover + 0xC): the other object and its
    # collision type, the hover's velocity, the other's GameObject and its
    # velocity, the contact normal, soldiers left out, the hover handed on,
    # and the returns.
    0x004C66A8: [("mov", "edi, dword ptr [ebp + 0xc]"), ("mov", "esi, ecx"), ("mov", "ecx, dword ptr [edi + 4]"),
                 ("test", "ecx, ecx"), ("je", "0x4c66bd"), ("mov", "eax, dword ptr [edi + 8]"),
                 ("mov", "ecx, dword ptr [ecx + eax*4 + 4]"), ("jmp", "0x4c66c0"),
                 ("mov", "ecx, dword ptr [edi + 0x20]")],
    # The hover's own type, the same way; nothing below it goes further.
    0x004C66C0: [("mov", "edx, dword ptr [esi + 4]"), ("test", "edx, edx"), ("je", "0x4c66d0"),
                 ("mov", "eax, dword ptr [esi + 8]"), ("mov", "eax, dword ptr [edx + eax*4 + 4]"),
                 ("jmp", "0x4c66d3"), ("mov", "eax, dword ptr [esi + 0x20]"), ("cmp", "ecx, eax"),
                 ("jl", "0x4c6e73")],
    0x004C66E3: [("movq", "xmm0, qword ptr [esi + 0x454]")],
    0x004C66FD: [("mov", "eax, dword ptr [eax + 0x38]"), ("call", "eax")],
    0x004C670F: [("mov", "ecx, eax"), ("mov", "edx, dword ptr [eax]"), ("mov", "eax, dword ptr [edx + 0x44]"),
                 ("call", "eax")],
    0x004C676D: [("lea", "ecx, [edx + 0xc]")],
    0x004C67A7: [("cmp", "eax, 2")],
    0x004C67DD: [("lea", "ecx, [esi - 0xc]")],
    0x004C6814: [("ret", "0xc")],
    0x004C6928: [("ret", "0xc")],
    # Move: the class with its BoostSpeed and ForwardSpeed, the matrix, the
    # velocity, the jump bit (tested, set, cleared), the up and forward rows.
    0x004C3FE2: [("mov", "eax, dword ptr [esi + 0x48c]")],
    0x004C4059: [("lea", "eax, [esi + 0xf0]")],
    0x004C40B5: [("movss", "xmm0, dword ptr [eax + 0xdec]")],
    0x004C40ED: [("test", "byte ptr [esi + 0x1d01], 2")],
    0x004C417F: [("lea", "edi, [esi + 0x460]")],
    0x004C42C2: [("movss", "xmm2, dword ptr [eax + 0x600]")],
    0x004C44A3: [("mulss", "xmm0, dword ptr [esi + 0x110]")],
    0x004C44CA: [("or", "cl, 2"), ("mov", "byte ptr [esi + 0x1d01], cl")],
    0x004C52D0: [("and", "byte ptr [esi + 0x1d01], 0xfd")],
    0x004C5283: [("movss", "xmm2, dword ptr [esi + 0x104]")],
    # Update works from the Controllable part (the hover + 0x240, its matrix at
    # -0x150): mBoost from the sprint input, then Move.
    0x004C2ECB: [("lea", "ecx, [ebx + 0x1ac0]"), ("movss", "xmm4, dword ptr [ebp + 8]"),
                 ("mov", "byte ptr [ecx], al")],
    0x004C2FA0: [("movq", "xmm0, qword ptr [ebx - 0x150]")],
    0x004C2FD0: [("call", "0x4c3fa0")],
    # PostCollisionUpdate takes the frame time off the ground ratio, floored at
    # 0; UpdateColliderBody sets it to 1 while a spring touches ground.
    0x004C343D: [("movss", "dword ptr [edi + 0x478], xmm1")],
    0x004C3447: [("mov", "dword ptr [edi + 0x478], 0")],
    0x004C61C5: [("mov", "dword ptr [esi + 0x478], 0x3f800000")],
    0x00402BA0: [("push", "{hover_rtti_name}")],                            # rttiHashEntityHover
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
        0x004F3072: [("fstp", "dword ptr [edx + 0x5f8]")],                  # mGetSpeedSpeed
        0x004F3AE2: [("or", "byte ptr [ecx + 0x5f4], 2")],                  # FlipAdd: flipping
        # The class's MinSpeed, MidSpeed, MaxSpeed (GetFlyer*Speed) and BoostSpeed
        0x004F0A31: [("fmul", "dword ptr [eax + 0x88c]")],
        0x004F09CB: [("fld", "dword ptr [eax + 0x890]")],
        0x004F096B: [("fld", "dword ptr [eax + 0x894]")],
        0x004F2FDA: [("fld", "dword ptr [ecx + 0x898]")],
        # Inside a landing region (mInLandingRegionFactor, flyer + 0x5FC, non-zero)
        # the three are capped by land_speed_max, _mid, _min and _min_mult.
        0x004F0956: [("fld", "dword ptr [ecx + 0x5fc]")],
        0x004F0971: [("fcom", "dword ptr [0xacdc60]")],
        0x004F09D1: [("fcom", "dword ptr [0xacdc64]")],
        0x004F0A2B: [("fld", "dword ptr [0xacdc6c]"), ("fmul", "dword ptr [eax + 0x88c]"),
                     ("fcom", "dword ptr [0xacdc68]")],
        # EntityFlyer::Update works from the flyer's Controllable part, the flyer +
        # 0x240, and reads the class's PitchRate and TurnRate.
        0x004FD86D: [("lea", "ecx, [ebx - 0x240]")],
        0x004FD752: [("fld", "dword ptr [edx + 0x8a0]"), ("fld", "dword ptr [edx + 0x8a4]")],
        # RecalculateSpeed takes mVelocity along the matrix's forward axis, + 0x110.
        0x004F3050: [("fmul", "dword ptr [edx + 0x118]")],
        0x004F305C: [("fmul", "dword ptr [edx + 0x114]")],
        0x004F3064: [("fld", "dword ptr [edx + 0x110]")],
        # The speed it steers toward, into mSetSpeed (flyer + 0x598): BoostSpeed
        # while boosting with one, else from mControlMove (Controllable + 0x80).
        0x004FD376: [("mov", "eax, dword ptr [ebx + 0x80]")],
        # The roll input next, mControlStrafe (Controllable + 0x84), with its 0.3
        # dead zone.
        0x004FD39D: [("fld", "dword ptr [ebx + 0x84]")],
        0x004FD3AB: [("fld", "st(0)"), ("fabs", ""), ("fcomp", "dword ptr [0xa2c65c]")],
        0x004FECF9: [("fld", "dword ptr [esi + 0x898]")],
        0x004FED08: [("mov", "ecx, dword ptr [esi + 0x898]"), ("mov", "dword ptr [ebx + 0x358], ecx")],
        0x004FED19: [("fld", "dword ptr [esp + 0x78]")],
        0x004FED7E: [("fstp", "dword ptr [ebx + 0x358]")],
        # The flyer's two collision shakes: ApplyShake(impact x 0.8, impact x 0.7)
        # for the flyer the chase camera follows, through the thunk.
        0x004F7F20: [("fmul", "dword ptr [0xa2c664]")],
        0x004F7F31: [("fmul", "dword ptr [0xa2a9b4]"), ("fstp", "dword ptr [esp]"),
                     ("call", "0x4162d4")],
        0x00503216: [("fmul", "dword ptr [0xa2c664]")],
        0x00503227: [("fmul", "dword ptr [0xa2a9b4]"), ("fstp", "dword ptr [esp]"),
                     ("call", "0x4162d4")],
        0x004162D4: [("jmp", "{camera_manager_apply_shake}")],
        0x004A06C4: [("ret", "8")],
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
        # WeaponMelee::UpdateFire: a new attack goes on the front of m_pDamageData;
        # each object struck is listed (eight at most) before the object is asked
        # to block (+0xD4, Deflect).
        0x00639350: [("mov", "ecx, dword ptr [ebx + 0x1d8]"), ("mov", "dword ptr [eax + 0x2c], ecx"),
                     ("mov", "dword ptr [ebx + 0x1d8], eax")],
        0x00639EA5: [("cmp", "ecx, 8")],
        0x00639EB2: [("mov", "dword ptr [eax + ecx*4 + 0xc], ebx")],
        0x00639FBD: [("call", "dword ptr [eax + 0xd4]"), ("test", "al, al")],
        0x0063A1ED: [("ret", "4")],
        # WeaponMelee::Deflect: the owner, and both returns
        0x0063767F: [("mov", "eax, dword ptr [ebx + 0x6c]")],
        0x006378FD: [("ret", "0xc")],
        0x00638AB7: [("ret", "0xc")],
        # EntityWalker, as on retail: UpdateState's mClass, MaxSpeed, flags, ground
        # contact and m_fGroundedTimer, BoostSpeed over MaxSpeed and the boost bit.
        0x0055B540: [("mov", "edx, dword ptr [esi + 0x498]"), ("mov", "eax, dword ptr [edx + 0x768]")],
        0x0055B550: [("mov", "eax, dword ptr [esi + 0x2060]"), ("test", "al, 0x40")],
        0x0055B562: [("and", "eax, 0xffffff7f"), ("mov", "dword ptr [esi + 0x20b4], edi"),
                     ("mov", "dword ptr [esi + 0x2060], eax")],
        0x0055B579: [("fadd", "dword ptr [esi + 0x20b4]"), ("fstp", "dword ptr [esi + 0x20b4]")],
        0x0055B618: [("fld", "dword ptr [eax + 0x76c]"), ("fcomp", "dword ptr [eax + 0x768]")],
        0x0055B674: [("test", "byte ptr [esi + 0x212c], 1")],
        0x0054F340: [("test", "byte ptr [ecx + 0x2060], 2")],
        0x0054F349: [("cmp", "dword ptr [ecx + 0x20b8], 4")],             # mState == dead
        # DoFootImpactEffects: mNumFeet, mVelocity, and the landed foot's bit.
        0x00555B76: [("movzx", "eax, byte ptr [eax + 0xd22]")],
        0x00555E2A: [("lea", "edx, [esi + 0x4a0]")],
        0x0055604D: [("mov", "edx, dword ptr [esi + 0x20a4]"), ("mov", "ecx, edi"), ("mov", "eax, 1"),
                     ("shl", "eax, cl"), ("mov", "ecx, dword ptr [esi + 0x498]"), ("or", "edx, eax"),
                     ("mov", "dword ptr [esi + 0x20a4], edx")],
        # Jump, from the Controllable part (the walker + 0x240).
        0x0054F776: [("mov", "ecx, dword ptr [esi + 0x258]"), ("fld", "dword ptr [ecx + 0x770]")],
        0x0054F7DC: [("mov", "eax, dword ptr [esi + 0x1e20]"), ("and", "eax, 0xffffffbf"), ("pop", "edi"),
                     ("or", "eax, 0x80"), ("mov", "dword ptr [esi + 0x1e20], eax")],
        0x0054F7F2: [("mov", "dword ptr [esi + 0x1e74], 0")],
        # EntityHover, as on retail. CollisionCallback (this = the hover + 0xC).
        0x005155B7: [("mov", "edi, dword ptr [esp + 0x44]"), ("mov", "eax, dword ptr [edi + 4]"),
                     ("test", "eax, eax"), ("mov", "ebp, ecx"), ("je", "0x5155cd"),
                     ("mov", "ecx, dword ptr [edi + 8]"), ("mov", "eax, dword ptr [eax + ecx*4 + 4]"),
                     ("jmp", "0x5155d0"), ("mov", "eax, dword ptr [edi + 0x20]")],
        0x005155D0: [("mov", "ecx, dword ptr [ebp + 4]"), ("test", "ecx, ecx"), ("je", "0x5155e0"),
                     ("mov", "edx, dword ptr [ebp + 8]"), ("mov", "ecx, dword ptr [ecx + edx*4 + 4]"),
                     ("jmp", "0x5155e3"), ("mov", "ecx, dword ptr [ebp + 0x20]"), ("cmp", "eax, ecx"),
                     ("jl", "0x515b19")],
        0x005155EB: [("lea", "esi, [ebp + 0x48c]")],
        0x00515603: [("mov", "edx, dword ptr [edi]"), ("mov", "ecx, edi"), ("mov", "dword ptr [esp + 0x2c], eax"),
                     ("call", "dword ptr [edx + 0x38]")],
        0x00515619: [("mov", "edx, dword ptr [eax]"), ("mov", "ecx, eax"), ("call", "dword ptr [edx + 0x44]")],
        0x00515657: [("lea", "ebx, [edx + 0xc]")],
        0x00515676: [("cmp", "eax, 2")],
        0x005156A4: [("lea", "ecx, [ebp - 0xc]")],
        0x005156E4: [("ret", "0xc")],
        0x00515794: [("ret", "0xc")],
        # Move: the class, the matrix, BoostSpeed, the jump bit, the forward row,
        # the velocity, ForwardSpeed; ApplyCollisionImpact's up row.
        0x0050E46F: [("mov", "eax, dword ptr [ebx + 0x4c4]")],
        0x0050E4E1: [("lea", "eax, [ebx + 0xf0]")],
        0x0050E576: [("fld", "dword ptr [ecx + 0xec0]")],
        0x0050E5AF: [("test", "byte ptr [ebx + 0x1d41], 2")],
        0x0050F47D: [("fmul", "dword ptr [ebx + 0x110]")],
        0x0050F49A: [("or", "cl, 2"), ("mov", "byte ptr [ebx + 0x1d41], cl")],
        0x0050F5C0: [("and", "byte ptr [ebx + 0x1d41], 0xfd")],
        0x0050FCDB: [("lea", "eax, [ebx + 0x498]")],
        0x0050FD4D: [("fld", "dword ptr [ecx + 0x6c8]")],
        0x0051529F: [("fld", "dword ptr [ebx + 0x104]")],
        # Update, from the Controllable part (the hover + 0x240): mBoost, then
        # Move on the hover, through the thunk.
        0x00513AFA: [("lea", "ecx, [esi + 0x1b00]"), ("mov", "byte ptr [ecx], al")],
        0x00513C0F: [("lea", "edi, [esi - 0x240]"), ("mov", "ecx, edi")],
        0x00513C2D: [("call", "0x40c897")],
        0x0040C897: [("jmp", "0x50e460")],
        # PostCollisionUpdate: the class, the ground ratio less the frame time,
        # floored at 0; UpdateColliderBody: 1 while a spring touches ground.
        0x0051450A: [("mov", "eax, dword ptr [ebx + 0x4c4]")],
        0x0051453D: [("fld", "dword ptr [ebx + 0x4b0]")],
        0x00514559: [("fst", "dword ptr [ebx + 0x4b0]")],
        0x00514575: [("mov", "dword ptr [ebx + 0x4b0], 0")],
        0x0050CAAB: [("mov", "dword ptr [esi + 0x4b0], 0x3f800000")],
        0x00A16A90: [("push", "{hover_rtti_name}")],                       # rttiHashEntityHover
    },
    "steam": {**retail_flyer, **retail_walker, **retail_hover, **{
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
        # WeaponMelee::UpdateFire and Deflect, as on modtools
        0x0068C537: [("mov", "ecx, dword ptr [edi + 0x1a8]"), ("mov", "dword ptr [edx + 0x2c], ecx"),
                     ("mov", "dword ptr [edi + 0x1a8], edx")],
        0x0068CF18: [("mov", "dword ptr [edx + eax*4 + 0xc], esi")],
        0x0068CF37: [("inc", "dword ptr [edx + 8]")],
        0x0068CFAD: [("mov", "eax, dword ptr [esi]"), ("mov", "eax, dword ptr [eax + 0xd4]"),
                     ("call", "eax"), ("test", "al, al")],
        0x0068D178: [("ret", "4")],
        0x0068A6C9: [("ret", "0xc")],
        0x0068B0F8: [("ret", "0xc")],
        # CameraManager::ApplyShake: the amount from XMM1, the duration from XMM2,
        # a plain RET.
        0x0044F4CE: [("movss", "dword ptr [ecx + eax*4 + 0xa0], xmm1")],
        0x0044F4DD: [("divss", "xmm1, xmm2"), ("movss", "dword ptr [ecx + eax*4 + 0xb0], xmm1"),
                     ("inc", "dword ptr [ecx + 0x9c]"), ("ret", "")],
        # The flyer's two collision shakes, in PostCollisionUpdate and
        # CollisionCallback: ApplyShake(impact x 0.8, impact x 0.7) with the camera
        # manager in ECX and EDX. After the call the impact in XMM0 and the
        # manager in EDX are used again, for the chase camera's mTimer.
        0x004B24CE: [("movss", "xmm0, dword ptr [esp + 0xc]"), ("mov", "edx, dword ptr [0x1e30324]"),
                     ("movaps", "xmm1, xmm0"), ("mulss", "xmm1, dword ptr [0x7b20a0]")],
        0x004B24E5: [("movaps", "xmm2, xmm0"), ("mov", "ecx, edx"), ("mulss", "xmm2, dword ptr [0x7b2070]"),
                     ("call", "{camera_manager_apply_shake}"), ("comiss", "xmm0, dword ptr [0x7b2014]"),
                     ("jbe", "0x4b254d"), ("mov", "eax, dword ptr [edx + 0x28]")],
        0x004B4D07: [("movss", "xmm0, dword ptr [esp + 0x10]"), ("mov", "edx, dword ptr [0x1e30324]"),
                     ("movaps", "xmm1, xmm0"), ("mulss", "xmm1, dword ptr [0x7b20a0]")],
        0x004B4D1E: [("movaps", "xmm2, xmm0"), ("mov", "ecx, edx"), ("mulss", "xmm2, dword ptr [0x7b2070]"),
                     ("call", "{camera_manager_apply_shake}"), ("comiss", "xmm0, dword ptr [0x7b2014]"),
                     ("jbe", "0x4b4d8a"), ("mov", "eax, dword ptr [edx + 0x28]")],
        # The getters' landing-region caps: MaxSpeed, MidSpeed, MinSpeed's share
        # and MinSpeed.
        0x004AC3C3: [("movss", "xmm1, dword ptr [0x7b2308]")],
        0x004AC403: [("movss", "xmm1, dword ptr [0x7b22d4]")],
        0x004AC443: [("mulss", "xmm0, dword ptr [0x7b1f88]"), ("movss", "xmm1, dword ptr [0x7b228c]")],
    }},
    "gog": {**retail_flyer, **retail_walker, **retail_hover, **{
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
        0x0068D5C7: [("mov", "ecx, dword ptr [edi + 0x1a8]"), ("mov", "dword ptr [edx + 0x2c], ecx"),
                     ("mov", "dword ptr [edi + 0x1a8], edx")],
        0x0068DFA8: [("mov", "dword ptr [edx + eax*4 + 0xc], esi")],
        0x0068DFC7: [("inc", "dword ptr [edx + 8]")],
        0x0068E03D: [("mov", "eax, dword ptr [esi]"), ("mov", "eax, dword ptr [eax + 0xd4]"),
                     ("call", "eax"), ("test", "al, al")],
        0x0068E208: [("ret", "4")],
        0x0068B759: [("ret", "0xc")],
        0x0068C188: [("ret", "0xc")],
        0x0044F4AE: [("movss", "dword ptr [ecx + eax*4 + 0xa0], xmm1")],
        0x0044F4BD: [("divss", "xmm1, xmm2"), ("movss", "dword ptr [ecx + eax*4 + 0xb0], xmm1"),
                     ("inc", "dword ptr [ecx + 0x9c]"), ("ret", "")],
        0x004B24CE: [("movss", "xmm0, dword ptr [esp + 0xc]"), ("mov", "edx, dword ptr [0x1e317c4]"),
                     ("movaps", "xmm1, xmm0"), ("mulss", "xmm1, dword ptr [0x7b3018]")],
        0x004B24E5: [("movaps", "xmm2, xmm0"), ("mov", "ecx, edx"), ("mulss", "xmm2, dword ptr [0x7b2fe8]"),
                     ("call", "{camera_manager_apply_shake}"), ("comiss", "xmm0, dword ptr [0x7b2f8c]"),
                     ("jbe", "0x4b254d"), ("mov", "eax, dword ptr [edx + 0x28]")],
        0x004B4D07: [("movss", "xmm0, dword ptr [esp + 0x10]"), ("mov", "edx, dword ptr [0x1e317c4]"),
                     ("movaps", "xmm1, xmm0"), ("mulss", "xmm1, dword ptr [0x7b3018]")],
        0x004B4D1E: [("movaps", "xmm2, xmm0"), ("mov", "ecx, edx"), ("mulss", "xmm2, dword ptr [0x7b2fe8]"),
                     ("call", "{camera_manager_apply_shake}"), ("comiss", "xmm0, dword ptr [0x7b2f8c]"),
                     ("jbe", "0x4b4d8a"), ("mov", "eax, dword ptr [edx + 0x28]")],
        0x004AC3C3: [("movss", "xmm1, dword ptr [0x7b3280]")],
        0x004AC403: [("movss", "xmm1, dword ptr [0x7b324c]")],
        0x004AC443: [("mulss", "xmm0, dword ptr [0x7b2f00]"), ("movss", "xmm1, dword ptr [0x7b3204]")],
    }},
}

# WeaponMelee's vtable: Deflect (+0x48) and UpdateFire (+0xA4) are the hooked
# functions, and IsMelee (+0x54) answers true. modtools goes through thunks.
melee_vtables = {"modtools": 0x00A54210, "steam": 0x007B1578, "gog": 0x007B24F0}

# EntityHover's and CommandHover's CollisionObject vtables, each with the
# constructor store that puts it at +0xC: (store, vtable). Slot 6 (+0x18) of
# both is the detoured callback; modtools goes through a thunk.
hover_vtables = {
    "modtools": [(0x005118E5, 0x00A3DE68), (0x0064907B, 0x00A57258)],
    "steam": [(0x004C0ABC, 0x0079BC98), (0x00478FD9, 0x0079834C)],
    "gog": [(0x004C0ABC, 0x0079CC38), (0x00478FD9, 0x007992EC)],
}

# EntityFlyer's functions the flyer shakes read in, found through its vtables:
# the constructor store that puts the vtable in place (at the part's offset),
# the vtable, the slot, the function there, and sites above that lie in it.
# PostCollisionUpdate (primary, slot 71) and CollisionCallback (CollisionObject
# part, slot 6) make the two collision CALLs; Update (Controllable part, slot 1)
# reads the throttle, the roll and the turn rates. modtools goes through thunks.
retail_update_sites = (0x004AC470, 0x004ACACB, 0x004ACD81, 0x004ACDA6, 0x004AD113, 0x004AD146, 0x004AE145,
                       0x004AE671, 0x004AE6E7)
flyer_functions = {
    "modtools": [
        (0x004F2876, 0x000, 0x00A3CDC8, 0x11C, 0x004F79B0, ("flyer_post_collision_shake_call", 0x004F7F20)),
        (0x004F287C, 0x00C, 0x00A3CD68, 0x018, 0x005025B0, ("flyer_collision_shake_call", 0x00503216)),
        (0x004F28A1, 0x240, 0x00A3CBC8, 0x004, 0x004FC930,
         (0x004FD376, 0x004FD39D, 0x004FD752, 0x004FD86D, 0x004FECF9, 0x004FED19)),
    ],
    "steam": [
        (0x004AAB53, 0x000, 0x0079B49C, 0x11C, 0x004B1E70, ("flyer_post_collision_shake_call", 0x004B24CE)),
        (0x004AAB59, 0x00C, 0x0079B20C, 0x018, 0x004B3DE0, ("flyer_collision_shake_call", 0x004B4D07)),
        (0x004AAB7E, 0x240, 0x0079B3CC, 0x004, 0x004AC460, retail_update_sites),
    ],
    "gog": [
        (0x004AAB53, 0x000, 0x0079C43C, 0x11C, 0x004B1E70, ("flyer_post_collision_shake_call", 0x004B24CE)),
        (0x004AAB59, 0x00C, 0x0079C1AC, 0x018, 0x004B3DE0, ("flyer_collision_shake_call", 0x004B4D07)),
        (0x004AAB7E, 0x240, 0x0079C36C, 0x004, 0x004AC460, retail_update_sites),
    ],
}

# Per build, the stock bump's amount and duration factors (0.8 and 0.7) the
# collision sites multiply by, and the landing-region caps the getters read:
# MaxSpeed 60, MidSpeed 20, MinSpeed 10 and MinSpeed's share 0.2.
bump_factors = {"modtools": (0xA2A9B4, 0xA2C664), "steam": (0x7B20A0, 0x7B2070), "gog": (0x7B3018, 0x7B2FE8)}
landing_caps = {
    "modtools": (0xACDC60, 0xACDC64, 0xACDC68, 0xACDC6C),
    "steam": (0x7B2308, 0x7B22D4, 0x7B228C, 0x7B1F88),
    "gog": (0x7B3280, 0x7B324C, 0x7B3204, 0x7B2F00),
}
for build, constants in list(bump_factors.items()) + list(landing_caps.items()):
    operands = " ".join(ops for instructions in sites[build].values() for _, ops in instructions)
    for va in constants:
        assert hex(va) in operands, (build, hex(va), "not read at any site")
assert "constexpr float kStockBumpAmount = 0.8f;" in core, "the stock amount factor"
for name, value in (("kLandSpeedMax", "60.0f"), ("kLandSpeedMid", "20.0f"),
                    ("kLandSpeedMin", "10.0f"), ("kLandSpeedMinMult", "0.2f")):
    assert re.search(r"constexpr float " + name + r"\s*=\s*" + re.escape(value), core), name

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


def relocations(pe):
    """The addresses the loader moves when it loads the exe elsewhere (HIGHLOW entries)."""
    if not hasattr(pe, "DIRECTORY_ENTRY_BASERELOC"):
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])
    base = pe.OPTIONAL_HEADER.ImageBase
    return {base + e.rva for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", [])
            for e in block.entries if e.type == pefile.RELOCATION_TYPE["IMAGE_REL_BASED_HIGHLOW"]}


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

        # And the hover's, which CommandHover answers too.
        hover_push = [a for a in sites[build] if sites[build][a] == [("push", "{hover_rtti_name}")]][0]
        hover_va = int.from_bytes(image[hover_push + 1 - base:hover_push + 5 - base], "little")
        assert image[hover_va - base:hover_va - base + 12] == b"EntityHover\x00", (build, "hover RTTI name")
        after = list(decoder.disasm(image[hover_push + 5 - base:hover_push + 20 - base], hover_push + 5, count=2))
        assert after[0].mnemonic == "mov" and after[0].op_str.startswith("ecx, ") and \
            after[1].mnemonic == "call", (build, "hover RTTI hash call")
        names["hover_rtti_name"] = hex(hover_va)

        for address_name, per_kind in GUARDS.items():
            raw, mask = per_kind[kind]
            want = codecs.decode(raw, "unicode_escape").encode("latin1")
            assert len(want) == len(mask), (build, address_name, "guard length")
            va = addrs[address_name]
            got = image[va - base:va - base + len(want)]
            assert all(m != "x" or g == w for g, w, m in zip(got, want, mask)), \
                (build, address_name, got.hex())

        # Steam and GOG are always loaded away from their build address, and the
        # loader moves every absolute address in them first. The guards compare
        # bytes as built, so none of the bytes they compare may be one it moves,
        # nor may the two CALLs that are retargeted (a CALL's target is relative).
        moved = relocations(pe)
        if kind == "modtools":
            assert pe.FILE_HEADER.Characteristics & 0x0001 and not moved, (build, "modtools has a fixed base")
        else:
            assert pe.OPTIONAL_HEADER.DllCharacteristics & 0x0040, (build, "retail is built to be relocated")
        for address_name, per_kind in GUARDS.items():
            va = addrs[address_name]
            compared = {va + i for i, m in enumerate(per_kind[kind][1]) if m == "x"}
            hits = [hex(r) for r in moved if compared & set(range(r, r + 4))]
            assert not hits, (build, address_name, "compares bytes the loader moves", hits)
        for name in ("flyer_post_collision_shake_call", "flyer_collision_shake_call"):
            assert not [r for r in moved if addrs[name] - 3 <= r < addrs[name] + 5], (build, name)

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

        def slot_target(table_va, offset):
            va = dword(table_va + offset)
            ins = list(decoder.disasm(image[va - base:va - base + 5], va, count=1))[0]
            return int(ins.op_str, 16) if ins.mnemonic == "jmp" else va

        melee = melee_vtables[build]
        assert slot_target(melee, 0x48) == addrs["weapon_melee_deflect"], (build, "WeaponMelee vtable: Deflect")
        assert slot_target(melee, 0xA4) == addrs["weapon_melee_update_fire"], (build, "WeaponMelee vtable: UpdateFire")
        is_melee = slot_target(melee, 0x54)
        assert image[is_melee - base:is_melee - base + 3] == b"\xb0\x01\xc3", (build, "WeaponMelee::IsMelee")
        assert slot_target(melee, 0x4C) == addrs["weapon_signal_fire"], (build, "WeaponMelee uses Weapon::SignalFire")

        for store_va, table_va in hover_vtables[build]:
            store = list(decoder.disasm(image[store_va - base:store_va - base + 10], store_va, count=1))[0]
            assert store.mnemonic == "mov" and store.op_str.endswith("+ 0xc], " + hex(table_va)), \
                (build, hex(store_va), store.op_str)
            assert slot_target(table_va, 0x18) == addrs["hover_collision_callback"], \
                (build, hex(table_va), "hover CollisionCallback slot")

        def lies_in(fn, va):
            """Whether decoding from fn reaches va without leaving the function."""
            for ins in decoder.disasm(image[fn - base:va + 16 - base], fn):
                if ins.address == va:
                    return True
                if ins.address > va or ins.mnemonic == "int3":
                    return False
            return False

        for store_va, part, table_va, slot, fn, inner in flyer_functions[build]:
            store = list(decoder.disasm(image[store_va - base:store_va - base + 10], store_va, count=1))[0]
            at = "]" if part == 0 else f" + {hex(part)}]"
            assert store.mnemonic == "mov" and store.op_str.endswith(f"{at}, {hex(table_va)}"), \
                (build, hex(store_va), store.op_str)
            assert slot_target(table_va, slot) == fn, (build, hex(table_va), hex(slot))
            for site in inner:
                va = addrs[site] if isinstance(site, str) else site
                assert lies_in(fn, va), (build, hex(fn), hex(va), "not in the function")

        def f32(va):
            return struct.unpack("<f", image[va - base:va - base + 4])[0]

        # The collision sites CALL ApplyShake (modtools through its thunk), with
        # the stock amount and duration factors the bump is read back with.
        for name in ("flyer_post_collision_shake_call", "flyer_collision_shake_call"):
            call = list(decoder.disasm(image[addrs[name] - base:addrs[name] - base + 5], addrs[name]))[0]
            assert call.mnemonic == "call", (build, name)
            target = int(call.op_str, 16)
            if build == "modtools":
                target = jmp_target(target)
            assert target == addrs["camera_manager_apply_shake"], (build, name)
        amount, duration = (f32(va) for va in bump_factors[build])
        assert abs(amount - 0.8) < 1e-6 and abs(duration - 0.7) < 1e-6, (build, amount, duration)

        # The landing-region caps camera_shake_core.hpp mirrors.
        caps = [round(f32(va), 6) for va in landing_caps[build]]
        assert caps == [60.0, 20.0, 10.0, 0.2], (build, caps)

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
