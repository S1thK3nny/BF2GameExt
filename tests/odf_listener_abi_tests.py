"""Read-only audit of the ODF property listeners' ClassParent hooks against all
three game PEs.

Usage: python tests/odf_listener_abi_tests.py PATH_TO_GAMEDATA
Requires pefile and capstone. entity/odf_gameext_props.cpp feeds the property
listeners (odf_add_property_handler, odf_add_derive_handler) from the readers'
PROP sites and reports each class made from its base from the two Derive calls
in EntityClass::Read and WeaponClass::Read. This checks the Derive sites in
game_addrs.hpp against each executable: the eight bytes the patcher expects
(and that the source expects them), the vcall they end in, that no branch in
the reader lands inside them, that nothing in them is relocated, and that the
next instruction takes the child from EAX, which the shims leave as they found
it. Never loads or executes the game. Not an in-game behaviour test.
"""
import re
import sys
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "PatcherDLL" / "src"
odf = (SRC / "entity" / "odf_gameext_props.cpp").read_text()
addresses = (SRC / "core" / "game_addrs.hpp").read_text()

# The Derive sites and the bytes the patcher expects, by reader and build kind,
# with the register holding the parent across the call (the shims report it).
derive_bytes = {
    ("modtools", "entity"): ("8B 03 52 8B CB FF 50 04", "ebx"),
    ("modtools", "weapon"): ("8B 16 57 8B CE FF 52 04", "esi"),
    ("release", "entity"): ("8B 07 8B CF 52 FF 50 04", "edi"),
    ("release", "weapon"): ("8B 07 8B CF 56 FF 50 04", "edi"),
}
for (kind, reader), (text, parent) in derive_bytes.items():
    literal = ", ".join("0x" + b for b in text.split())
    assert literal in odf, (kind, reader, "derive bytes changed in odf_gameext_props.cpp")

# Each shim reports (parent, child) from the register the reader keeps the
# parent in, and keeps every register with pushad/popad.
shims = {
    ("modtools", "entity"): "shim_mt_entity_derive",
    ("modtools", "weapon"): "shim_mt_weapon_derive",
    ("release", "entity"): "shim_rt_entity_derive",
    ("release", "weapon"): "shim_rt_weapon_derive",
}
for key, name in shims.items():
    body = re.search(r"void " + name + r"\(\)\s*\{\s*__asm \{(.*?)\}\s*\}", odf, re.S).group(1)
    lines = [ln.split("//")[0].strip() for ln in body.splitlines() if ln.split("//")[0].strip()]
    parent = derive_bytes[key][1]
    k = lines.index("pushad")
    assert lines[k + 1:k + 6] == ["push eax", "push " + parent, "call odf_class_derived", "add  esp, 8", "popad"], \
        (key, lines)
    assert lines[-1].startswith("jmp  [s_cont"), (key, lines[-1])

reader_starts = {
    "modtools": {"entity": 0x004D0830, "weapon": 0x0061E3F0},
    "steam": {"entity": 0x00491CC0, "weapon": 0x0067A240},
    "gog": {"entity": 0x00491CC0, "weapon": 0x0067B2E0},
}

builds = [
    ("modtools", "BF2_modtools_NoDVD_NoConsole.exe"),
    ("steam", "BattlefrontII.exe"),
    ("gog", "BattlefrontII_GoG.exe"),
]
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
decoder.detail = True


def table(build):
    body = re.search(r"namespace " + build + r"\s*\{(.*?)\n\s*\}\s*//\s*namespace " + build,
                     addresses, re.S).group(1)
    return {name: int(value, 16) for name, value in
            re.findall(r"constexpr uintptr_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;", body)}


def relocations(pe):
    """The addresses the loader moves when it loads the exe elsewhere (HIGHLOW entries)."""
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
    pe = pefile.PE(str(Path(sys.argv[1]) / filename), fast_load=True)
    try:
        image = pe.get_memory_mapped_image()
        base = pe.OPTIONAL_HEADER.ImageBase
        assert base == 0x400000
        relocs = relocations(pe)
        for reader in ("entity", "weapon"):
            va = addrs[reader + "_class_read_derive_site"]
            text, parent = derive_bytes[(kind, reader)]
            assert image[va - base:va - base + 8] == bytes.fromhex(text), (build, reader, "derive bytes")
            ins = list(decoder.disasm(image[va - base:va - base + 24], va))
            assert [x.address for x in ins[:4]] == [va, va + 2, va + 3, va + 5] or \
                [x.address for x in ins[:4]] == [va, va + 2, va + 4, va + 5], (build, reader)
            call = ins[3]
            assert call.mnemonic == "call" and call.op_str.endswith("+ 4]"), (build, reader, call.op_str)
            assert any(x.mnemonic == "mov" and x.op_str == "ecx, " + parent for x in ins[:3]), (build, reader)
            # The reader takes the child from EAX within the next three
            # instructions, before anything writes EAX.
            after = [x for x in ins[4:7]]
            assert after[0].address == va + 8, (build, reader)
            took = next((i for i, x in enumerate(after) if x.mnemonic == "mov" and x.op_str.endswith(", eax")), None)
            assert took is not None, (build, reader, [(x.mnemonic, x.op_str) for x in after])
            for x in after[:took]:
                _, written = x.regs_access()
                assert "eax" not in {x.reg_name(r) for r in written}, (build, reader, x.mnemonic, x.op_str)
            assert not any(va - 3 <= r < va + 8 for r in relocs), (build, reader, "relocated bytes")
            inside = {t for t in branch_targets(image, base, reader_starts[build][reader], va + 0x100)
                      if va < t < va + 8}
            assert not inside, (build, reader, "a branch lands inside the Derive site", inside)
        # A script's SetClassProperty (Lua_Callbacks::SetClassProperty): its one vcall to the
        # class's SetProperty (slot 0x18), the class in EDI, the value in ESI, the hash in EAX
        # (modtools) or at [EAX] (Steam, GOG), nothing relocated, no branch into it, and after it
        # only POP ESI / POP EBX: the shim's pushad/popad leave it as SetProperty would.
        va = addrs["lua_set_class_property_site"]
        lua_text = "8B 17 56 50 8B CF FF 52 18" if kind == "modtools" else "8B 17 8B CF 56 FF 30 FF 52 18"
        size = len(lua_text.split())
        assert image[va - base:va - base + size] == bytes.fromhex(lua_text), (build, "Lua SetClassProperty bytes")
        assert ", ".join("0x" + b for b in lua_text.split()) in odf, (build, "Lua site bytes changed in the source")
        ins = list(decoder.disasm(image[va - base:va - base + 16], va))
        want = ([("mov", "edx, dword ptr [edi]"), ("push", "esi"), ("push", "eax"), ("mov", "ecx, edi"),
                 ("call", "dword ptr [edx + 0x18]")] if kind == "modtools" else
                [("mov", "edx, dword ptr [edi]"), ("mov", "ecx, edi"), ("push", "esi"), ("push", "dword ptr [eax]"),
                 ("call", "dword ptr [edx + 0x18]")])
        assert [(x.mnemonic, x.op_str) for x in ins[:5]] == want, (build, [(x.mnemonic, x.op_str) for x in ins[:6]])
        assert ins[5].address == va + size and (ins[5].mnemonic, ins[5].op_str) == ("pop", "esi"), (build, "after")
        assert not any(va - 3 <= r < va + size for r in relocs), (build, "relocated Lua site bytes")
        lua_fn = {"modtools": 0x004728F0, "steam": 0x0058ECC0, "gog": 0x0058FC60}[build]
        inside = {t for t in branch_targets(image, base, lua_fn, va + 0x20) if va < t < va + size}
        assert not inside, (build, "a branch lands inside the Lua site", inside)
        print("%-8s ok: Derive sites 0x%08X (EntityClass::Read), 0x%08X (WeaponClass::Read); Lua SetClassProperty 0x%08X"
              % (build, addrs["entity_class_read_derive_site"], addrs["weapon_class_read_derive_site"], va))
    finally:
        pe.close()

# The script route's shims push (this, hash, value, slot 0x18) to its own dispatcher, which skips the
# ODF-only listeners (attached_effects_cleanup's RemoveOnDeath).
for name, hash_push in (("shim_mt_lua_class", "push eax"), ("shim_rt_lua_class", "push dword ptr [eax]")):
    body = re.search(r"void " + name + r"\(\)\s*\{\s*__asm \{(.*?)\}\s*\}", odf, re.S).group(1)
    lines = [ln.split("//")[0].strip() for ln in body.splitlines() if ln.split("//")[0].strip()]
    assert lines[:7] == ["pushad", "push 0x18", "push esi", hash_push, "push edi", "call odf_script_prop_dispatch",
                         "add  esp, 16"] and lines[7] == "popad" and lines[-1] == "jmp  [s_contLuaClass]", (name, lines)
assert "if (s_handlerScripts[i]) consumed = s_propertyHandlers[i](cls, hash, value);" in odf
cleanup = (SRC / "entity" / "attached_effects_cleanup.cpp").read_text()
assert "odf_add_property_handler(on_property, false)" in cleanup

print("ODF listener ABI: all builds ok")
