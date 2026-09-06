"""Linear-sweep the .text of a BF2 exe and report every instruction whose
disp32 or imm32 equals one of the target constants.

Raw byte matching produces mid-instruction false positives; this decodes so a
hit is only reported when the constant really is an operand.
"""
import sys, capstone, pefile


class Img:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.d = open(path, "rb").read()
        self.secs = [(s.VirtualAddress + self.base, s.Misc_VirtualSize,
                      s.PointerToRawData, s.Name.rstrip(b"\0").decode())
                     for s in self.pe.sections]

    def text(self):
        for v, vs, pr, n in self.secs:
            if n == ".text":
                return v, vs, pr
        raise SystemExit("no .text")


def sweep(path, targets, lo=None, hi=None):
    im = Img(path)
    v, vs, pr = im.text()
    blob = im.d[pr:pr + vs]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    found = {t: [] for t in targets}
    # Linear sweep from every byte is too noisy; sweep from function starts is
    # unavailable, so sweep linearly and resync on decode failure.
    off = 0
    while off < len(blob):
        got = False
        for insn in md.disasm(blob[off:off + 4096], v + off):
            got = True
            if lo is not None and not (lo <= insn.address <= hi):
                off = insn.address - v + insn.size
                if insn.address > hi:
                    return found, im
                continue
            for op in insn.operands:
                val = None
                if op.type == capstone.x86.X86_OP_MEM and op.mem.disp:
                    val = op.mem.disp & 0xFFFFFFFF
                elif op.type == capstone.x86.X86_OP_IMM:
                    val = op.imm & 0xFFFFFFFF
                if val in found:
                    found[val].append(
                        (insn.address, insn.bytes.hex(" "),
                         f"{insn.mnemonic} {insn.op_str}"))
            off = insn.address - v + insn.size
        if not got:
            off += 1
    return found, im


if __name__ == "__main__":
    path = sys.argv[1]
    targets = [int(x, 16) for x in sys.argv[2:]]
    found, im = sweep(path, targets)
    for t in targets:
        print(f"=== 0x{t:X}  ({len(found[t])} sites)")
        for a, b, s in found[t]:
            print(f"    {a:08x}  {b:<26s} {s}")
