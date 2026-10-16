"""Read-only audit of everything the comm keys in single player read, call or detour (modtools only).

Usage: python tests/comm_orders_abi_tests.py PATH_TO_GAMEDATA
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
source = (SRC / "ai/comm_orders.cpp").read_text()
addrs = (SRC / "core/game_addrs.hpp").read_text()


def constant(text, name):
    match = re.search(r"\b" + name + r"\s*=\s*(-?0x[0-9a-fA-F]+|-?\d+)\s*;", text)
    assert match, name
    return int(match.group(1), 0)


for name, value in {
    # A Controllable's parts, as target_bar_latch.cpp and the flyer wingmen read them.
    "kCtl_Trackable": 0x18,
    "kVt_GetObject": 0x20,
    "kCtl_Ctrl": 0xC8,
    "kCtl_EyePoint": 0xDC,
    "kCtl_EyeDir": 0xE8,
    "kCtl_AimStart": 0x148,
    "kPC_Inputs": 0x88,
    "kInputs_Bits": 0x10,
    "kChr_PlayerId": 0x130,
    "kGO_MatrixTrans": 0x120,
    "kGO_Flags": 0x1FC,
    "kGO_HandleId": 0x204,
    "kGO_Team": 0x234,
    # AIGoalType and GoalTargetType (P), the squad query.
    "kGoalDestroy": 2,
    "kGoalDefend": 3,
    "kGoalFollow": 7,
    "kGttPos": 1,
    "kGttCharacter": 2,
    "kGttObject": 3,
    "kEvqIsFollowing": 0x0F,
    # The auto-aim's pick, a unit's class and health, its agent.
    "kCtl_Reticule": 0x164,
    "kReticuleChannels": 2,
    "kGO_Class": 0x08,
    "kVt_SoldierType": 0x24,
    "kGO_Health": 0x144,
    "kGO_MaxHealth": 0x148,
    "kHealthTypeBits": 0x7,
    "kUC_Agent": 0x2C0,
    "kRepairHelperSize": 0x30,
    "kRH_Agent": 0x04,
    # The stock request: NG_ChatMsg, TeamAssistManager's list, the pool's headroom.
    "kChatMsgSize": 0x128,
    "kMsg_Id": 0x00,
    "kMsg_Flag": 0x04,
    "kMsg_Code": 0x10,
    "kTA_List": 0x18,
    "kMaxLiveAssists": 48,
}.items():
    assert constant(source, name) == value, name
assert constant(source, "kMaxLiveAssists") < 64   # the pool's size: it must never fill
# The same offsets where other features read them.
latch = (SRC / "render/target_bar_latch.cpp").read_text()
assert constant(latch, "kCtrl_EyePoint") == constant(source, "kCtl_EyePoint")
assert constant(latch, "kCtrl_EyeDir") == constant(source, "kCtl_EyeDir")
assert constant(latch, "kCtrl_AimStart") == constant(source, "kCtl_AimStart")
assert "kAimRayMask = 0x02 | 0x08 | 0x10 | 0x20 | 0x80;" in latch
assert "constexpr int   kAimRayMask   = 0x02 | 0x08 | 0x10 | 0x20 | 0x80;" in source
wingmen = (SRC / "ai/flyer_wingmen.cpp").read_text()
assert constant(wingmen, "kFly_Ctrl") - constant(wingmen, "kFly_Controllable") == constant(source, "kCtl_Ctrl")
assert constant(wingmen, "kPC_Inputs") == constant(source, "kPC_Inputs")
assert constant(wingmen, "kInputs_Bits") == constant(source, "kInputs_Bits")
goals = (SRC / "ai/ai_goals.cpp").read_text()
assert constant(goals, "kChr_PlayerId") == constant(source, "kChr_PlayerId")
squads = (SRC / "ai/ai_squads.cpp").read_text()
assert constant(squads, "kIsFollowingChr") == constant(source, "kEvqIsFollowing")
# The squad key's recruit, which a squadmate gets again as its order ends.
formation = (SRC / "ai/squad_formation.cpp").read_text()
assert constant(formation, "kFollowChr") == constant(squads, "kFollowChr") == constant(source, "kEvtFollowChr") == 0x10
# CommandAnyFriendly, one of the squad keys a soldier class sets (through ClassParent).
assert 'kHashCommandAny = pbl_hash("CommandAnyFriendly")' in formation and "case kHashCommandAny: key = Key::CommandAny;" in formation
assert "keys->hasCommandAny && keys->commandAny" in source
assert constant(wingmen, "kCtl_Reticule") == constant(source, "kCtl_Reticule")
assert constant(wingmen, "kUC_Agent") == constant(source, "kUC_Agent")
assert constant(wingmen, "kAgent_Ctrl") == 0x0C   # the agent's mCtrl, which CalcMyAidTypes reads
hudw = (SRC / "render/hud_wingmen.cpp").read_text()
assert constant(hudw, "kGO_Health") == constant(source, "kGO_Health")
assert constant(hudw, "kGO_MaxHealth") == constant(source, "kGO_MaxHealth")
core = (SRC / "ai/comm_orders_core.hpp").read_text()
assert constant(core, "kFirstInput") == 20 and constant(wingmen, "kInputCommFormation") == 20
assert re.search(r"\bkAttack\s*=\s*6,", core) and constant(wingmen, "kInputCommAttack") == 26
assert re.search(r"\bkDefend\s*=\s*7,", core) and constant(wingmen, "kInputCommDefend") == 27
assert "static_assert(sizeof(GoalTarget) == 0x1C" in source

mt = re.search(r"namespace modtools\s*\{(.*?)\n\s*\}\s*//\s*namespace modtools", addrs, re.S).group(1)
for name, value in {
    "controller_manager_update": 0x005997A0,
    "ai_goal_add": 0x005CD130,
    "ai_goal_delete": 0x005CD4D0,
    "ai_goal_manager_instance": 0x00B8FAA4,
    "uc_force_attack_target": 0x0059EC20,
    "collision_ray_hit": 0x0042E230,
    "net_game_get_local_player": 0x006E3D20,
    "object_affiliation": 0x0055F840,
    "net_in_shell": 0x00ADABC2,
    "net_enabled": 0x00BE14F0,
    "net_enabled_next": 0x00BE14F1,
    "target_manager_add_marker": 0x0048E3A0,
    "target_manager_remove_marker": 0x0048E650,
    "ai_goal_unassign_all": 0x005CCD00,
    "net_display_message": 0x0070C490,
    "team_assist_add_request": 0x00687440,
    "team_assist_instance": 0x00BA37F4,
    "repair_calc_aid_types": 0x005BBD20,
}.items():
    assert constant(mt, name) == value, name

checks = {
    # ControllerManager::Update, cdecl(float dt): its first loop hands dt to each player
    # controller's update (vptr +0x14), sPlayerCtrls at 0x00B8EE6C.
    0x005997A0: [("push", "ecx"), ("mov", "eax, dword ptr [0xb8ee6c]"), ("push", "ebx"), ("push", "ebp"),
                 ("push", "esi"), ("mov", "esi, dword ptr [eax + 4]"), ("mov", "ecx, dword ptr [esi + 0xc]"),
                 ("test", "ecx, ecx"), ("je", "0x5997c7"), ("mov", "eax, dword ptr [esp + 0x14]"),
                 ("mov", "edx, dword ptr [ecx]"), ("mov", "esi, dword ptr [esi + 4]"), ("push", "eax"),
                 ("call", "dword ptr [edx + 0x14]")],
    # AIGoalManager::AddGoal, thiscall(team, type, weight, target), RET 0x10: the weight
    # third, the manager in ECX ...
    0x005CD130: [("fld", "dword ptr [esp + 0xc]"), ("push", "ebx"), ("fcomp", "dword ptr [0xa2a06c]"),
                 ("mov", "ebx, dword ptr [esp + 8]"), ("push", "ebp"), ("mov", "ebp, ecx")],
    0x005CD18A: [("ret", "0x10")],
    0x005CD212: [("ret", "0x10")],
    # ... its switch on the type (1 to 7, by type - 1) ...
    0x005CD1D8: [("cmp", "eax, 6")],
    0x005CD1E2: [("jmp", "dword ptr [eax*4 + 0x5cd3f0]")],
    # ... the Defend case: a target of some kind, 0x3C bytes from the pool, DefendGoal's
    # constructor (through its thunk) ...
    0x005CD295: [("push", "0x3c")],
    0x005CD2B2: [("call", "0x4122a6")],
    0x004122A6: [("jmp", "0x5cf4e0")],
    0x005CD316: [("push", "0x3c")],
    0x005CD333: [("call", "0x40ee21")],
    0x0040EE21: [("jmp", "0x5cf6c0")],
    # ... which copies the GoalTarget: its type +0, position +4, character +0x10, object
    # +0x14 and handle +0x18 (Defend and Destroy alike).
    0x005CF4EE: [("push", "3")],
    0x005CF505: [("mov", "eax, dword ptr [esp + 0x14]"), ("mov", "edx, dword ptr [eax]"),
                 ("mov", "dword ptr [esi + 0x20], edx"), ("lea", "ecx, [eax + 4]")],
    0x005CF524: [("mov", "edx, dword ptr [eax + 0x10]"), ("mov", "dword ptr [esi + 0x30], edx"),
                 ("mov", "ecx, dword ptr [eax + 0x14]"), ("mov", "dword ptr [esi + 0x34], ecx"),
                 ("mov", "edx, dword ptr [eax + 0x18]")],
    0x005CF6CE: [("push", "2")],
    0x005CF6E5: [("mov", "eax, dword ptr [esp + 0x14]"), ("mov", "edx, dword ptr [eax]"),
                 ("mov", "dword ptr [esi + 0x20], edx"), ("lea", "ecx, [eax + 4]")],
    0x005CF704: [("mov", "edx, dword ptr [eax + 0x10]"), ("mov", "dword ptr [esi + 0x30], edx"),
                 ("mov", "ecx, dword ptr [eax + 0x14]"), ("mov", "dword ptr [esi + 0x34], ecx"),
                 ("mov", "edx, dword ptr [eax + 0x18]")],
    # AIGoalManager::DeleteGoal, thiscall(goal), RET 4.
    0x005CD4D0: [("push", "esi"), ("push", "edi"), ("mov", "edi, ecx"), ("mov", "ecx, dword ptr [esp + 0xc]")],
    0x005CD517: [("ret", "4")],
    # AIGoal::UnAssignAll, thiscall() on the goal, plain RET: each member's controllable
    # (mRemote, mVehicle, mUnit), its mCtrl (+0xC8), ForceCommand (vptr +0x28) ...
    0x005CCD00: [("sub", "esp, 0x74"), ("push", "ebx"), ("push", "ebp"), ("mov", "ebx, ecx"),
                 ("mov", "ebp, dword ptr [ebx + 8]")],
    0x005CCD64: [("mov", "eax, dword ptr [eax + 0xc8]")],
    0x005CCDD3: [("call", "dword ptr [edx + 0x28]")],
    0x005CCE1B: [("ret", "")],
    # NetGame::DisplayMessage, cdecl(NG_ChatMsg*), plain RET: the sender by its id (+0, a
    # character whose mPlayerId +0x130 matches), the code (+0x10, -1 for none), the team
    # flag (+4), the yellow, then AddRequest(code, the sender) through its thunk.
    0x0070C490: [("push", "ebp"), ("mov", "ebp, esp"), ("sub", "esp, 0x17c")],
    0x0070C4F3: [("mov", "al, byte ptr [0xae2178]")],
    0x0070C5F5: [("mov", "ecx, dword ptr [ebp + 8]"), ("mov", "edx, dword ptr [ecx]")],
    0x0070C63E: [("mov", "eax, dword ptr [edx + ecx + 0x130]")],
    0x0070C6BC: [("mov", "ecx, dword ptr [ebp + 8]"), ("cmp", "dword ptr [ecx + 0x10], -1")],
    0x0070C7B2: [("mov", "eax, dword ptr [ebp + 8]"), ("mov", "ecx, dword ptr [eax + 4]")],
    0x0070C89D: [("mov", "ecx, dword ptr [ebp + 8]"), ("cmp", "dword ptr [ecx + 0x10], -1"), ("jle", "0x70c8bc"),
                 ("mov", "edx, dword ptr [ebp - 0x128]"), ("push", "edx"), ("mov", "eax, dword ptr [ebp + 8]"),
                 ("mov", "ecx, dword ptr [eax + 0x10]"), ("push", "ecx"), ("call", "0x40370b"),
                 ("add", "esp, 8")],
    0x0070C8BF: [("ret", "")],
    0x0040370B: [("jmp", "0x687440")],
    # ... NG_ChatMsg is 0x128 bytes (AddChatMessageToQue's new).
    0x0070C2FC: [("push", "0x128"), ("call", "0x7e34a0")],
    # TeamAssistManager::AddRequest, cdecl(code, Character*): 0x14 bytes from the pool at
    # 0x00BA3800, its 5 s duration, plain RETs; the pool set up with 64 (GameLoop::Init) and
    # no growth; the manager made into sInstance (Create).
    0x00687440: [("push", "0x14"), ("mov", "ecx, 0xba3800"), ("call", "0x802300"), ("test", "eax, eax"),
                 ("je", "0x68747c"), ("mov", "ecx, dword ptr [0xad74f4]")],
    0x0068747B: [("ret", "")],
    0x00687487: [("ret", "")],
    0x00734763: [("push", "0x40"), ("mov", "ecx, 0xba3800")],
    0x006873F5: [("mov", "dword ptr [0xba37f4], eax")],
    # RepairHelper::CalcMyAidTypes, thiscall(healthMask, ammoMask), RET 8: both zeroed, then
    # its +4 (the agent), the agent's mCtrl (+0xC), its owner (+4), GetWeaponCount (+0x38).
    0x005BBD20: [("push", "ecx"), ("push", "ebx"), ("push", "ebp"), ("push", "esi"),
                 ("mov", "esi, dword ptr [esp + 0x14]"), ("mov", "dword ptr [esi], 0"), ("mov", "ebx, ecx"),
                 ("push", "edi"), ("mov", "edi, dword ptr [esp + 0x1c]"), ("mov", "dword ptr [edi], 0"),
                 ("mov", "eax, dword ptr [ebx + 4]"), ("mov", "ecx, dword ptr [eax + 0xc]"),
                 ("mov", "ecx, dword ptr [ecx + 4]"), ("mov", "edx, dword ptr [ecx]")],
    0x005BBD4C: [("call", "dword ptr [edx + 0x38]")],
    0x005BC0C5: [("ret", "8")],
    # CheckForRecipientAndInit's gate: the unit's object's class (+8), its vptr +0x24
    # (GetSoldierType) 2 or 3.
    0x005BC82B: [("mov", "ecx, dword ptr [eax + 8]"), ("mov", "eax, dword ptr [ecx]"),
                 ("call", "dword ptr [eax + 0x24]"), ("cmp", "eax, 2"), ("je", "0x5bc83d"), ("cmp", "eax, 3")],
}

decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
pe = pefile.PE(str(Path(sys.argv[1]) / "BF2_modtools_NoDVD_NoConsole.exe"))
image = pe.get_memory_mapped_image()
base = pe.OPTIONAL_HEADER.ImageBase
assert base == 0x400000
for address, expected in checks.items():
    data = image[address - base:address - base + 96]
    actual = [(i.mnemonic, i.op_str) for i in decoder.disasm(data, address, count=len(expected))]
    assert actual == expected, (hex(address), actual, expected)
# AddGoal's jump table: Destroy (2) and Defend (3) to the cases above.
table = [int.from_bytes(image[0x005CD3F0 + 4 * k - base:0x005CD3F4 + 4 * k - base], "little") for k in range(7)]
assert table[1] == 0x005CD2C2 and table[2] == 0x005CD241, [hex(v) for v in table]
# The guard's bytes are the code's, and it takes another feature's JMP: the AI squads
# detour ControllerManager::Update too.
assert r'starts_with(base, a->controller_manager_update, "\x51\xA1", 2)' in source
assert r'starts_with(base, a->controller_manager_update + 6, "\x53\x55\x56\x8B\x70\x04", 6)' in source
assert "*(const uint8_t*)resolve(base, a->controller_manager_update) == 0xE9" in source
assert image[0x005997A0 - base:0x005997A2 - base] == b"\x51\xA1"
assert image[0x005997A6 - base:0x005997AC - base] == b"\x53\x55\x56\x8B\x70\x04"
assert "a->controller_manager_update" in squads and "DetourAttach(&(PVOID&)s_update" in squads
assert "const bool ok = code[0] == 0xE9 ||" in squads, "the AI squads' guard must take the comm keys' JMP"
# The stock request's and the aid check's guards are the code's bytes.
for va, text in ((0x0070C490, r"\x55\x8B\xEC\x81\xEC\x7C\x01\x00\x00"), (0x00687440, r"\x6A\x14\xB9"),
                 (0x005BBD20, r"\x51\x53\x55\x56\x8B\x74\x24\x14")):
    assert text in source, text
    want = bytes(int(b, 16) for b in text.split(r"\x")[1:])
    assert image[va - base:va - base + len(want)] == want, hex(va)
# The request pool's 5 s.
assert image[0x00AD74F4 - base:0x00AD74F8 - base] == b"\x00\x00\xa0\x40"
# The ending of an order: its members told to wait before its goal goes.
assert "if (goal && s_unassignAll) s_unassignAll(goal);" in source
print(f"modtools: {len(checks)} instruction checks, the goal targets, the guard and the constants passed")
