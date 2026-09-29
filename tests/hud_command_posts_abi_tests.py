"""Read-only audit of the command post strip against all three game PEs.

Usage: python tests/hud_command_posts_abi_tests.py PATH_TO_GAMEDATA
Requires pefile and capstone. Reads the offsets, addresses and install guard
from the production sources and checks them, and the instructions they were
read from, against each executable. Never loads or executes the game. Not an
in-game behaviour test.
"""
import codecs
import re
import sys
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "PatcherDLL" / "src"
module = (SRC / "render" / "hud_command_posts.cpp").read_text()
layout = (SRC / "core" / "layout" / "command_post.hpp").read_text()
character = (SRC / "core" / "layout" / "character.hpp").read_text()
camera = (SRC / "core" / "layout" / "red_camera.hpp").read_text()
latch = (SRC / "render" / "target_bar_latch.cpp").read_text()
addresses = (SRC / "core" / "game_addrs.hpp").read_text()


def const(text, name):
    match = re.search(r"\b" + name + r"\s*=\s*(0x[0-9a-fA-F]+|\d+)", text)
    assert match, name
    return int(match.group(1), 0)


# The offsets the module reads, as the audit expects them.
expected = {
    (layout, "kHudIndex"): 0x1C, (layout, "kObject"): 0x2C, (layout, "kHoldTeam"): 0x40,
    (layout, "kBiasTeam"): 0x78, (layout, "kNeutralizeTimer"): 0xA0,
    (layout, "kCaptureTimer"): 0xA4, (layout, "kFlagsModtools"): 0x1A58,
    (layout, "kFlagsRelease"): 0x0B40, (layout, "kFlagHudIndexDisplay"): 0x02,
    (layout, "kClassNeutralizeTime"): 0x04, (layout, "kClassCaptureTime"): 0x08,
    (layout, "kIcon"): 0x1C, (layout, "kColor"): 0x68, (character, "kTeam"): 0x134,
    (module, "kGO_MatrixTrans"): 0x120, (module, "kGO_Flags"): 0x1FC,
    (module, "kGO_HandleId"): 0x204, (module, "kGO_Team"): 0x234,
    (module, "kTypeColor"): 7, (module, "kTypeFloat"): 4, (module, "kTypeUint"): 3,
    (module, "kTypeVector3"): 9,
    # The markers: the stock objective anchor's reads (sites below), the camera
    # the target bar already projects with, and the controlled object.
    (module, "kGO_SphereStack"): 0x10, (module, "kGO_SphereIndex"): 0x14,
    (module, "kGO_SphereCentre"): 0x18, (module, "kGO_MatrixUp"): 0x100,
    (module, "kCtrl_Trackable"): 0x18, (module, "kVt_GetGameObject"): 0x20,
    (camera, "kMatrix"): 0x30, (camera, "kTanHalfFovW"): 0x144,
    (camera, "kTanHalfFovH"): 0x148, (camera, "kRedCamera0"): 0x24,
    (latch, "kCam_Matrix"): 0x30, (latch, "kCam_TanHalfFovW"): 0x144,
    (latch, "kCam_TanHalfFovH"): 0x148, (latch, "kCM_Camera0"): 0x24,
}
for (text, name), value in expected.items():
    assert const(text, name) == value, name
assert re.search(r"kMarkerLift\s*=\s*1\.3f", module), "the stock 1.3 m lift"

guard = re.search(r'guard\(base, g_addr->net_game_is_near_local_player, "[^"]*",\s*'
                  r'modtools \? "([^"]*)"\s*: "([^"]*)",\s*"([^"]*)"\)', module)
assert guard, "Update the audit if the IsNearLocalPlayer guard changes shape"


def table(build):
    body = re.search(r"namespace " + build + r"\s*\{(.*?)\n\s*\}\s*//\s*namespace " + build,
                     addresses, re.S).group(1)
    return {name: int(value, 16) for name, value in
            re.findall(r"constexpr uintptr_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;", body)}


# (address, [(mnemonic, operands), ...]). Operands may use {name} for a
# game_addrs value of that build, formatted as capstone prints it.
common_rt = {
    0x0047BA21: [("mov", "dword ptr [esi + 0x1c], eax")],                   # HUDIndex store
    0x0047CE7F: [("mov", "dword ptr [edi + 0x40], 0")],                     # mHoldTeam reset
    0x0047D416: [("cmp", "dword ptr [edi + 0x78], ecx")],                   # mBiasTeam
    0x0047DF3F: [("movss", "xmm0, dword ptr [edi + 0xa0]")],                # mNeutralizeTimer
    0x0047D76E: [("movss", "xmm0, dword ptr [edi + 0xa4]")],                # mCaptureTimer
    0x0047BED6: [("xor", "al, byte ptr [esi + 0xb40]"), ("and", "al, 2"),   # HUDIndexDisplay
                 ("xor", "byte ptr [esi + 0xb40], al")],
    0x0047D757: [("mov", "eax, dword ptr [edi + {command_post_class_off}]")],
    0x0047F1DB: [("lea", "eax, [edi + 4]")],                                # NeutralizeTime
    0x0047F1BE: [("lea", "eax, [edi + 8]")],                                # CaptureTime
    0x0047ABB0: [("mov", "edx, dword ptr [{command_post_count_ptr}]")],     # FindPostByName
    0x0047ABD0: [("mov", "eax, dword ptr [{command_post_array_ptr}]"),
                 ("mov", "eax, dword ptr [eax + esi*4]")],
    0x0047CFA3: [("cmp", "byte ptr [{net_on_client}], 0")],
    0x0047CFBB: [("call", "{net_game_is_near_local_player}")],
}
sites = {
    "modtools": {
        0x0064C47A: [("mov", "dword ptr [ebx + 0x1c], eax")],
        0x004730C3: [("mov", "ecx, dword ptr [eax + 0x2c]")],
        0x004730F7: [("mov", "eax, dword ptr [ecx + 0x234]"), ("shl", "eax, 0x1c"),
                     ("sar", "eax, 0x1c")],
        0x0064E302: [("mov", "dword ptr [esi + 0x40], ebx")],
        0x0064E7F3: [("mov", "eax, dword ptr [esi + 0x78]")],
        0x0064EE09: [("fld", "dword ptr [esi + 0xa0]")],
        0x0064E904: [("fld", "dword ptr [esi + 0xa4]")],
        0x0064C948: [("mov", "al, byte ptr [ebx + 0x1a58]"), ("setne", "dl"), ("shl", "dl, 1")],
        0x00649A70: [("mov", "eax, dword ptr [ecx + {command_post_class_off}]"),
                     ("fld", "dword ptr [eax + 8]")],
        0x00649A80: [("mov", "eax, dword ptr [ecx + {command_post_class_off}]"),
                     ("fld", "dword ptr [eax + 4]")],
        0x006498D0: [("mov", "eax, dword ptr [{command_post_count_ptr}]")],
        0x006498E3: [("mov", "ecx, dword ptr [{command_post_array_ptr}]"),
                     ("mov", "edx, dword ptr [ecx + esi*4]")],
        0x004702E8: [("mov", "edx, dword ptr [{team_array_base}]")],
        0x00470302: [("mov", "dword ptr [esi + 0x1c], eax")],             # Team::mIcon
        0x006A8439: [("mov", "cl, byte ptr [eax + edx*4 + 0x68]")],       # Team::mColor
        0x0046E899: [("fild", "dword ptr [eax + 0x134]")],                # mTeamNumber
        0x0064E3F3: [("mov", "al, byte ptr [{net_on_client}]")],
        0x0064E407: [("call", "0x415a00")],
        0x00415A00: [("jmp", "{net_game_is_near_local_player}")],
        0x00692B94: [("cmp", "eax, 7")],                                  # EventColor
        0x00692BA5: [("mov", "eax, dword ptr [eax]"), ("mov", "ecx, dword ptr [eax]")],
        # LockOnManager::UpdateTargetVisibility: the sphere centre, from the stack
        # at +0x10 (index +0x14) when set, else +0x18; then the 1.3 m lift.
        0x00454BB1: [("mov", "ecx, dword ptr [edi + 0x10]"), ("test", "ecx, ecx"),
                     ("lea", "ebx, [edi + 0x10]"), ("je", "0x454bc8"),
                     ("mov", "eax, dword ptr [ebx + 4]"), ("add", "eax, 4"),
                     ("shl", "eax, 4"), ("add", "eax, ecx")],
        0x00454BC8: [("lea", "eax, [ebx + 8]")],
        0x00454CD7: [("mov", "dword ptr [esp + 0x34], 0x3fa66666")],
    },
    "steam": {**common_rt, **{
        0x0058FFDF: [("mov", "eax, dword ptr [ecx + 0x2c]")],
        0x00590035: [("mov", "eax, dword ptr [edx + 0x234]"), ("shl", "eax, 0x1c"),
                     ("sar", "eax, 0x1c")],
        0x0058B86A: [("mov", "eax, dword ptr [{team_array_base}]")],
        0x0058B883: [("mov", "dword ptr [esi + 0x1c], eax")],
        0x0055469B: [("mov", "al, byte ptr [edx + ecx*4 + 0x68]")],
        0x0058F85C: [("movd", "xmm0, dword ptr [eax + 0x134]")],
        0x0054A095: [("cmp", "eax, 7")],
        0x0054A0A5: [("mov", "eax, dword ptr [eax]"), ("mov", "ecx, dword ptr [eax]")],
        0x0057B769: [("mov", "edx, dword ptr [edi + 0x10]")],
        0x0057B770: [("mov", "eax, dword ptr [edi + 0x14]")],
        0x0057B776: [("shl", "eax, 4")],
        0x0057B77D: [("lea", "eax, [edi + 0x18]")],
        0x0057B8AA: [("mov", "dword ptr [ebp - 8], 0x3fa66666")],
    }},
    "gog": {**common_rt, **{
        0x00590F7F: [("mov", "eax, dword ptr [ecx + 0x2c]")],
        0x00590FD5: [("mov", "eax, dword ptr [edx + 0x234]"), ("shl", "eax, 0x1c"),
                     ("sar", "eax, 0x1c")],
        0x0058C81A: [("mov", "eax, dword ptr [{team_array_base}]")],
        0x0058C833: [("mov", "dword ptr [esi + 0x1c], eax")],
        0x0055540B: [("mov", "al, byte ptr [edx + ecx*4 + 0x68]")],
        0x005907FC: [("movd", "xmm0, dword ptr [eax + 0x134]")],
        0x0054ADE5: [("cmp", "eax, 7")],
        0x0054ADF5: [("mov", "eax, dword ptr [eax]"), ("mov", "ecx, dword ptr [eax]")],
        0x0057C4E9: [("mov", "edx, dword ptr [edi + 0x10]")],
        0x0057C4F0: [("mov", "eax, dword ptr [edi + 0x14]")],
        0x0057C4F6: [("shl", "eax, 4")],
        0x0057C4FD: [("lea", "eax, [edi + 0x18]")],
        0x0057C62A: [("mov", "dword ptr [ebp - 8], 0x3fa66666")],
    }},
}

builds = [
    ("modtools", "BF2_modtools_NoDVD_NoConsole.exe"),
    ("steam", "BattlefrontII.exe"),
    ("gog", "BattlefrontII_GoG.exe"),
]
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for build, filename in builds:
    addrs = table(build)
    names = {name: hex(value) for name, value in addrs.items()}
    pe = pefile.PE(str(Path(sys.argv[1]) / filename))
    try:
        image = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        assert base == 0x400000
        mt = build == "modtools"
        want = codecs.decode(guard.group(1) if mt else guard.group(2), "unicode_escape").encode("latin1")
        va = addrs["net_game_is_near_local_player"]
        got = image[va - base:va - base + len(want)]
        assert all(m != "x" or g == w for g, w, m in zip(got, want, guard.group(3))), \
            (build, "IsNearLocalPlayer guard", got.hex())
        for address, instructions in sites[build].items():
            want_ins = [(m, ops.format(**names)) for m, ops in instructions]
            data = image[address - base:address - base + 32]
            got_ins = [(i.mnemonic, i.op_str) for i in
                       decoder.disasm(data, address, count=len(want_ins))]
            assert got_ins == want_ins, (build, hex(address), got_ins, want_ins)
        print(f"{build}: guard and {len(sites[build])} command post sites passed")
    finally:
        pe.close()
