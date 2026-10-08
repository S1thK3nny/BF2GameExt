"""Read-only verification of NumberMath's installer guards against shipping PEs.

Usage: python tests/hud_number_math_abi_tests.py PATH_TO_GAMEDATA
Requires pefile. Reads the actual addresses and fingerprints from production
sources; never loads/executes the game or writes to its files.
"""
import codecs
import re
import struct
import sys
from pathlib import Path
import pefile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'PatcherDLL/src/render/hud_number_math.cpp').read_text()
addresses = (ROOT / 'PatcherDLL/src/core/game_addrs.hpp').read_text()
builds = {
    'modtools': 'BF2_modtools_NoDVD_NoConsole.exe',
    'steam': 'BattlefrontII.exe',
    'gog': 'BattlefrontII_GoG.exe',
}

def literal(value):
    return codecs.decode(value, 'unicode_escape').encode('latin1')

# Each conditional fingerprint has explicit MT/retail byte strings and masks.
pattern = (r'guard\(base, g_addr->(\w+),\s*modtools \? "([^"]*)" : "([^"]*)",'
           r'\s*modtools \? "([^"]*)" : "([^"]*)"\)')
guards = re.findall(pattern, source)
assert len(guards) == 5, 'Update audit parser if installer guard structure changes'
entry_pattern = (r'entry_guard\(base, (factoryVtable|itemVtable)\[(\d)\],\s*'
                 r'modtools \? "([^"]*)" : "([^"]*)",\s*'
                 r'(?:modtools \? "([^"]*)" : "([^"]*)"|"([^"]*)")\)')
entry_guards = re.findall(entry_pattern, source)
assert len(entry_guards) == 3
unlink_sites = {
    'modtools': (0x006AD6BF, bytes.fromhex('8B4108 8B510C 895004 8B410C 8B5108 8910')),
    'steam': (0x0055DB7E, bytes.fromhex('8B4A08 8B420C 894104 8B4A0C 8B4208 8901')),
    'gog': (0x0055E8FE, bytes.fromhex('8B4A08 8B420C 894104 8B4A0C 8B4208 8901')),
}
assert 'static_cast<char*>(h->next) + 4) = h->prev' in source, 'unlink_handler changed shape'

# The HUD editor's writer (modtools; GameExt keeps the editor off on Steam and
# GOG). The module writes these items' own lines through the writers HUD::Item::
# Write uses, in place of the stock transform's WriteData (+0x28), and leaves the
# stock write flag check (+0x10) alone when it has them; without them it keeps
# the items off the top level and writes nothing.
writers = re.search(r'memcmp\(indent, "([^"]*)", 10\) != 0\s*\|\| std::memcmp\(format, "([^"]*)", 10\)', source)
assert writers, 'Update the audit if resolve_writers changes shape'
assert 'itemVtable[10] = reinterpret_cast<void*>(write_data);' in source
assert 'if (!writeIndent) itemVtable[4] = reinterpret_cast<void*>(write_enabled);' in source
assert 'writeIndent(file, nullptr, indent);' in source and 'writeFormat(file, "%s\\n", n.lines[i]);' in source
# Read off the disassembly (modtools): HUD::Item's constructor turns the write
# flag (+0x18 bit 0) on; ViewPort::Read turns it off for each item it reads, and
# ViewPort::WriteData writes its items itself; Manager::Write writes an item
# whose flag is on; Item::Write writes the header from the factory's name, then
# calls WriteData through +0x28. Item::Write is the Vector3 vtable's +0x0C and
# the flag check its +0x10.
editor_sites = [
    (0x006B6FDC, '8A 56 18 80 CA 01'),      # Item ctor: MOV DL,[ESI+0x18]; OR DL,1
    (0x006B6FE4, '88 56 18'),               #            MOV [ESI+0x18],DL
    (0x006BDDF8, 'FF 52 08 8A 4E 18 80 E1 FE'),   # ViewPort::Read: the item's Read, then flag off
    (0x006BDE03, '88 4E 18'),
    (0x006BD749, 'FF 52 0C'),               # ViewPort::WriteData: CALL [EDX+0xC], Write
    (0x006B8958, 'FF 52 10'),               # Manager::Write: CALL [EDX+0x10], the flag
    (0x006B896C, 'FF 50 0C'),               #                 CALL [EAX+0xC], Write
    (0x006B6206, '68 F4 00 A6 00'),         # Item::Write: PUSH "%s(\"%s\")\n"
    (0x006B622F, 'FF 50 28'),               #              CALL [EAX+0x28], WriteData
    (0x006B624A, 'C2 0C 00'),               #              RET 0xC
]
viewport_vtable = 0x00A611DC   # ViewPort's: +0x08 Read, +0x28 WriteData

for build, filename in builds.items():
    body = re.search(r'namespace '+build+r'\s*\{(.*?)\n\s*\}\s*//\s*namespace '+build,
                     addresses, re.S).group(1)
    table = {name: int(value, 16) for name, value in
             re.findall(r'constexpr uintptr_t\s+(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;', body)}
    pe = pefile.PE(str(Path(sys.argv[1]) / filename))
    image = pe.get_memory_mapped_image()
    base = pe.OPTIONAL_HEADER.ImageBase
    assert base == 0x400000
    mt = build == 'modtools'

    def check(address, expected, mask):
        expected = literal(expected)
        actual = image[address-base:address-base+len(mask)]
        assert len(actual) == len(mask) == len(expected)
        assert all(m != 'x' or a == b for a, b, m in zip(actual, expected, mask)), (
            build, hex(address), actual.hex(), expected.hex(), mask)

    for name, mt_bytes, rt_bytes, mt_mask, rt_mask in guards:
        check(table[name], mt_bytes if mt else rt_bytes, mt_mask if mt else rt_mask)
    for vtable, slot, mt_bytes, rt_bytes, mt_mask, rt_mask, common in entry_guards:
        field = 'hud_vector3_factory_vtable' if vtable == 'factoryVtable' else 'hud_vector3_vtable'
        address = struct.unpack_from('<I', image, table[field]-base+int(slot)*4)[0]
        if image[address-base] == 0xe9:
            address += 5 + struct.unpack_from('<i', image, address-base+1)[0]
        check(address, mt_bytes if mt else rt_bytes, common or (mt_mask if mt else rt_mask))
    check(table['hud_event_send'], r'\x51\x8b\x09\xe8', 'xxxx')
    # TransformNumberLerp unlinks its third handler itself. EventHandler's
    # destructor (TransformNumber's dtor calls it on +0x40) is the same unlink
    # on every build: next = [h+8], prev = [h+0xC]; next->prev = prev at +4,
    # prev->next = next at +0.
    unlink = unlink_sites[build]
    assert image[unlink[0]-base:unlink[0]-base+len(unlink[1])] == unlink[1], (build, 'unlink')
    editor = ''
    if mt:
        def target(address):
            while image[address-base] == 0xe9:
                address += 5 + struct.unpack_from('<i', image, address-base+1)[0]
            return address

        def entry(vtable, offset):
            return target(struct.unpack_from('<I', image, vtable-base+offset)[0])

        def call(address):
            assert image[address-base] == 0xe8, (build, hex(address))
            return target(address + 5 + struct.unpack_from('<i', image, address-base+1)[0])

        check(table['hud_write_indent'], writers.group(1), 'x' * 10)
        check(table['hud_write_format'], writers.group(2), 'x' * 10)
        for address, text in editor_sites:
            want = bytes.fromhex(text.replace(' ', ''))
            assert image[address-base:address-base+len(want)] == want, (build, hex(address))
        assert image[0x00A600F4-base:0x00A600F4-base+10] == b'%s("%s")\n\0'
        vector3 = table['hud_vector3_vtable']
        assert entry(vector3, 0x0C) == 0x006B61D0, 'Item::Write'
        flag = entry(vector3, 0x10)
        assert image[flag-base:flag-base+6] == bytes.fromhex('8A4118 2401 C3'), 'the write flag check'
        assert call(0x006B61E0) == table['hud_write_indent'] and call(0x006B620C) == table['hud_write_format']
        assert entry(viewport_vtable, 0x08) == 0x006BDC20 and entry(viewport_vtable, 0x28) == 0x006BD560
        editor = ', the editor writers and its write path'
    print(f'{build}: all 9 native entry fingerprints and the handler unlink{editor} passed')
