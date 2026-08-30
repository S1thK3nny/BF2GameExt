# EntityPath branch regions

Why a path node's `BranchRegion("id")` property has never resolved in any
shipping build of BF2, the two independent defects behind it, and why no
amount of reading the disassembly found either of them.

## What a branch region is meant to do

An `EntityPath` node can carry a trigger volume. Two node properties write the
same field, and `EntityPath::NodeProperties::ReadPropertyScope` (modtools
`0x005E4D80`) handles both:

```
Range(r)            ->  node+0x1C = r * r          node+0x20 = 1
BranchRegion("id")  ->  node+0x1C = BranchRegion*  node+0x20 = 2
```

Both then set bit `0x04` in the flags byte at `node+0x21`. Node property records
are `0x24` bytes. So `+0x20` is a discriminator: the node's trigger is either a
plain sphere of `Range` centred on the node itself, or a named region that can
sit anywhere in the world. The named form is the only one that decouples the
trigger volume from the node's own position.

The region half is built at load time. Each region description in the world goes
through `LoadUtil::ProcessRegionInfo`, which asks `RedRegionFactory::Find` for
the first registered factory whose name prefix matches the region's name and
calls that factory to construct the object. `EntityPath::BranchRegionFactory`
registers the prefix `entitypathbranch`.

`EntityPath::BranchRegionFactory::CreateRegion` (modtools `0x005E4C90`) rejects
anything whose descriptor `mTypeId` is not `PblHash("sphere")` = `0xAFD98518`,
so a branch region must be a sphere region. It takes the centre from the
descriptor at `+0x30` and the radius from `+0x4C`, allocates `0x24` bytes, and
constructs an `EntityPath::BranchRegion`, which links itself into the global
branch-region list. `EntityPath::BranchRegion::FindByID` (modtools `0x005E4C20`)
walks that list comparing `region+0x20`, the object's last dword.

Relevant PblHashes:

| Text | PblHash |
| --- | --- |
| `BranchRegion` (the node property) | `0x7554BDFD` |
| `Range` (the sibling property) | `0xFADC0CD2` |
| `sphere` (the descriptor type gate) | `0xAFD98518` |
| `entitypathbranch` (the factory prefix) | `0x3972C091` |

## The symptom

On modtools, every map that uses branch regions logs one warning per node
property:

```
Unable to find branch region <id>
```

The format string is at modtools `0x00A4B5D8`, and the `RedWarning::SetLogData`
call that precedes it names `EntityPath.cpp` line 254. `ReadPropertyScope` still
returns "handled" afterwards, so the node ends up with no trigger volume at all
and the path silently loses its branch.

On Steam and GOG the same failure is completely silent. Those images do not
contain the text: a string search for `branch region` across `BattlefrontII.exe`
returns nothing, the warning having been compiled out with the rest of the
`RedWarning` strings. Nothing is written anywhere. From a mapper's seat on
retail, branch regions simply do nothing and never say why.

## Defect 1: the wrong vtable slot

`LoadUtil::ProcessRegionInfo` dispatches region creation through vtable slot
[1]:

```c
(**(code **)(*factory + 4))(desc, name)
```

Every factory that works overrides that slot. On modtools:

```
soundstatic  vtbl 0x00A2B970  slot1 = 0x00403E0E   (its own)
danger       vtbl 0x00A47014  slot1 = 0x00405C22   (its own)
```

`EntityPath::BranchRegionFactory` does not. Its slot [1] still holds the
inherited `RedRegionFactory::CreateRegion`, which builds a plain `RedRegion`,
and its own creator sits in slot [3], which nothing calls:

```
entitypathbranch vtbl 0x00A4B5A4
    slot0  0x0040FAB5 -> 0x005E4690
    slot1  0x00821F60     RedRegionFactory::CreateRegion (base)
    slot2  0x00821FC0     base
    slot3  0x0040DF58 -> 0x005E4C90  BranchRegionFactory::CreateRegion
```

Ghidra labels that slot [1] outright as "RedRegionFactory member function
inherited by EntityPath::BranchRegionFactory". It reads like a signature
mismatch that made the compiler append a new virtual instead of overriding the
existing one.

The consequence is total: no `BranchRegion` object has ever been constructed, in
any map, on any build. That was proven at runtime on modtools before anything
was changed: the factory is registered and is correctly selected by name
prefix - `Find("entitypathbranch <id>")` returns it - yet its `CreateRegion` is
never entered, and the branch-region list count stays at zero while the lookups
run.

The repair is one dword per build. Point slot [1] at the real implementation:

| Build | vtable | Slot [1] patched at | Old value (base) | New value |
| --- | --- | --- | --- | --- |
| Modtools | `0x00A4B5A4` | `0x00A4B5A8` | `0x00821F60` | `0x005E4C90` |
| Steam | `0x0079C440` | `0x0079C444` | `0x006DC930` | `0x004D0F00` |
| GOG | `0x0079D3E0` | `0x0079D3E4` | `0x006DD9D0` | `0x004D0F00` |

That is patch set "EntityPath Branch Region Fix" in `patch_table.cpp`. The old
value is verified before the write, so a build whose vtable does not match is
left alone.

## Defect 2: the id is hashed one character early

Repairing the slot makes regions exist. It does not make them findable, because
the id they register under is not the id anyone would write.

`CreateRegion` derives it like this:

```c
p = strchr(name, ' ');      // p points AT the space
if (!p || !p[1]) return 0;  // "entitypathbranch" alone is rejected
hash = PblHash(p);          // hashed FROM the space, inclusive
BranchRegion::BranchRegion(this, &desc->centre, desc->radius, hash);
```

The retail builds are the same shape, with the radius passed in XMM2 rather than
on the stack. `ReadPropertyScope`, on the other side, hashes the node property's
argument exactly as written.

So a region named `entitypathbranch dropzone1` registers as `" dropzone1"`,
while a node saying `BranchRegion("dropzone1")` asks for `"dropzone1"`. Measured
live in one session, with both halves instrumented:

| Text actually hashed | PblHash |
| --- | --- |
| `" dropzone1"` - what `CreateRegion` registered | `0x5C8E3E3B` |
| `"dropzone1"` - what `FindByID` was asked for | `0x13A09F91` |

Had the original written `p + 1`, the two would agree. Nothing downstream cares
what the id is, only that the two sides derive it the same way, so the feature
was one character from working.

## The fix

`PatcherDLL/src/entity/branch_region_fix.cpp`, INI `[Fixes] BranchRegionFix=1`.

Part 1 is the vtable-slot patch above. Part 2 hooks `CreateRegion`, lets the
engine construct and link the region exactly as it always has, and then
re-stamps `mHashID` at `+0x20` with the hash of the text *after* the separator:

```c
void* region = orig_CreateRegion(desc, name);
const char* space = strchr(name, ' ');
if (region && space && space[1])
    *(uint32_t*)((char*)region + 0x20) = PblHash(space + 1);
```

`+0x20` is verified on modtools (`FindByID` compares `[region+0x20]`) and on both
retail builds (the constructor writes the hash argument to `this+0x20`).

**Why re-stamp rather than build the region ourselves.** Replacing `CreateRegion`
outright would mean allocating with the engine's allocator and calling
`BranchRegion::BranchRegion` directly, and that constructor's signature is not
the same across builds: modtools passes the radius as a stack float, the retail
builds pass it in XMM2. Porting would then need the allocator, the constructor
address and a per-build calling convention for each of the three targets. The
shape actually used needs one hook address and one field offset, both of which
port cleanly. Calling the engine's `CreateRegion` *and* building our own was
never on the table either - that would register every region twice, the second
time under an id nobody uses.

The engine's own id is not kept as an alias. The leading-space spelling is not
supported and deliberately so: the feature has never worked for anyone, so there
is no content in the wild relying on it.

A diagnostic ships alongside: `[Diagnostic] BranchRegionDebug=0`,
`PatcherDLL/src/entity/branch_region_debug.cpp`, modtools only, off by default.
It changes no behaviour; it narrates `RedRegionFactory::Find`, `CreateRegion`
and `FindByID` - which factory claims each region, the id each side derives, and
the live region count on every line - so an id that will not resolve can be
traced rather than guessed at.

## Authoring

Name the region `entitypathbranch <id>` and make it a sphere. In the path node,
write `BranchRegion("<id>")`:

```
region:  entitypathbranch dropzone1     (sphere)
node:    BranchRegion("dropzone1");
```

The separator is a single space, the id is everything after it, and matching is
case-insensitive because `PblHash` folds case. A region named
`entitypathbranch` with nothing after it is rejected by the engine before the
hook ever sees it.

## Status

Verified on modtools: all 24 branch region lookups resolve, and the log has no
`Unable to find branch region` lines left.

The Steam and GOG addresses are derived and byte-verified at load - the vtable
slot's old value must match before it is written, and the hook address comes out
of the address registry - but the feature has **not** been verified in actual
play on either retail build. Nothing here should be read as "confirmed working
on retail". The two things that would have to hold and have not been observed
are that retail's `LoadUtil::ProcessRegionInfo` dispatches through the same slot
[1] (read from disassembly, not run) and that `mHashID` is genuinely at `+0x20`
in the retail object (read from the constructor, not run).

## Methodology note

This bug is worth recording for how it was found rather than what it was.

A succession of hypotheses was derived from static reading, and every one of
them was wrong. They were all of the same shape - a plausible account of how
`CreateRegion` or the lookup could go astray, argued from the decompilation of
code that was, in fact, never being executed at all. No amount of further
reading was going to distinguish between them, because the premise they shared
was the thing that was false.

The root cause was only found by instrumenting the three steps that have to line
up - which factory claims the region, whether the branch creator is entered and
with what id, and what the node property asks for - and reading the numbers.
Both defects fell out of the run with all three instrumented. On modtools the
instrumentation points are `RedRegionFactory::Find` `0x008224C0`,
`BranchRegionFactory::CreateRegion` `0x005E4C90`, `BranchRegion::FindByID`
`0x005E4C20`, the list count at `0x00AD345C` and `RedRegionFactory::sList` at
`0x00E5F578`.

Two lessons generalise. A function that is never called decompiles exactly like
one that is, so "this code is wrong" and "this code does not run" look identical
on the page - check entry before checking logic. And when two halves of a system
derive the same key independently, log both keys before theorising about either.
