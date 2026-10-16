"""Read-only audit of the coroutine fix against all three game PEs.

Usage: python tests/lua_coroutines_abi_tests.py PATH_TO_GAMEDATA
Requires pefile and capstone. Reads lua_resume and LuaHelper::mState from
game_addrs.hpp and the install guard from the module, and checks them, and the
calls the fix rests on, against each executable. Never loads or executes the
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
module = (ROOT / "PatcherDLL/src/lua/lua_coroutines.cpp").read_text()
addresses = (ROOT / "PatcherDLL/src/core/game_addrs.hpp").read_text()
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

LITERALS = r'((?:"[^"]*"\s*)+)'
m = re.search(r'guard\(base, g_addr->lua_resume,\s*modtools \? ' + LITERALS + r':\s*' + LITERALS +
              r',\s*modtools \? ' + LITERALS + r':\s*' + LITERALS + r'\)', module, re.S)
assert m, "Update the audit if the install guard changes shape"


def joined(literals):
    text = "".join(re.findall(r'"([^"]*)"', literals))
    return codecs.decode(text, "unicode_escape").encode("latin1")


GUARD = {"modtools": (joined(m.group(1)), joined(m.group(3)).decode()),
         "retail": (joined(m.group(2)), joined(m.group(4)).decode())}

# The stub, as the module writes it: the global is the coroutine for the call
# and back after; it calls through s_resume.
stub = re.search(r"__declspec\(naked\) int resume_stub\(\)\s*\{\s*__asm \{(.*?)\}\s*\}", module, re.S)
assert stub, "Update the audit if the stub changes shape"
asm = [line.split("//")[0].strip() for line in stub.group(1).splitlines() if line.split("//")[0].strip()]
assert asm == ["push ecx", "push edx", "mov  ecx, [s_mState]", "push dword ptr [ecx]", "mov  edx, [esp + 16]",
               "mov  [ecx], edx", "push dword ptr [esp + 20]", "push edx", "call dword ptr [s_resume]",
               "add  esp, 8", "mov  ecx, [s_mState]", "pop  dword ptr [ecx]", "pop  edx", "pop  ecx", "ret"], asm

# Read off the disassembly before recording.
facts = {
    "modtools": dict(coresume=0x007BD860, cowrap=0x007BD9A0, auxwrap=0x007BD8E0, auxresume=0x007BD7E0),
    "steam": dict(coresume=0x006A0640, cowrap=0x006A0720, auxwrap=0x006A0AB0, auxresume=0x006A0850),
    "gog": dict(coresume=0x006A16D0, cowrap=0x006A17B0, auxwrap=0x006A1B40, auxresume=0x006A18E0),
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

        def calls(va, n):
            return [int(x.op_str, 16) for x in code(va, n) if x.mnemonic == "call" and x.op_str.startswith("0x")]

        def cstr(va):
            o = va - base
            if not 0 <= o < len(image):
                return None
            e = image.find(b"\0", o, o + 64)
            return image[o:e].decode("latin1") if e > o else None

        resume = table["lua_resume"]
        state = table["g_lua_state_ptr"]

        # The install guard.
        want, mask = GUARD["modtools" if mt else "retail"]
        got = at(resume, len(mask))
        assert all(k != "x" or a == b for a, b, k in zip(got, want, mask)), (build, got.hex())

        # The coroutine library: {create, wrap, resume, yield, status}.
        found = None
        for mm in re.finditer(b"\x00create\x00", image):
            sva = base + mm.start() + 1
            for r in re.finditer(re.escape(struct.pack("<I", sva)), image):
                o = r.start()
                names = [cstr(struct.unpack_from("<I", image, o + 8 * i)[0]) for i in range(5)]
                if names == ["create", "wrap", "resume", "yield", "status"]:
                    found = [struct.unpack_from("<I", image, o + 8 * i + 4)[0] for i in range(5)]
        assert found and found[2] == f["coresume"] and found[1] == f["cowrap"], (build, found)

        # coroutine.resume and the functions coroutine.wrap makes both go
        # through auxresume, and auxresume calls lua_resume.
        assert f["auxresume"] in calls(f["coresume"], 0x60), build
        wrap = code(f["cowrap"], 0x30)
        pushed = [int(x.op_str, 16) for x in wrap if x.mnemonic == "push" and x.op_str.startswith("0x")
                  and text_lo <= int(x.op_str, 16) < text_hi]
        assert pushed == [f["auxwrap"]], (build, [hex(p) for p in pushed])
        assert f["auxresume"] in calls(f["auxwrap"], 0x60), build
        assert resume in calls(f["auxresume"], 0x50), build

        # Nothing else calls lua_resume (directly or through a thunk).
        targets = {resume}
        seg = at(text_lo, text_hi - text_lo)
        for jm in re.finditer(b"\xe9", seg):
            o = jm.start()
            if o + 5 <= len(seg) and text_lo + o + 5 + struct.unpack_from("<i", seg, o + 1)[0] == resume:
                targets.add(text_lo + o)
        callers = []
        for cm in re.finditer(b"\xe8", seg):
            o = cm.start()
            if o + 5 <= len(seg) and text_lo + o + 5 + struct.unpack_from("<i", seg, o + 1)[0] in targets:
                callers.append(text_lo + o)
        assert len(callers) == 1 and f["auxresume"] < callers[0] < f["auxresume"] + 0x50, \
            (build, [hex(c) for c in callers])

        # lua_resume itself: both of its errors, plain cdecl returns.
        ins = code(resume, 0x70)
        refs = [cstr(int(x.op_str.split(", ")[-1], 16)) for x in ins
                if x.mnemonic in ("push", "mov") and re.search(r"0x[0-9a-f]{6,}$", x.op_str)]
        assert "cannot resume dead coroutine" in refs and "cannot resume non-suspended coroutine" in refs, \
            (build, refs)
        rets = [x for x in ins if x.mnemonic == "ret"]
        assert rets and all(x.op_str == "" for x in rets), (build, [x.op_str for x in rets])

        # The stock bindings read their arguments through LuaHelper::mState, not
        # the state they are called with: GetPathPoint does.
        name = image.find(b"\x00GetPathPoint\x00") + 1
        entry = image.find(struct.pack("<I", base + name))
        gpp = struct.unpack("<I", at(base + entry + 4, 4))[0]
        operand = "[0x%x]" % state
        assert any(operand in x.op_str for x in code(gpp, 0x40)), (build, "GetPathPoint reads mState")

        print(f"{build}: guard, the stub, the coroutine library, resume and wrap through auxresume, lua_resume's "
              f"one caller, its errors and returns, and mState in a stock binding passed")
    finally:
        pe.close()
