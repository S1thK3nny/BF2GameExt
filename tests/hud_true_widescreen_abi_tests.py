"""Read-only audit of TrueWidescreen against all three game PEs.

Usage: python tests/hud_true_widescreen_abi_tests.py PATH_TO_GAMEDATA
Requires pefile and capstone. Reads the addresses from game_addrs.hpp and the
install guards, stand-ins and layout constants from the module, and checks
them, the addresses the loader moves inside the bytes the install compares or
writes, and the code the hooks and stand-ins rely on, against each executable.
Never loads or executes the game. Not an in-game behaviour test.
"""
import codecs
import re
import struct
import sys
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "PatcherDLL/src/render/hud_true_widescreen.cpp").read_text()
addresses = (ROOT / "PatcherDLL/src/core/game_addrs.hpp").read_text()

LITERALS = r'((?:"[^"]*"\s*)+)'


def joined(literals):
    text = "".join(re.findall(r'"([^"]*)"', literals))
    return codecs.decode(text, "unicode_escape").encode("latin1")


def body_of(name):
    match = re.search(r"\n(?:bool|void) " + name + r"\([^)]*\)\n\{(.*?)\n\}\n", source, re.S)
    assert match, name
    return match.group(1)


def flat(text):
    return re.sub(r"\s+", " ", text)


def const(name):
    match = re.search(r"constexpr \w+\s+" + name + r"\s*=\s*(0x[0-9A-Fa-f]+|\d+)", source)
    assert match, name
    return int(match.group(1), 0)


# ---- the module ---------------------------------------------------------------

# Layout, the same on every build (each checked against the code below).
layout = {name: const(name) for name in ("kRed", "kNode", "kRedParentList", "kRedChildList", "kMode", "kRedLocal",
                                         "kViewStride", "kViewGroups", "kNumCameras", "kTypeVector3", "kModeScreen",
                                         "kBarSegmentedBase")}
assert layout == {"kRed": 0xB0, "kNode": 0xB4, "kRedParentList": 0x1C, "kRedChildList": 0x70, "kMode": 0x158,
                  "kRedLocal": 0x30, "kViewStride": 0xA0, "kViewGroups": 5, "kNumCameras": 0x1C,
                  "kTypeVector3": 9, "kModeScreen": 1, "kBarSegmentedBase": 0x200}, layout

# The guards: modtools compares exactly; retail through a mask, with each
# moved operand checked against the address it names.
modtools_body = body_of("modtools_code_matches")
retail_body = body_of("retail_code_matches")
modtools_guards = re.findall(r'\bcode_is\(base, a\.(\w+), "([^"]*)",\s*' + LITERALS + r',\s*(\d+)\)', modtools_body)
assert len(modtools_guards) == 14, len(modtools_guards)
MOVED = r'(?:,\s*\{ (0x[0-9A-Fa-f]+), a\.(\w+)(?: \+ (0x[0-9A-Fa-f]+))? \})?'
retail_guards = re.findall(r'retail_code_is\(base, a\.(\w+), "([^"]*)",\s*' + LITERALS + r',\s*' + LITERALS +
                           MOVED + MOVED + r'\)', retail_body)
assert len(retail_guards) == 7, len(retail_guards)
draws = {kind: re.search(r'draw_is\(base, a\.hud_element_draw,\s*' + LITERALS + r',\s*' + LITERALS + r'\)', body)
         for kind, body in (("modtools", modtools_body), ("retail", retail_body))}
assert all(draws.values())
assert "constexpr size_t kDrawStolen = 6;" in source
slots = re.findall(r'vtable_slot\(base, a\.(\w+), (0x[0-9A-Fa-f]+), a\.(\w+), "[^"]*"\)', source)
assert sorted(slots) == [("hud_file_info_vtable", "0x20", "hud_file_info_read_data"),
                         ("hud_file_info_vtable", "0x28", "hud_file_info_write_data"),
                         ("hud_target_vtable", "0x2C", "hud_target_update")], slots

# The stand-ins: which register each reads, and which sites get which.
stand_ins = {
    "read_top_item": ["push edi", "mov ecx, edi", "jmp dword ptr [edx + 8]"],
    "read_top_item_retail": ["push esi", "mov ecx, esi", "jmp dword ptr [eax + 8]"],
    "aspect_esi": ["pushfd", "pushad", "push esi", "call aspect_for", "popad", "popfd"],
    "aspect_ebx": ["pushfd", "pushad", "push ebx", "call aspect_for", "popad", "popfd"],
    "aspect_bar_segmented": ["pushfd", "pushad", "lea eax, [ebx - 0x200]", "call aspect_for", "popad", "popfd"],
    "aspect_esi_retail": ["pushfd", "pushad", "push esi", "call aspect_for", "popad", "popfd"],
    "aspect_bar_segmented_retail": ["pushfd", "pushad", "lea eax, [edi - 0x200]", "call aspect_for", "popad",
                                    "popfd"],
    "hooked_ViewWidthRetail": ["pushad", "push ecx", "call retail_view_width", "test al, al",
                               "movss xmm0, dword ptr [esp]", "jmp dword ptr [s_viewWidthRetail]"],
    "hooked_ToPixelsRetail": ["cmp ecx, 1", "jne stock", "push eax", "pushad", "push dword ptr [esp + 0xA8]",
                              "movss dword ptr [esp], xmm3", "push ecx", "call retail_screen_for",
                              "fstp dword ptr [esp + 0xA0]", "call dword ptr [s_toPixelsRetail]", "add esp, 4",
                              "jmp dword ptr [s_toPixelsRetail]"],
}
for name, lines in stand_ins.items():
    match = re.search(r"__declspec\(naked\) void " + name + r"\(\)\n\{\n   __asm \{(.*?)\n   \}\n\}", source, re.S)
    assert match, name
    asm = flat(re.sub(r"//[^\n]*", "", match.group(1)))
    at = 0
    for line in lines:
        found = asm.find(line, at)
        assert found >= 0, (name, line)
        at = found + len(line)
for name in ("aspect_esi_retail", "aspect_bar_segmented_retail", "hooked_ViewWidthRetail", "hooked_ToPixelsRetail"):
    asm = re.search(r"void " + name + r"\(\)\n\{(.*?)\n\}", source, re.S).group(1)
    for reg in range(8):   # every XMM register kept
        assert re.search(r"movups \[esp \+ 0x[0-9A-F]+\], xmm%d" % reg, asm), (name, reg)
install = flat(body_of("hud_true_widescreen_install"))
for site, modtools_to, retail_to in (("hud_bitmap_rect_aspect_call", "aspect_esi", "aspect_esi_retail"),
                                     ("hud_map_aspect_call_1", "aspect_ebx", "aspect_esi_retail"),
                                     ("hud_map_aspect_call_2", "aspect_ebx", "aspect_esi_retail"),
                                     ("hud_map_aspect_call_3", "aspect_ebx", "aspect_esi_retail"),
                                     ("hud_map_aspect_call_4", "aspect_ebx", "aspect_esi_retail"),
                                     ("hud_bar_segmented_aspect_call", "aspect_bar_segmented",
                                      "aspect_bar_segmented_retail")):
    assert install.count("retarget(base, a.%s, " % site) == 2, site
    assert "retarget(base, a.%s, %s);" % (site, modtools_to) in install, site
    assert "retarget(base, a.%s, %s);" % (site, retail_to) in install, site
assert "modtools ? read_top_item : read_top_item_retail" in install
for hook in ("s_loadRetail, hooked_LoadRetail", "&s_viewWidthRetail, hooked_ViewWidthRetail",
             "&s_toPixelsRetail, hooked_ToPixelsRetail", "s_readData, hooked_ReadData",
             "s_position, hooked_Position", "s_draw, hooked_Draw"):
    assert hook in install, hook
assert "using LoadRetailFn = void(__fastcall*)(void* config);" in source
assert "void __fastcall hooked_LoadRetail(void* config)" in source

# Where each aspect call's element lives, read off the disassembly: the
# function puts it (or the ElementBar base 0x200 above it) in a register once,
# from ECX at entry, and never writes that register again before the call.
aspect_functions = {
    "modtools": {"bitmap": (0x00698790, "esi"), "map": (0x0069B920, "ebx"), "bar": (0x00696AB0, "ebx")},
    "steam": {"bitmap": (0x0054D590, "esi"), "map": (0x005520B0, "esi"), "bar": (0x0054C130, "edi")},
    "gog": {"bitmap": (0x0054E2E0, "esi"), "map": (0x00552E10, "esi"), "bar": (0x0054CE80, "edi")},
}
aspect_sites = {"bitmap": ["hud_bitmap_rect_aspect_call"],
                "map": ["hud_map_aspect_call_%d" % i for i in range(1, 5)],
                "bar": ["hud_bar_segmented_aspect_call"]}

# The masked absolute addresses no Moved checks: the draw's security cookie
# and GetScreenAspectRatio's value. Every other moved operand in compared
# bytes must be a Moved one.
unchecked = {"draw": [0x1A], "GetScreenAspectRatio": [0x02]}


def relocations(pe):
    """The addresses the loader moves when it loads the exe elsewhere (HIGHLOW entries)."""
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])
    base = pe.OPTIONAL_HEADER.ImageBase
    return {base + e.rva for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", [])
            for e in block.entries if e.type == pefile.RELOCATION_TYPE["IMAGE_REL_BASED_HIGHLOW"]}


def b(text):
    return bytes.fromhex(text.replace(" ", ""))


FULL = {"al": "eax", "ah": "eax", "ax": "eax", "cl": "ecx", "ch": "ecx", "cx": "ecx", "dl": "edx", "dh": "edx",
        "dx": "edx", "bl": "ebx", "bh": "ebx", "bx": "ebx", "si": "esi", "di": "edi", "bp": "ebp", "sp": "esp"}


def written(ins):
    """The full registers an instruction writes."""
    _, regs = ins.regs_access()
    return {FULL.get(ins.reg_name(r), ins.reg_name(r)) for r in regs}


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
    mt = build == "modtools"
    pe = pefile.PE(str(Path(sys.argv[1]) / filename), fast_load=True)
    try:
        image = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        assert base == 0x400000
        text_section = [s for s in pe.sections if s.Name.rstrip(b"\0") == b".text"][0]
        text_lo = base + text_section.VirtualAddress
        text = image[text_section.VirtualAddress:text_section.VirtualAddress + text_section.Misc_VirtualSize]

        def at(va, n):
            return image[va - base:va - base + n]

        def u32(va):
            return struct.unpack("<I", at(va, 4))[0]

        def target(va):
            """Where a CALL/JMP rel32 at va goes, through modtools' incremental-link JMPs."""
            assert at(va, 1) in (b"\xE8", b"\xE9"), (build, hex(va), at(va, 5).hex())
            dest = va + 5 + struct.unpack("<i", at(va + 1, 4))[0]
            while at(dest, 1) == b"\xE9":
                dest = dest + 5 + struct.unpack("<i", at(dest + 1, 4))[0]
            return dest

        def code(va, n):
            return list(decoder.disasm(at(va, n), va))

        def until_ret(va, limit=0x2000):
            out = []
            for ins in decoder.disasm(at(va, limit), va):
                out.append(ins)
                if ins.mnemonic == "ret":
                    break
            return out

        def find_all(needle):
            out, start = [], 0
            while True:
                i = text.find(needle, start)
                if i < 0:
                    return out
                out.append(text_lo + i)
                start = i + 1

        def calls_to(va):
            out = []
            for i in range(len(text) - 5):
                if text[i] == 0xE8 and text_lo + i + 5 + struct.unpack("<i", text[i + 1:i + 5])[0] in thunks_of(va):
                    out.append(text_lo + i)
            return out

        thunk_cache = {}

        def thunks_of(va):
            """va and every modtools incremental-link JMP to it."""
            if va not in thunk_cache:
                found = {va}
                if mt:
                    for i in range(0x1000, 0x20000):
                        if image[i] == 0xE9 and base + i + 5 + struct.unpack("<i", image[i + 1:i + 5])[0] == va:
                            found.add(base + i)
                thunk_cache[va] = found
            return thunk_cache[va]

        moved = relocations(pe)
        if mt:
            assert pe.FILE_HEADER.Characteristics & 0x0001 and not moved, (build, "modtools has a fixed base")
        else:
            assert pe.OPTIONAL_HEADER.DllCharacteristics & 0x0040, (build, "retail is built to be relocated")

        def relocs_in(lo, hi):
            return sorted(va for va in moved if lo - 3 <= va < hi)

        # ---- what the install compares, and what the loader moves inside it --------
        if mt:
            for name, what, literals, length in modtools_guards:
                want = joined(literals)
                assert len(want) == int(length), (build, what)
                assert at(table[name], len(want)) == want, (build, what, at(table[name], len(want)).hex())
            want = joined(draws["modtools"].group(1))
            mask = joined(draws["modtools"].group(2)).decode()
            assert mask == "x" * len(want) and at(table["hud_element_draw"], len(want)) == want, (build, "draw")
        else:
            for name, what, literals, mask_literals, *moves in retail_guards:
                want = joined(literals)
                mask = joined(mask_literals).decode()
                va = table[name]
                assert len(want) == len(mask), (build, what)
                have = at(va, len(mask))
                assert all(m != "x" or h == w for h, w, m in zip(have, want, mask)), (build, what, have.hex())
                listed = []
                for off, named, plus in (moves[0:3], moves[3:6]):
                    if not off:
                        continue
                    off = int(off, 16)
                    names = table[named] + (int(plus, 16) if plus else 0)
                    assert u32(va + off) == names, (build, what, hex(off), hex(u32(va + off)), hex(names))
                    assert mask[off:off + 4] == "????", (build, what, "a moved operand is masked", hex(off))
                    listed.append(off)
                key = "GetScreenAspectRatio" if name == "renderer_screen_aspect" else what
                inside = [va_ - va for va_ in relocs_in(va, va + len(mask))]
                assert inside == sorted(listed + unchecked.get(key, [])), (build, what, [hex(x) for x in inside])
                for off in inside:
                    assert mask[off:off + 4] == "????", (build, what, hex(off))
            want = joined(draws["retail"].group(1))
            mask = joined(draws["retail"].group(2)).decode()
            draw = table["hud_element_draw"]
            have = at(draw, len(mask))
            assert all(m != "x" or h == w for h, w, m in zip(have, want, mask)), (build, "draw", have.hex())
            inside = [va_ - draw for va_ in relocs_in(draw, draw + len(mask))]
            assert inside == unchecked["draw"] and mask[0x1A:0x1E] == "????", (build, "draw", inside)
            # The first three instructions, which another detour would replace, are six bytes.
            assert [x.size for x in code(draw, 6)][:3] == [1, 2, 3], build

        # The bytes the install writes: five-byte CALLs, with nothing the loader moves.
        write_sites = [table[name] for names in aspect_sites.values() for name in names]
        write_sites.append(table["hud_manager_load_read_call"])
        for va in write_sites:
            assert relocs_in(va, va + 5) == [], (build, hex(va))

        # ---- FileInfo (HUD::Manager::ConfigFile) and ElementTarget ----------------------
        for vtable, slot, function in slots:
            if function == "hud_file_info_write_data" and not mt:
                continue
            entry = u32(table[vtable] + int(slot, 16))
            if mt:
                assert at(entry, 1) == b"\xE9", (build, function)
                entry = target(entry)
            assert entry == table[function], (build, function, hex(entry))
        if not mt:
            # Retail keeps RTTI: the vtables' complete object locators name the classes.
            for vtable, name in (("hud_file_info_vtable", b".?AVConfigFile@Manager@HUD@@"),
                                 ("hud_target_vtable", b".?AVElementTarget@HUD@@")):
                col = u32(table[vtable] - 4)
                assert u32(col) == 0 and u32(col + 4) == 0, (build, vtable, "the primary vtable")
                descriptor = u32(col + 12)
                assert at(descriptor + 8, len(name) + 1) == name + b"\0", (build, vtable)
        read_data = until_ret(table["hud_file_info_read_data"])
        assert read_data[-1].bytes == b("C2 08 00"), (build, "ReadData returns with RET 8")

        # ---- HUD::Manager::Load ----------------------------------------------------------
        load = table["hud_manager_load"]
        callers = calls_to(load)
        assert len(callers) == 1, (build, [hex(c) for c in callers])
        rets = [x for x in code(load, 0x400) if x.mnemonic == "ret"]
        if mt:
            # cdecl(PblConfig*): read off the stack; the caller pushes it and pops it.
            assert at(load, 4) == b("8B 44 24 04"), build
            assert at(callers[0] - 1, 1) == b("50") and at(callers[0] + 5, 3) == b("83 C4 04"), build
            assert at(load + 0x18, 6) == b("8B 0D") + struct.pack("<I", table["camera_manager_instance"]), build
            assert at(load + 0x1E, 3) == b("8B 69 1C"), (build, "CameraManager camera count")
        else:
            # The PblConfig in ECX, handed to PblConfig's constructor; plain RETs;
            # the one caller loads ECX just before the CALL.
            assert at(load + 0x0C, 4) == b("51 8D 4D D0"), build
            assert at(callers[0] - 4, 4) == b("8D 4C 24 0C"), (build, at(callers[0] - 4, 4).hex())
            assert rets and all(x.bytes == b("C3") for x in rets[:2]), (build, [x.op_str for x in rets])
        # The read of each top-level item: its vtable loaded from the item register
        # just before; the hash compare after is FileInfo's (0xD4E0C797).
        read_call = table["hud_manager_load_read_call"]
        assert load < read_call < load + 0x400, build
        if mt:
            assert at(read_call - 9, 2) == b("8B 17"), (build, "MOV EDX,[EDI]")
        else:
            assert at(read_call - 10, 2) == b("8B 06"), (build, "MOV EAX,[ESI]")
        assert at(read_call + 7, 4) == struct.pack("<I", 0xD4E0C797), build
        # The view groups: the camera loop steps 0xA0 through gHudViewPorts and
        # asks each group for its parent: GetParent, [this+0x1C] - 0x70.
        get_parent = target(load + (0x49 if mt else 0x49))
        assert at(get_parent, 11) == b("8B 41 1C 85 C0 74 04 83 C0 90 C3"), (build, hex(get_parent))
        if mt:
            assert at(load + 0x2B, 5) == b("BF A0 00 00 00") or b("A0 00 00 00") in at(load, 0x60), build
        else:
            assert at(load + 0x30, 5) == b("BE A0 00 00 00"), build

        # gHudViewPorts: five RedScreenGroupElements of 0xA0 bytes.
        cell = table["hud_view_groups"]
        stores = [va for va in find_all(b"\x89") if at(va + 2, 4) == struct.pack("<I", cell)
                  and at(va + 1, 1)[0] in (0x05, 0x0D, 0x15, 0x1D, 0x2D, 0x35, 0x3D)]
        stores += [va for va in find_all(b"\xA3" + struct.pack("<I", cell))]
        assert stores, build
        if mt:
            assert at(stores[-1] - 2, 2) == b("6A 05"), (build, "five groups")
            assert b("81 C6 A0 00 00 00 81 FE 20 03 00 00") in text, (build, "0xA0 apart")
        else:
            near = at(stores[0] - 0x30, 0x30)
            assert b("6A 05") in near and b("68 A0 00 00 00") in near, (build, "five groups 0xA0 apart")

        # ---- GetContainerViewWidth -------------------------------------------------------
        view_width = table["hud_container_view_width"]
        if mt:
            # thiscall -> ST0; keeps ECX (PUSH/POP) and EDX.
            ins = until_ret(view_width)
            assert ins[0].bytes == b("51") and ins[-2].bytes == b("59"), build
            assert at(view_width + 10, 6) == b("D9 05") + struct.pack("<I", table["hud_viewport_width"]), build
        else:
            # ECX -> XMM0. Changes only EAX, ECX and XMM0 (and the flags), itself
            # and through the GetViewport it calls.
            changed = set()
            for start in (view_width, target(view_width)):
                for ins in code(start, 0x30 if start == view_width else 0x18):
                    changed |= written(ins)
                    if start != view_width and ins.mnemonic == "ret":
                        break
            assert changed <= {"eax", "ecx", "xmm0", "eflags", "esp", "eip"}, (build, changed)
            assert at(target(view_width), 6) == b("8B 81 F4 00 00 00"), (build, "GetViewport walks +0xF4")

        # ---- the relative-to-pixels conversion -----------------------------------------
        to_pixels = table["hud_relative_to_pixels"]
        if mt:
            # cdecl(mode, value, frame, view, screen) -> ST0: Screen (1) multiplies by
            # the fifth argument, Viewport (2) by the fourth, 3 by the third.
            jump_table = u32(to_pixels + 12)
            entries = [u32(jump_table + 4 * i) for i in range(4)]
            want = ["D9 44 24 08 C3", "D9 44 24 08 D8 4C 24 14", "D9 44 24 08 D8 4C 24 10", "D9 44 24 08 D8 4C 24 0C"]
        else:
            # Mode in ECX; value, frame and view in XMM1-3; the screen at [EBP+8];
            # XMM0 back. Screen (1) multiplies by the stack argument.
            jump_table = u32(to_pixels + 11)
            assert jump_table == to_pixels + 0x2C, build
            entries = [u32(jump_table + 4 * i) for i in range(4)]
            want = ["0F 28 C1 5D C3", "F3 0F 59 4D 08", "F3 0F 59 CB", "F3 0F 59 CA"]
            changed = set()
            for ins in code(to_pixels, 0x2C):
                changed |= written(ins)
            assert changed <= {"xmm0", "xmm1", "ebp", "esp", "eflags", "eip"}, (build, changed)
            assert all(x.bytes in (b("5D"), b("C3")) or x.mnemonic != "ret" for x in code(to_pixels, 0x2C))
            assert [x.bytes for x in code(to_pixels, 0x2C) if x.mnemonic == "ret"] == [b("C3")] * 3, build
        for mode, (entry, text_) in enumerate(zip(entries, want)):
            assert to_pixels < entry < to_pixels + 0x40, (build, mode, hex(entry))
            assert at(entry, len(b(text_))) == b(text_), (build, "mode", mode, at(entry, 8).hex())

        # ---- EventPosition ---------------------------------------------------------------
        position = table["hud_event_position"]
        ins = until_ret(position)
        assert ins[-1].bytes == b("C3"), (build, "cdecl, plain RET")
        disps = {op.mem.disp for x in ins for op in x.operands if op.type == capstone.x86.X86_OP_MEM}
        assert layout["kRed"] in disps and layout["kMode"] in disps, (build, "element +0xB0, +0x158")
        targets = [target(x.address) for x in ins if x.bytes[0] == 0xE8]
        assert targets.count(view_width) == 1 and targets.count(to_pixels) == 2, (build, [hex(t) for t in targets])
        assert b("83 F8 09") in at(position, 0x40), (build, "the Vector3 type, 9")
        # Registered by ElementGroupBase's constructor: one PUSH of its address
        # (modtools: of its incremental-link JMP).
        pushes = [va for th in thunks_of(position) for va in find_all(b"\x68" + struct.pack("<I", th))]
        assert len(pushes) == 1, (build, [hex(p) for p in pushes])
        if not mt:
            # Its Event: EventClass at +0, payload at +4; the class's type at +4.
            assert at(target(position + 0x0B), 3) == b("8B 01 C3"), build
            assert at(target(position + 0x12), 4) == b("8B 41 04 C3"), build
            assert at(target(position + 0x27), 15) == b("55 8B EC 8B 45 08 8B 49 04 89 08 5D C2 04 00"), build
            # EventClass::Find compares the name's hash at the class's +0 (list node at +0x10).
            assert b("39 50 F0") in at(table["hud_event_class_find"], 0x20), build
            # The ElementGroupBase constructor's RelativeMode default, at +0x158.
            assert b("66 C7 81 58 01 00 00 02 00") in at(pushes[0], 0x10), build
        # SetPosition writes the local matrix's translation row (+0x30 + 0x30):
        # x, y and z from +0x60, then 1.0 at +0x6C.
        set_position = []
        for t in targets:
            row = [x for x in code(t, 0x24) if any(op.type == capstone.x86.X86_OP_MEM and op.mem.disp ==
                                                   layout["kRedLocal"] + 0x30 for op in x.operands)]
            if row and b("C7 41 6C 00 00 80 3F") in at(t, 0x24):
                set_position.append(t)
        assert len(set_position) == 1, (build, "SetPosition")

        # ---- the element draw ------------------------------------------------------------
        draw = table["hud_element_draw"]
        draw_code = until_ret(draw, 0x400)
        assert draw_code[-1].bytes == b("C2 08 00"), (build, "thiscall, RET 8")
        assert len(calls_to(draw)) == 3, build

        # ---- HUD::Element::sList, and an element's red and node -------------------------
        terminator = table["hud_element_list"]
        stores = [va for va in find_all(b"\xC7") if at(va + 2, 4) == struct.pack("<I", terminator)
                  and at(va + 1, 1)[0] in (0x06, 0x07)]
        assert len(stores) == 1, (build, [hex(s) for s in stores])
        node_store = stores[0]
        before = at(node_store - 0x180, 0x180)
        if mt:
            assert b("89 8E B0 00 00 00") in before and b("8D BE B4 00 00 00") in before, build
            assert at(node_store + 6, 6) == b("8B 15") + struct.pack("<I", terminator + 4), build
        else:
            assert b("89 86 B0 00 00 00 81 C6 B4 00 00 00") in before, build
            assert at(node_store + 6, 5) == b("A1") + struct.pack("<I", terminator + 4), build
            assert at(node_store + 0x19, 6) == b("89 35") + struct.pack("<I", terminator + 4), (build, "appended")

        # ---- RedGroupElement::AddChild: the child's link at +0x1C, { list, next,
        # prev, item }, the list at the group's +0x70 ------------------------------------
        if mt:
            add_child = 0x00838AB0
            assert at(add_child + 0x0A, 3) == b("8D 53 1C"), build        # LEA EDX,[EBX+0x1C]
            assert at(add_child + 0x45, 3) == b("8D 4D 70"), build        # LEA ECX,[EBP+0x70]
            assert at(add_child + 0x4F, 3) == b("89 5A 0C"), build        # MOV [EDX+0xC],EBX
        else:
            col_name = b".?AVRedGroupElement@@\0"
            vtables = []
            descriptor = image.find(col_name) + base - 8
            cols = [base + m.start() - 12 for m in re.finditer(re.escape(struct.pack("<I", descriptor)), image)]
            for col in cols:
                if u32(col) == 0:
                    vtables += [base + m.start() + 4 for m in re.finditer(re.escape(struct.pack("<I", col)), image)]
            assert len(vtables) == 1, (build, vtables)
            add_child = u32(vtables[0] + 0x44)
            body_ = at(add_child, 0x68)
            assert b("8D 77 1C") in body_ and b("83 C1 70") in body_ and b("89 7E 0C") in body_ \
                and b("89 0E") in body_, build
            # Groups and screen groups draw their children through one function
            # (+0x40), which calls the draw for each.
            group_draw = u32(vtables[0] + 0x40)
            assert sorted(c for c in calls_to(draw) if group_draw <= c < group_draw + 0xB0) == \
                sorted(calls_to(draw))[1:], build

        # ---- the interface camera's frustum width t ------------------------------------
        t = table["hud_interface_frustum"]
        if mt:
            inits = find_all(b"\xD9\x1D" + struct.pack("<I", t))
        else:
            inits = find_all(b"\xF3\x0F\x11\x05" + struct.pack("<I", t))
        assert len(inits) == 1, (build, "one static initialiser")
        # RedInterfaceScreen::Render hands it to the interface camera's
        # SetFrustum(near 1, far 10000, width t, height) and divides the
        # screen's width by it for the HUD plane's distance.
        render = at(sorted(calls_to(draw))[0] - 0x600, 0x600)
        operand = struct.pack("<I", t)
        if mt:
            load = render.find(b("8B 15") + operand)                       # MOV EDX,[t]
            assert load >= 0 and b("52 68 00 40 1C 46 68 00 00 80 3F E8") in render[load:load + 0x20], build
            assert b("D8 35") + operand in render, (build, "FDIV [t]")
        else:
            load = render.find(b("F3 0F 10 1D") + operand)                 # MOVSS XMM3,[t]
            assert load >= 0 and b("F3 0F 11 5C 24 08 C7 44 24 04 00 40 1C 46") in render[load:load + 0x80], build
            assert b("F3 0F 5E 05") + operand in render, (build, "DIVSS XMM0,[t]")

        # ---- GetScreenAspectRatio and its six calls ------------------------------------
        aspect = table["renderer_screen_aspect"]
        assert at(aspect, 2) == b("D9 05") and at(aspect + 6, 1) == b("C3"), build
        all_sites = [table[name] for names in aspect_sites.values() for name in names]
        assert len(set(all_sites)) == 6, (build, "six different calls")
        for kind in aspect_sites:   # every aspect call these functions make, and no other
            start, _ = aspect_functions[build][kind]
            calls = [x.address for x in code(start, 0x300) if x.bytes[0] == 0xE8 and target(x.address) == aspect]
            wanted = sorted(table[name] for name in aspect_sites[kind])
            assert sorted(calls) == wanted, (build, kind, [hex(c) for c in calls])
        for kind, names in aspect_sites.items():
            start, reg = aspect_functions[build][kind]
            for name in names:
                site = table[name]
                assert start < site < start + 0x300, (build, name)
                assert target(site) == aspect, (build, name)
                writes = []
                for x in code(start, site - start):
                    no_op = x.op_str in (f"{reg}, [{reg}]", f"{reg}, {reg}")   # alignment filler
                    if reg in written(x) and x.mnemonic != "pop" and not no_op:
                        writes.append(x)
                assert [(x.mnemonic, x.op_str) for x in writes] == [("mov", reg + ", ecx")], \
                    (build, name, [(hex(x.address), x.mnemonic, x.op_str) for x in writes])
        bar, _ = aspect_functions[build]["bar"]
        if mt:
            assert b("8D B3 00 FE FF FF") in at(bar, 0x30), (build, "the element 0x200 below the ElementBar")
        else:
            # The bar's SetValue is slot 1 of ElementBarSegmented's vtable for its
            # ElementBar base, the one whose locator says 0x200.
            descriptor = image.find(b".?AVElementBarSegmented@HUD@@\0") + base - 8
            cols = [base + m.start() - 12 for m in re.finditer(re.escape(struct.pack("<I", descriptor)), image)]
            col = [c for c in cols if u32(c) == 0 and u32(c + 4) == layout["kBarSegmentedBase"]]
            assert len(col) == 1, build
            vtable = [base + m.start() + 4 for m in re.finditer(re.escape(struct.pack("<I", col[0])), image)]
            assert len(vtable) == 1 and u32(vtable[0] + 4) == bar, (build, hex(u32(vtable[0] + 4)))

        # ---- sViewportWidth ---------------------------------------------------------------
        width = table["hud_viewport_width"]
        reads = find_all(struct.pack("<I", width))
        assert len(reads) >= 7, (build, "ElementMap's constructor and the rest read it")

        print(f"{build}: {len(modtools_guards) if mt else len(retail_guards) + 1} guards, "
              f"{'no moved addresses' if mt else 'every moved address in them'}, the written CALLs, "
              f"FileInfo/ElementTarget slots, Load, its read call, GetContainerViewWidth, the conversion's "
              f"modes, EventPosition, the draw, sList, AddChild, the frustum, six aspect calls and "
              f"their registers passed")
    finally:
        pe.close()
