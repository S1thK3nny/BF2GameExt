r"""Reconstruct a class's primary vtable (slot -> introducing method) from a
cvdump -t dump of the PDB.

Approach: every new vtable slot is an `LF_ONEMETHOD ... INTRODUCING VIRTUAL`
with a `vfptr offset = N`. Overrides reuse the introducing class's slot/name.
So: walk the primary-base chain (LF_BCLASS at offset 0) from the target class
to the root, collect all introducing virtuals, sort by vfptr offset.

Usage:  py -3 parse_vtable.py <ClassName> [types.txt]

NOTE: PDB build != modtools build. Cross-check the printed order against the
actual binary vtable (the already-labeled slots must line up) before trusting.
"""
import re, sys, os

TYPES = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "types.txt")

rec_hdr = re.compile(r"^0x([0-9a-fA-F]+) : Length = \d+, Leaf = 0x[0-9a-fA-F]+ (LF_\w+)")
cls_name = re.compile(r"class name = ([^,]+), UDT")
fl_type = re.compile(r"field list type 0x([0-9a-fA-F]+)")
bclass = re.compile(r"LF_BCLASS, \w+, type = 0x([0-9a-fA-F]+), offset = (\d+)")
vfptr = re.compile(r"vfptr offset = (\d+), name = '([^']+)'")
# LF_METHOD entry in a field list: references a method list by id, with a name
lf_method = re.compile(r"LF_METHOD, count = \d+, list = 0x([0-9a-fA-F]+), name = '([^']+)'")
# An INTRODUCING VIRTUAL line inside an LF_METHODLIST record (offset at end)
ml_intro = re.compile(r"INTRODUCING VIRTUAL,[^\n]*vfptr offset = (\d+)")

def load(path):
    """Return (udt_by_type, fieldlists). udt_by_type[tid]=(name, fl_id);
    fieldlists[fl_id]=(base0_tid_or_None, [(vfoff,name),...])."""
    blocks = {}  # tid -> list[str]
    cur = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = rec_hdr.match(line)
            if m:
                cur = int(m.group(1), 16)
                blocks[cur] = [line]
            elif cur is not None:
                blocks[cur].append(line)

    tid_to_name = {}     # every class tid (incl forward refs) -> name
    name_to_fl = {}      # name -> defining field list id
    fieldlists = {}
    methodlists = {}     # methodlist tid -> [intro vfptr offsets]
    for tid, lines in blocks.items():
        text = "".join(lines)
        leaf = rec_hdr.match(lines[0]).group(2)
        if leaf in ("LF_CLASS", "LF_STRUCTURE", "LF_INTERFACE"):
            nm = cls_name.search(text)
            if nm:
                name = nm.group(1).strip()
                tid_to_name[tid] = name
                if "FORWARD REF" not in text:
                    fl = fl_type.search(text)
                    if fl:
                        name_to_fl[name] = int(fl.group(1), 16)
            continue
        elif leaf == "LF_METHODLIST":
            offs = [int(m.group(1)) for m in ml_intro.finditer(text)]
            methodlists[tid] = offs
        elif leaf == "LF_FIELDLIST":
            base0 = None
            methods = []        # (vfoff, name) direct LF_ONEMETHOD intros
            ml_refs = []        # (methodlist_id, name)
            for e in re.split(r"\n\s*list\[\d+\] = ", "\n" + text.split("\n", 1)[1]):
                bm = bclass.search(e)
                if bm and int(bm.group(2)) == 0:
                    base0 = int(bm.group(1), 16)
                lm = lf_method.search(e)
                if lm:
                    ml_refs.append((int(lm.group(1), 16), lm.group(2)))
                elif "INTRODUCING VIRTUAL" in e:
                    vm = vfptr.search(e)
                    if vm:
                        methods.append((int(vm.group(1)), vm.group(2)))
            fieldlists[tid] = (base0, methods, ml_refs)
    return tid_to_name, name_to_fl, fieldlists, methodlists

def build(classname):
    tid_to_name, name_to_fl, fieldlists, methodlists = load(TYPES)
    if classname not in name_to_fl:
        print(f"{classname}: not found"); return
    slots = {}           # vfoff -> (name, ownerclass)
    chain = []
    name = classname
    seen = set()
    while name is not None and name not in seen:
        seen.add(name)
        chain.append(name)
        flid = name_to_fl.get(name)
        if flid is None or flid not in fieldlists:
            break
        base0_tid, methods, ml_refs = fieldlists[flid]
        for vfoff, mname in methods:
            slots.setdefault(vfoff, (mname, name))   # derived->base; keep most-derived
        for mlid, mname in ml_refs:
            offs = methodlists.get(mlid, [])
            if not offs:
                continue
            if len(set(offs)) == 1:                  # cvdump prints the ending offset
                end, k = offs[0], len(offs)
                slotlist = [end - 4 * (k - 1 - i) for i in range(k)]
            else:
                slotlist = sorted(offs)
            for o in slotlist:
                slots.setdefault(o, (mname, name))
        name = tid_to_name.get(base0_tid) if base0_tid is not None else None
    print(f"# {classname} primary vtable  (chain: {' -> '.join(chain)})")
    print(f"# {len(slots)} introducing slots")
    print("slot  byteoff  introduced_by::name")
    for vfoff in sorted(slots):
        mname, owner = slots[vfoff]
        print(f"{vfoff//4:>4}  {vfoff:>7}  {owner}::{mname}")

if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "EntityEx")
