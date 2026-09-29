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
    print(f'{build}: all 9 native entry fingerprints and the handler unlink passed')
