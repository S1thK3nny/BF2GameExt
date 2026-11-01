#!/usr/bin/env python3
"""Grow a Galactic Conquest save (.gc) past the stock 512 byte limit, for testing.

Adds string keys "__gcpad1", "__gcpad2", ... holding random text to one table
item of the save, recompresses at level 9 and writes the file the way
BF2GameExt does: CRC-32/BZIP2, then the zlib stream, zero padded to 512 bytes
when it fits, else the stream alone.

The default item is 24, planetResources in the stock ifs_freeform_main. The
script only ever indexes it by team, so the extra keys ride along through every
load and save. A nil item becomes a table holding only the padding. Check the
item with gcread.py first on a modded GC, whose order may differ.

Usage: gcpad.py in.gc out.gc [--size BYTES] [--item N] [--seed N]
    --size  compressed size to reach, default 2000 (max 16384)
    --item  1-based item to pad, default 24
"""
import argparse
import os
import random
import string
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gcread  # noqa: E402

ALPHABET = (string.ascii_letters + string.digits).encode()


def item_span(plain, index):
    """(start, end) of the 1-based top-level item in the raw stream."""
    pos = 1
    for i in range(1, plain[0] + 1):
        start = pos
        _, pos = gcread.read_item(plain, pos)
        if i == index:
            return start, pos
    raise ValueError("the save holds %d items, no item %d" % (plain[0], index))


def padded(plain, index, pairs):
    start, end = item_span(plain, index)
    tag = plain[start]
    if tag == 5:     # table: new pairs go straight after the tag
        return plain[:start + 1] + pairs + plain[start + 1:]
    if tag == 0:     # nil: becomes a table of padding
        return plain[:start] + b"\x05" + pairs + b"\x00" + plain[end:]
    raise ValueError("item %d is a %s, not a table or nil"
                     % (index, gcread.TYPE_NAMES.get(tag, "type %d" % tag)))


def build(plain, index, target, rng):
    pairs = b""
    n = 0
    while True:
        n += 1
        value = bytes(rng.choice(ALPHABET) for _ in range(200))
        pairs += b"\x04__gcpad%d\x00\x04" % n + value + b"\x00"
        stream = zlib.compress(padded(plain, index, pairs), 9)
        if len(stream) >= target:
            return stream, n


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--size", type=int, default=2000)
    ap.add_argument("--item", type=int, default=24)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()

    info = gcread.decode(open(a.src, "rb").read())
    if not info["complete"]:
        sys.exit("%s is truncated, pad a save that loads" % a.src)

    try:
        stream, n = build(info["plain"], a.item, a.size, random.Random(a.seed))
    except ValueError as e:
        sys.exit("%s: %s (pick another with --item)" % (a.src, e))
    payload = stream.ljust(gcread.STOCK_PAYLOAD, b"\x00")
    out = struct.pack("<I", gcread.crc32_bzip2(payload)) + payload
    open(a.dst, "wb").write(out)
    print("%s: item %d +%d keys, %d byte stream, %d byte file%s"
          % (a.dst, a.item, n, len(stream), len(out),
             "" if len(stream) <= gcread.MAX_PAYLOAD else " (over the 16 KB limit)"))


if __name__ == "__main__":
    main()
