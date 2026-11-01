"""Read-only check of the GC save limit's addresses and install guards against the shipping exes.

Usage: python tests/gc_save_limit_abi_tests.py PATH_TO_GAMEDATA

Reads the addresses from game_addrs.hpp and the guard bytes from gc_save_limit.cpp,
so it tests what the DLL will actually do. For every build it checks:
  - the three hooked functions start with the bytes the installer expects;
  - each of the four buffer references holds the stock buffer address, and a sweep of
    .text finds no other reference to it or into it;
  - the six Lua API functions are __cdecl (plain RET) and the retail QueueFile is
    __stdcall with five arguments (RET 14h), the modtools one __thiscall;
  - on Steam/GOG every caller of the Lua item functions passes the buffer in EDX and
    uses only EAX after the call, which is what the naked thunks rely on.
If build/tests/gc_save_sample.gc exists (run tools/run_tests.py first) it is decoded
with Python's zlib, proving a miniz save reads with an independent zlib. Never runs
the game or writes to its files. Requires pefile and capstone.
"""
import re
import struct
import sys
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import gcread  # noqa: E402
import scan_sites  # noqa: E402

source = (ROOT / "PatcherDLL/src/shell/gc_save_limit.cpp").read_text(encoding="utf-8")
addresses = (ROOT / "PatcherDLL/src/core/game_addrs.hpp").read_text(encoding="utf-8")
BUILDS = {
    "modtools": "BattlefrontII.Debug.FullScreen.1080.exe",
    "steam": "BattlefrontII.exe",
    "gog": "BattlefrontII_GOG.exe",
}
FIELDS = ["save_lua_items", "load_lua_items", "save_fsm_queue_file", "metagame_buffer",
          "metagame_buffer_ref_load", "metagame_buffer_ref_save",
          "metagame_buffer_ref_save_state", "metagame_buffer_ref_load_state",
          "lua_type", "lua_next", "lua_pushvalue", "lua_rawset", "lua_checkstack",
          "lua_topointer", "lua_newtable", "g_lua_state_ptr"]
LUA_API = ["lua_type", "lua_next", "lua_pushvalue", "lua_rawset", "lua_checkstack",
           "lua_topointer"]


def guard(name):
    m = re.search(r"static const uint8_t %s\[\] = \{([^}]*)\};" % name, source)
    assert m, "guard %s not found in gc_save_limit.cpp" % name
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))


def table_for(build):
    body = re.search(r"namespace %s\s*\{(.*?)\n\s*\}\s*//\s*namespace %s" % (build, build),
                     addresses, re.S).group(1)
    table = {n: int(v, 16) for n, v in
             re.findall(r"constexpr uintptr_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;", body)}
    missing = [f for f in FIELDS if f not in table]
    assert not missing, (build, missing)
    return table


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    gamedata = Path(sys.argv[1])
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    for build, filename in BUILDS.items():
        a = table_for(build)
        pe = pefile.PE(str(gamedata / filename), fast_load=True)
        image = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        assert base == 0x400000
        mt = build == "modtools"
        rd = lambda va, n: image[va - base:va - base + n]

        if mt:   # the right modtools exe: the 2006 source tree, not a rebuilt one
            assert b"C:\\Battlefront2\\main\\Battlefront2\\Source" in image, filename

        for field, mtg, rtg in [("save_lua_items", "kSaveMt", "kSaveRt"),
                                ("load_lua_items", "kLoadMt", "kLoadRt"),
                                ("save_fsm_queue_file", "kQueueMt", "kQueueRt")]:
            g = guard(mtg if mt else rtg)
            assert rd(a[field], len(g)) == g, (build, field, rd(a[field], len(g)).hex())

        buf = a["metagame_buffer"]
        refs = [a[f] for f in FIELDS if f.startswith("metagame_buffer_ref")]
        for ref in refs:
            assert struct.unpack("<I", rd(ref, 4))[0] == buf, (build, hex(ref))
            assert rd(ref - 1, 1)[0] in (0x68, 0xBA), (build, hex(ref))   # PUSH imm32 / MOV EDX

        text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
        tva = base + text.VirtualAddress
        blob = image[text.VirtualAddress:text.VirtualAddress + text.Misc_VirtualSize]

        # decoded sweep: the buffer's start only at the four refs, its inside nowhere
        found, _ = scan_sites.sweep(str(gamedata / filename), range(buf, buf + 0x200))
        sites = [insn + 1 for insn, _, _ in found[buf]]
        assert sorted(sites) == sorted(refs), (build, [hex(s) for s in sites])
        inner = {hex(t): v for t, v in found.items() if t != buf and v}
        assert not inner, (build, inner)
        for s in pe.sections:   # and no data section keeps a pointer to it
            if s.Name.rstrip(b"\0") == b".text":
                continue
            data = image[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize]
            ptrs = [base + s.VirtualAddress + i for i in range(0, len(data) - 3, 4)
                    if buf <= struct.unpack_from("<I", data, i)[0] < buf + 0x200]
            assert not ptrs, (build, s.Name, [hex(p) for p in ptrs])

        def first_ret(va):
            for ins in md.disasm(rd(va, 0x400), va):
                if ins.mnemonic == "ret":
                    return ins.op_str
            raise AssertionError((build, hex(va), "no ret"))

        for f in LUA_API:
            assert first_ret(a[f]) == "", (build, f, "not __cdecl")
        # five stack arguments, callee cleaned, on every build
        queue_ret = first_ret(a["save_fsm_queue_file"])
        assert queue_ret == "0x14", (build, queue_ret)
        if mt:   # __thiscall: this in ECX on entry
            assert rd(a["save_fsm_queue_file"], 3) == b"\x56\x8b\xf1", build

        if not mt:
            for target in (a["save_lua_items"], a["load_lua_items"]):
                callers = []
                pos = blob.find(b"\xe8")
                while pos != -1:
                    if tva + pos + 5 + struct.unpack_from("<i", blob, pos + 1)[0] == target:
                        callers.append(tva + pos)
                    pos = blob.find(b"\xe8", pos + 1)
                assert len(callers) == 3, (build, hex(target), [hex(c) for c in callers])
                for call in callers:
                    before = list(md.disasm(rd(call - 5, 5), call - 5))
                    assert before and before[0].mnemonic == "mov" and before[0].op_str.startswith("edx,"), \
                        (build, hex(call), "buffer not in EDX")
                    for ins in md.disasm(rd(call + 5, 0x20), call + 5):
                        regs_read, _ = ins.regs_access()
                        names = {ins.reg_name(r) for r in regs_read}
                        assert not names & {"ecx", "edx", "cx", "dx", "cl", "dl"}, \
                            (build, hex(call), ins.mnemonic, ins.op_str)
                        if ins.mnemonic == "ret":
                            break
        print("%s: guards, 4 buffer refs, calling conventions and callers passed" % build)

    sample = ROOT / "build/tests/gc_save_sample.gc"
    if sample.exists():
        info = gcread.decode(sample.read_bytes())
        assert info["stored_crc"] == info["crc"] and info["payload"] == 512 and info["complete"]
        assert len(info["items"]) == 8
        assert info["items"][0][0] == {"cor": 1, "kas": 2, "tat": 2, "yav": 1}
        assert info["items"][1][0] == "TQN" and info["items"][2][0] == 100
        print("sample: a miniz save decodes with Python's zlib %s" % __import__("zlib").ZLIB_VERSION)
    else:
        print("sample: skipped, run tools/run_tests.py first")

    saves = gamedata / "SaveGames"
    if saves.is_dir():
        files = sorted(saves.glob("*.gc"))
        for f in files:
            info = gcread.decode(f.read_bytes())
            assert info["stored_crc"] == info["crc"] and info["complete"], f.name
        print("SaveGames: %d .gc files read cleanly" % len(files))


if __name__ == "__main__":
    main()
