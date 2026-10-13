"""Read-only audit of everything real flight on paths reads, calls or redirects (modtools only).

Usage: python tests/flyer_real_flight_abi_tests.py PATH_TO_GAMEDATA
Requires pefile and capstone. Does not execute the game or modify its files.
These are instruction/layout regression checks, not an in-game behaviour test.
"""
import re
import sys
from pathlib import Path

import capstone
import pefile


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "PatcherDLL/src"
source = (SRC / "ai/flyer_real_flight.cpp").read_text()
wingmen = (SRC / "ai/flyer_wingmen.cpp").read_text()
addrs = (SRC / "core/game_addrs.hpp").read_text()


def constant(text, name):
    match = re.search(r"\b" + name + r"\s*=\s*(-?0x[0-9a-fA-F]+|-?\d+)\s*;", text)
    assert match, name
    return int(match.group(1), 0)


for name, value in {
    "kFly_Remote": 0x78,
    "kFly_Follower": 0x3CC,
    "kFly_EnergyClass": 0x618,
    "kFly_Energy": 0x61C,
    "kFly_EnergyFlags": 0x620,
    "kFly_Class": 0x66C,
    "kClass_PathClass": 0x6AC,
    "kClass_BoostSpeed": 0x898,
    "kFol_LandNow": 0xD5,
    "kVt_BranchParams": 0x1C,
    "kFly_NextBarrierCheck": 0x650,
    "kModel_RigidMask": 0x114,
    "kModel_OrdnanceMask": 0x12C,
}.items():
    assert constant(source, name) == value, name
# The reads it shares with the wingmen (tests/flyer_wingmen_abi_tests.py audits those).
for name in ("kFly_Ctrl", "kFly_State", "kFly_Flags", "kFly_FreeFlyTime", "kStateFlying", "kFlagPaths", "kFlagSliding",
             "kUC_LowLevel", "kLL_Dest", "kDestSize", "kDest_Has", "kDest_Pos", "kDest_Original", "kDest_Dir",
             "kDest_Safe", "kDest_Taper", "kDest_Speed", "kSafeHeightFull", "kVt_IsRtti", "kVt_GetVelocity",
             "kGO_Class", "kGO_MatrixIface", "kVt_GetMatrix", "kGO_Flags", "kGO_HandleId"):
    assert constant(source, name) == constant(wingmen, name), name
assert 'pbl_hash("UnitController")' in source and 'pbl_hash("PathFollowerRealFlight")' in source
assert "static_assert(sizeof(BranchParams) == 0x24" in source
assert "static_assert(offsetof(BranchParams, fallbackToClosest) == 0x21" in source
# The wingmen's hooks call into it.
assert "flyer_real_flight::on_update(obj, dt)" in wingmen and "flyer_real_flight::steer_before(flyer, onPath)" in wingmen
assert "flyer_real_flight::install(base, borrowed)" in wingmen and "flyer_real_flight::uninstall()" in wingmen

mt = re.search(r"namespace modtools\s*\{(.*?)\n\s*\}\s*//\s*namespace modtools", addrs, re.S).group(1)
for name, value in {
    "flyer_path_step": 0x005EC8F0,
    "flyer_update_path_follower": 0x004F30C0,
    "flyer_update_path_call": 0x004FDD93,
    "flyer_free_reset_call": 0x004FDDED,
    "path_follower_reset": 0x005E7290,
    "entity_flyer_land": 0x004F1380,
    "flyer_calc_dest": 0x004F4630,
    "flyer_raise_ray_call": 0x004F429E,
    "collision_ray_hit": 0x0042E230,
    "collision_model_ray_hit": 0x004359C0,
}.items():
    assert constant(mt, name) == value, name

checks = {
    # EntityFlyer::UpdatePathFollower: thiscall(dt, matrix*) on the entity ...
    0x004F30C0: [("sub", "esp, 0x50"), ("push", "ebx"), ("push", "esi"), ("mov", "esi, ecx")],
    # ... nothing for a network proxy (BL is 1) ...
    0x004F3110: [("test", "byte ptr [esi + 0x78], bl"), ("jne", "0x4f334b")],
    # ... the follower at +0x3CC, its GetBranchParams at vptr +0x1C into params at [esp+0x38] ...
    0x004F3119: [("mov", "eax, dword ptr [esi + 0x3cc]"), ("push", "edi"), ("lea", "ecx, [esp + 0x38]"),
                 ("lea", "edi, [esi + 0x3cc]"), ("push", "ecx"), ("mov", "ecx, edi"), ("call", "dword ptr [eax + 0x1c]")],
    # ... their mFallbackToClosest (+0x21) set; the boost ratio when the class's boost speed (+0x898) is above 0 ...
    0x004F3130: [("mov", "edx, dword ptr [esi + 0x66c]"), ("mov", "byte ptr [esp + 0x59], bl"),
                 ("fld", "dword ptr [edx + 0x898]"), ("fcomp", "dword ptr [0xa2a06c]")],
    0x004F314D: [("mov", "ecx, esi"), ("call", "0x415aff"), ("fstp", "dword ptr [esp + 0x10]"), ("mov", "ecx, esi"),
                 ("call", "0x402e64"), ("fdivr", "dword ptr [esp + 0x10]")],
    # ... the energy ratio: none counted with +0x620 bit 0, else +0x61C over the bar class's max ...
    0x004F3171: [("test", "byte ptr [esi + 0x620], bl"), ("jne", "0x4f319c"), ("mov", "ecx, dword ptr [esi + 0x618]"),
                 ("fld", "dword ptr [ecx]")],
    0x004F318E: [("fld", "dword ptr [esi + 0x61c]"), ("fdiv", "dword ptr [ecx]")],
    # ... the step: params, dt, velocity out (+0x580), the matrix out, 0, boost, energy; the follower in ECX ...
    0x004F31A4: [("mov", "eax, dword ptr [esp + 0xc]"), ("mov", "ecx, dword ptr [esp + 0x10]"),
                 ("mov", "edx, dword ptr [esp + 0x64]"), ("push", "eax"), ("push", "ecx"),
                 ("mov", "ecx, dword ptr [esp + 0x68]"), ("push", "0"), ("push", "edx"), ("lea", "eax, [esi + 0x580]"),
                 ("push", "eax"), ("push", "ecx"), ("lea", "edx, [esp + 0x50]"), ("push", "edx"), ("mov", "ecx, edi"),
                 ("call", "0x402a22"), ("test", "al, al"), ("je", "0x4f32c5")],
    # ... on false: 2 s of free flight, then Reset(the class's path class) ...
    0x004F32C5: [("test", "byte ptr [esi + 0x5f4], 3"), ("mov", "dword ptr [esi + 0x1cdc], 0x40000000")],
    0x004F3317: [("add", "eax, 0x6ac"), ("jmp", "0x4f3320"), ("xor", "eax, eax"), ("push", "eax"), ("mov", "ecx, edi"),
                 ("call", "0x404133")],
    # ... and on mbLandNow (+0xD5 within the follower), half a second of free flight and Land.
    0x004F332F: [("mov", "al, byte ptr [esi + 0x4a1]"), ("test", "al, al"), ("pop", "edi"), ("je", "0x4f334b"),
                 ("mov", "ecx, esi"), ("mov", "dword ptr [esi + 0x1cdc], 0x3f000000"), ("call", "0x41306b")],
    0x004F3359: [("ret", "8")],
    0x00402A22: [("jmp", "0x5ec8f0")],
    0x00404133: [("jmp", "0x5e7290")],
    0x0041306B: [("jmp", "0x4f1380")],
    0x00411CA2: [("jmp", "0x4f30c0")],
    0x004049AD: [("jmp", "0x4f52d0")],
    # EntityPathFollower::Update: thiscall, 7 arguments, RET 0x1C; dt 0 does nothing; the
    # params' matrix (+4) is the one it moves (its translation row at +0x30).
    0x005EC8F0: [("push", "ebp"), ("mov", "ebp, esp"), ("and", "esp, 0xfffffff0"), ("sub", "esp, 0xf4"),
                 ("fld", "dword ptr [0xa2a06c]"), ("push", "ebx"), ("fld", "dword ptr [ebp + 0xc]"), ("push", "esi"),
                 ("fucompp", ""), ("push", "edi"), ("mov", "ebx, ecx"), ("fnstsw", "ax"), ("test", "ah, 0x44"),
                 ("jnp", "0x5ed5cc"), ("mov", "esi, dword ptr [ebp + 8]"), ("mov", "edi, dword ptr [esi + 4]")],
    0x005ECB69: [("ret", "0x1c")],
    0x005ED3F7: [("ret", "0x1c")],
    0x005ED418: [("ret", "0x1c")],
    0x005ED5D4: [("ret", "0x1c")],
    # EntityPathFollower::Reset: thiscall(pathClass), RET 4.
    0x005E7290: [("mov", "eax, dword ptr [esp + 4]"), ("push", "esi"), ("push", "eax"), ("mov", "esi, ecx")],
    0x005E72BE: [("ret", "4")],
    # EntityFlyer::Land: thiscall() on the entity, plain RET.
    0x004F1380: [("push", "esi"), ("mov", "esi, ecx"), ("mov", "eax, dword ptr [esi + 0x5a4]"), ("cmp", "eax, 2")],
    0x004F142B: [("ret", "")],
    # GetBranchParams: `this` the follower (mCtrl at -0xC4, the class at +0x2A0, the matrix at
    # -0x2DC: the entity's +0x308, +0x66C and +0xF0), 9 dwords copied out, RET 4.
    0x004F52D0: [("sub", "esp, 0x34"), ("push", "ebx"), ("push", "esi"), ("mov", "esi, ecx"),
                 ("mov", "eax, dword ptr [esi - 0xc4]")],
    0x004F52E4: [("mov", "ecx, dword ptr [esi + 0x2a0]"), ("fld", "dword ptr [ecx + 0x898]")],
    0x004F5455: [("add", "esi, 0xfffffd24")],
    0x004F546A: [("mov", "ecx, 9")],
    0x004F547D: [("ret", "4")],
    # EntityFlyer's constructor: the follower at +0x3CC, its vtable 0x00A3CB44.
    0x0050381F: [("lea", "ebx, [esi + 0x3cc]"), ("push", "edx"), ("mov", "ecx, ebx"), ("call", "0x40e42b"),
                 ("mov", "dword ptr [ebx], 0xa3cb44")],
    # EntityFlyer::Update (EBX the Controllable, the entity +0x240): on its path (flags +0x5F4 bit 8,
    # flying, alive, mFreeFlyTime +0x1CDC not above 0, no trick) it calls UpdatePathFollower ...
    0x004FDD4A: [("mov", "cl, byte ptr [ebx + 0x3b4]"), ("test", "cl, 8"), ("je", "0x4fdd98"),
                 ("cmp", "dword ptr [ebx + 0x364], 2"), ("jne", "0x4fdd98"), ("mov", "edx, dword ptr [ebx - 0x44]"),
                 ("shr", "edx, 3"), ("test", "dl, 1"), ("je", "0x4fdd98"), ("fld", "dword ptr [ebx + 0x1a9c]"),
                 ("fcomp", "dword ptr [0xa2a06c]"), ("fnstsw", "ax"), ("test", "ah, 0x41"), ("jp", "0x4fdd98"),
                 ("test", "cl, 3"), ("jne", "0x4fdd98"), ("mov", "ecx, dword ptr [ebp + 8]"),
                 ("lea", "eax, [esp + 0x90]"), ("push", "eax"), ("push", "ecx"), ("lea", "ecx, [ebx - 0x240]"),
                 ("call", "0x411ca2")],
    # ... then tests the same again, the free-flight time read afresh, and off the path resets
    # the follower (+0x3CC) with the class's path class, every update.
    0x004FDD98: [("mov", "cl, byte ptr [ebx + 0x3b4]"), ("test", "cl, 8"), ("je", "0x4fddd3"),
                 ("cmp", "dword ptr [ebx + 0x364], 2"), ("jne", "0x4fddd3"), ("mov", "edx, dword ptr [ebx - 0x44]"),
                 ("shr", "edx, 3"), ("test", "dl, 1"), ("je", "0x4fddd3"), ("fld", "dword ptr [ebx + 0x1a9c]"),
                 ("fcomp", "dword ptr [0xa2a06c]"), ("fnstsw", "ax"), ("test", "ah, 0x41"), ("jp", "0x4fddd3"),
                 ("test", "cl, 3"), ("je", "0x4fe040"), ("mov", "eax, dword ptr [ebx + 0x42c]"), ("test", "eax, eax"),
                 ("je", "0x4fdde4"), ("add", "eax, 0x6ac"), ("jmp", "0x4fdde6"), ("xor", "eax, eax"), ("push", "eax"),
                 ("lea", "ecx, [ebx + 0x18c]"), ("call", "0x404133")],
    # CalculateDest (EBP the entity): a slide is called off only once the next barrier check
    # (+0x650) is a second overdue (the mission time from 0x0040DCD3) ...
    0x004F4682: [("call", "0x40dcd3"), ("fld", "dword ptr [ebp + 0x650]"), ("fadd", "dword ptr [0xa2a290]"),
                 ("fcompp", ""), ("fnstsw", "ax"), ("test", "ah, 5"), ("jnp", "0x4f46a2"),
                 ("test", "byte ptr [esi + 0x74], 1"), ("je", "0x4f46a9"), ("and", "byte ptr [ebp + 0x5f4], 0xef")],
    # ... and the check runs only once it is due: 0.1 s on, AvoidBarriers(flyer, myPos, point),
    # whose true slides the flyer (flags 0x10).
    0x004F47D9: [("call", "0x40dcd3"), ("fcomp", "dword ptr [ebp + 0x650]"), ("fnstsw", "ax"), ("test", "ah, 0x41"),
                 ("jne", "0x4f4842"), ("call", "0x40dcd3"), ("fadd", "dword ptr [0xa2c074]"), ("push", "esi"),
                 ("push", "ebx"), ("push", "ebp"), ("fstp", "dword ptr [ebp + 0x650]"), ("call", "0x40beb0"),
                 ("add", "esp, 0xc"), ("test", "al, al"), ("je", "0x4f4842"), ("or", "byte ptr [ebp + 0x5f4], 0x10")],
    0x0040DCD3: [("jmp", "0x732e60")],
    0x0040BEB0: [("jmp", "0x591130")],
    # Then RaiseHeight(myPos, taper, point, speed*, taperFactor), the flyer in ECX.
    0x004F48A1: [("push", "ebx"), ("mov", "ecx, ebp"), ("call", "0x409827")],
    0x00409827: [("jmp", "0x4f40a0")],
    # RaiseHeight: the flyer in EDI, stored as the ray's one ignored object ...
    0x004F40A0: [("sub", "esp, 0x54"), ("push", "ebx"), ("push", "esi"), ("push", "edi"), ("mov", "edi, ecx")],
    0x004F423B: [("mov", "dword ptr [esp + 0x2c], edi")],
    # ... then CollisionManager::RayHit(start, dir, length, &hit, 0, &ignore, 1, 0x18, false), cdecl, ST0.
    0x004F4279: [("push", "0"), ("push", "0x18"), ("push", "1"), ("lea", "edx, [esp + 0x38]"), ("push", "edx"),
                 ("push", "0"), ("lea", "eax, [esp + 0x34]"), ("push", "eax"), ("push", "ecx"),
                 ("fsubr", "dword ptr [esp + 0x74]"), ("lea", "edx, [esp + 0x58]"), ("push", "edx"),
                 ("lea", "eax, [esp + 0x74]"), ("fstp", "dword ptr [esp + 0x78]"), ("push", "eax"), ("call", "0x407581"),
                 ("fst", "dword ptr [esp + 0x38]"), ("fcomp", "dword ptr [0xa2a290]"), ("add", "esp, 0x24")],
    0x00407581: [("jmp", "0x42e230")],
    0x0042E230: [("fld", "dword ptr [0xa2a290]"), ("mov", "edx, dword ptr [esp + 0x10]"),
                 ("fld", "dword ptr [0xa2a06c]"), ("mov", "dword ptr [edx], 0")],
    0x0042E25B: [("ret", "")],
    # CollisionModel::RayHit: with ordnance only ([ebp+0x24]) a body counts if its bit is set
    # in the ordnance mask (+0x12C, +0x130 past the 32nd); RET 0x24, ST0.
    0x004359C0: [("push", "ebp"), ("mov", "ebp, esp"), ("and", "esp, 0xfffffff0"), ("sub", "esp, 0x74"),
                 ("push", "ebx"), ("push", "esi"), ("push", "edi")],
    0x00435A50: [("mov", "al, byte ptr [ebp + 0x24]"), ("test", "al, al"), ("je", "0x435a86"), ("cmp", "esi, 0x1f"),
                 ("jle", "0x435a70"), ("mov", "eax, dword ptr [edi + 0x130]")],
    0x00435A79: [("test", "dword ptr [edi + 0x12c], eax")],
    0x00435B44: [("ret", "0x24")],
    # CollisionModel::GetCollisionMask: ordnance (0x400) +0x12C, rigid (8, the vehicles') +0x114.
    0x00435649: [("cmp", "eax, 0x400"), ("je", "0x435679")],
    0x00435679: [("lea", "eax, [ecx + 0x12c]"), ("ret", "4")],
    0x0043562E: [("lea", "eax, [ecx + 0x114]"), ("ret", "4")],
}

decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
pe = pefile.PE(str(Path(sys.argv[1]) / "BF2_modtools_NoDVD_NoConsole.exe"))
image = pe.get_memory_mapped_image()
base = pe.OPTIONAL_HEADER.ImageBase
assert base == 0x400000
for address, expected in checks.items():
    data = image[address - base:address - base + 160]
    actual = [(i.mnemonic, i.op_str) for i in decoder.disasm(data, address, count=len(expected))]
    assert actual == expected, (hex(address), actual, expected)


def dword(va):
    return int.from_bytes(image[va - base:va - base + 4], "little")


# Every flyer type's follower vtable has GetBranchParams (through its thunk) at +0x1C.
for vtable in (0x00A3CB44, 0x00A3A3F0, 0x00A56B28):
    assert dword(vtable + 0x1C) == 0x004049AD, hex(vtable)
# GetCollisionMask's jump table sends rigid (8) to the +0x114 case.
table = [dword(0x00435684 + 4 * k) for k in range(4)]
assert table[image[0x00435694 - base + (8 - 4)]] == 0x0043562E
# The guards' bytes are the code's.
for va, text in ((0x004F30C0, "83EC5053568BF1"),
                 (0x004F3119, "8B86CC030000578D4C24388DBECC030000518BCFFF501C"),
                 (0x004F3130, "8B966C060000885C2459D98298080000"),
                 (0x004F3171, "849E2006000075238B8E18060000"),
                 (0x004F318E, "D9861C060000"),
                 (0x004F332F, "8A86A1040000"),
                 (0x005EC8F0, "558BEC83E4F081ECF4000000"),
                 (0x005E7290, "8B44240456508BF1"),
                 (0x004F1380, "568BF18B86A4050000"),
                 (0x004FDD8D, "8D8BC0FDFFFFE8"),
                 (0x004FDD98, "8A8BB4030000F6C108"),
                 (0x004FDDB7, "D9839C1A0000"),
                 (0x004FDDE7, "8D8B8C010000E8"),
                 (0x004F4687, "D98550060000"),
                 (0x004F47DE, "D89D50060000"),
                 (0x004F4279, "6A006A186A01"),
                 (0x004F423B, "897C242C"),
                 (0x004359C0, "558BEC83E4F083EC74"),
                 (0x00435A79, "85872C010000"),
                 (0x00435B44, "C22400")):
    want = bytes.fromhex(text.replace(" ", ""))
    assert image[va - base:va - base + len(want)] == want, hex(va)
# The same guards are the module's (as C escapes), at the offsets it checks them.
for text in (r"\x83\xEC\x50\x53\x56\x8B\xF1", r"\x8B\x86\xCC\x03\x00\x00\x57\x8D\x4C\x24\x38\x8D\xBE\xCC\x03\x00\x00\x51\x8B\xCF\xFF\x50\x1C",
             r"\x8B\x96\x6C\x06\x00\x00\x88\x5C\x24\x59\xD9\x82\x98\x08\x00\x00",
             r"\x84\x9E\x20\x06\x00\x00\x75\x23\x8B\x8E\x18\x06\x00\x00", r"\xD9\x86\x1C\x06\x00\x00",
             r"\x8A\x86\xA1\x04\x00\x00", r"\x55\x8B\xEC\x83\xE4\xF0\x81\xEC\xF4\x00\x00\x00",
             r"\x8B\x44\x24\x04\x56\x50\x8B\xF1", r"\x56\x8B\xF1\x8B\x86\xA4\x05\x00\x00", r"\x8D\x8B\xC0\xFD\xFF\xFF",
             r"\x8A\x8B\xB4\x03\x00\x00\xF6\xC1\x08", r"\xD9\x83\x9C\x1A\x00\x00", r"\x8D\x8B\x8C\x01\x00\x00",
             r"\xD9\x85\x50\x06\x00\x00", r"\xD8\x9D\x50\x06\x00\x00", r"\x6A\x00\x6A\x18\x6A\x01", r"\x89\x7C\x24\x2C",
             r"\x55\x8B\xEC\x83\xE4\xF0\x83\xEC\x74", r"\x85\x87\x2C\x01\x00\x00", r"\xC2\x24\x00"):
    assert text in source, text
for offset, va in ((0x59, 0x004F3119), (0x70, 0x004F3130), (0xB1, 0x004F3171), (0xCE, 0x004F318E), (0x108, 0x004F31C8),
                   (0x26F, 0x004F332F)):
    assert va - 0x004F30C0 == offset and f"upf + 0x{offset:X}" in source, hex(offset)
assert "upfCall + 0x24" in source and 0x004FDDB7 - 0x004FDD93 == 0x24
for name, offset, va, at in (("calcDest", 0x57, 0x004F4687, 0x004F4630), ("calcDest", 0x1AE, 0x004F47DE, 0x004F4630),
                             ("modelRay", 0xB9, 0x00435A79, 0x004359C0), ("modelRay", 0x184, 0x00435B44, 0x004359C0)):
    assert va - at == offset and f"{name} + 0x{offset:X}" in source, (name, hex(offset))
for offset, va in ((0x25, 0x004F4279), (0x63, 0x004F423B)):
    assert 0x004F429E - va == offset and f"rayCall - 0x{offset:X}" in source, hex(offset)
print(f"modtools: {len(checks)} instruction checks, the vtables and the guards passed")
