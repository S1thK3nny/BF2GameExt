#!/usr/bin/env python3
"""Dump a Galactic Conquest save (.gc) and show how full it is.

File layout (all builds, written by SaveFsm, read back the same way):
    u32  CRC-32/BZIP2 of the 512 bytes below
    512  zlib stream (level 9), zero padded

ScriptCB_SaveMetagameState deflates its Lua arguments into that fixed 512 byte
buffer in one shot. A stream that does not fit is cut off and still saved, so
"TRUNCATED" here means the save already lost data.

Uncompressed stream: u8 item count, then one tagged item per Lua argument:
    0 nil | 1 bool u8 | 2 lightuserdata u32 | 3 number f32 | 4 string, NUL ended
    5 table: key/value item pairs until a 0 byte | 6 function, global name
    7 userdata, 8 thread: tag only, load back as nil
Numbers are float32, so integers above 16777216 and most fractions lose
precision. .rote (campaign) files use the same encoding with a 256 byte buffer.

Usage: gcread.py [-s] file.gc [...]    (-s: header lines only)
"""
import struct
import sys
import zlib

TYPE_NAMES = {0: "nil", 1: "bool", 2: "lightuserdata", 3: "number", 4: "string",
              5: "table", 6: "function", 7: "userdata", 8: "thread"}


def crc32_bzip2(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) if crc & 0x80000000 else (crc << 1)
            crc &= 0xFFFFFFFF
    return crc ^ 0xFFFFFFFF


def read_item(buf, pos):
    t = buf[pos]
    pos += 1
    if t == 0:
        return None, pos
    if t == 1:
        return bool(buf[pos]), pos + 1
    if t in (2, 3):
        (v,) = struct.unpack_from("<f", buf, pos)
        if t == 3 and v == int(v):
            v = int(v)
        return v, pos + 4
    if t in (4, 6):
        end = buf.index(0, pos)
        s = buf[pos:end].decode("latin-1")
        return (s if t == 4 else "<function %s>" % s), end + 1
    if t == 5:
        pairs = []
        while buf[pos] != 0:
            k, pos = read_item(buf, pos)
            v, pos = read_item(buf, pos)
            pairs.append((k, v))
        return dict(pairs), pos + 1
    return "<%s>" % TYPE_NAMES.get(t, "type %d" % t), pos


def fmt(v, indent=0):
    pad = "  " * indent
    if isinstance(v, dict):
        if not v:
            return "{}"
        lines = ["{"]
        for k in sorted(v, key=lambda k: (not isinstance(k, (int, float)), str(k))):
            key = k if isinstance(k, str) and k.isidentifier() else "[%s]" % fmt(k)
            lines.append("%s  %s = %s," % (pad, key, fmt(v[k], indent + 1)))
        lines.append(pad + "}")
        return "\n".join(lines)
    if v is None:
        return "nil"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, str) and not v.startswith("<"):
        return '"%s"' % v
    return repr(v)


def dump(path, summary):
    raw = open(path, "rb").read()
    print("file:  %s (%d bytes)" % (path, len(raw)))
    if len(raw) < 5:
        print("too short\n")
        return
    (stored,) = struct.unpack_from("<I", raw, 0)
    payload = raw[4:]
    calc = crc32_bzip2(payload)
    print("crc:   %08X stored, %08X computed, %s" % (stored, calc, "OK" if stored == calc else "MISMATCH"))

    d = zlib.decompressobj()
    try:
        plain = d.decompress(payload)
    except zlib.error as e:
        print("inflate failed: %s\n" % e)
        return
    used = len(payload) - len(d.unused_data)
    print("zlib:  %d of %d bytes (%d free, %.0f%% full), %s"
          % (used, len(payload), len(payload) - used, 100.0 * used / len(payload),
             "complete" if d.eof else "TRUNCATED, the save did not fit"))
    print("raw:   %d bytes (%.1f:1)" % (len(plain), len(plain) / max(used, 1)))
    if not plain:
        print()
        return
    print("items: %d\n" % plain[0])
    if summary or not d.eof:
        return

    pos = 1
    for i in range(1, plain[0] + 1):
        start = pos
        v, pos = read_item(plain, pos)
        print("-- item %d (%d raw bytes) --" % (i, pos - start))
        print(fmt(v) + "\n")


def main():
    args = sys.argv[1:]
    summary = "-s" in args
    files = [a for a in args if a != "-s"]
    if not files:
        sys.exit(__doc__)
    for f in files:
        dump(f, summary)


if __name__ == "__main__":
    main()
