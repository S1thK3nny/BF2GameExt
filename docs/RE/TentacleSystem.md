# TentacleSimulator

The acklay/rancor tentacle solver. Notes taken while porting RJP1992's tentacle
limit patch (`origin/dinput-hook`, `PatcherDLL/src/tentacle_patch.cpp`) into
`PatcherDLL/src/entity/tentacle_limit.cpp`.

All addresses are unrelocated (imagebase `0x400000`).

## Functions

| | modtools | Steam | GOG |
|---|---|---|---|
| `TentacleSimulator::TentacleSimulator` | `0x0056D090` | `0x00655770` | `0x00656810` |
| `DoTentacles` | `0x0056F4E0` | `0x006558F0` | `0x00656990` |
| `UpdatePose` | `0x0056DC80` | `0x00655B60` | `0x00656C00` |
| `UpdatePositions` | `0x0056E420` | `0x00656270` | `0x00657310` |
| `EnforceCollisions` | `0x0056F020` | `0x006569C0` | `0x00657A60` |
| bone-name hash table | `0x00A442F0` | `0x0078B630` | `0x0078C5D0` |

Conventions, taken off the `RET n` of each function rather than inferred:

```
ctor              __thiscall(numTentacles, bonesPerTentacle, collType)          RET 0xC
DoTentacles       __thiscall(pose, parentMatrix, velocity, targetMats, float dt) RET 0x14
UpdatePose        __thiscall(pose, parentMatrix, bonePtrs[][5], targetMats)     RET 0x10
EnforceCollisions __thiscall(pose, parentMatrix)                                RET 0x8
UpdatePositions   modtools: __thiscall(dt, velocity, pose, parentMatrix, bonePtrs) RET 0x14
                  Steam/GOG: dt in XMM1, dropped from the stack                    RET 0x10
```

`UpdatePositions` is the only one the retail LTCG build re-writes; the naked shim
in `tentacle_limit.cpp` adapts the modtools shape to it.

## Struct layout

Read off the Steam constructor at `0x00655770` and cross-checked against the
modtools Verlet step, which addresses `oldPos` as `tPos + 0x120`:

```
0x000  PblVector3 tPos[4][6]        row stride 0x48, element stride 0xC
0x120  PblVector3 oldPos[4][6]
0x240  PblVector3 oldVelocity
0x24C  float      mInternalTimer
0x250  float      mTimeSinceLastUpdate     net extrapolation only
0x254  float      mTimerOffset             net extrapolation only
0x258  int        mNumTentacles
0x25C  int        mBonesPerTentacle
0x260  int        mCollType
0x264  bool       mFirstUpdate
                                           sizeof == 0x268
```

Allocation is a dedicated `MemoryPool` named `"TentacleSimulator"`. The size
`0x268` appears as a `PUSH` immediate at three sites per build: the pool setup
(a static initializer, so a DLL loaded through the import table gets there
first), `EntitySoldier`'s constructor, and the spawn-screen display soldier.

| | modtools | Steam / GOG |
|---|---|---|
| pool setup | `0x00A17181` | `0x004069D1` |
| `EntitySoldier` ctor | `0x005347CA` | `0x004DF909` |
| display soldier | `0x006745CD` | `0x0048DA5E` |

### Phantom is not a valid reference here

The Phantom dev build's `TentacleSimulator` is **608 bytes**, has no
`mTimeSinceLastUpdate` / `mTimerOffset`, and its `DoTentacles` takes four
arguments with no `dt` and no `netFrameLock` branch. The two float members and
the `dt` argument were added after Phantom was cut, which shifts every scalar by
8. The PDB in `tools/types.txt` describes the same pre-change layout. Use the
Steam constructor as ground truth for this struct, not the PDB.

### A modtools-only defect

In `BattlefrontII.Debug.FullScreen.1080.exe` two functions address the struct
inconsistently with the rest of the same binary:

- the constructor's zeroing loop (`0x0056D0F1`, `LEA EBP,[EDX+0x260]`) and
  `DoTentacles`' first-frame seeding (`0x0056F6DB`, `LEA EDI,[ECX+0x260]`) treat
  `oldPos` as `this+0x260` rather than `this+0x120`;
- `DoTentacles` reads and clears `mFirstUpdate` at `this+0x4A4`
  (`0x0056F683` / `0x0056F74B`) where the constructor writes it at `this+0x264`.

`EnforceCollisions` reads `mCollType` at `this+0x260` (`0x0056F05C`) and
`UpdatePositions` uses `oldPos = tPos + 0x120`, so the simulation itself is
correct; it is the seeding pass that writes past the `0x268` block and over
`mCollType`. Steam, GOG and Phantom are all internally consistent. Both
offending functions are replaced by the limit patch, so enabling it removes the
defect as a side effect.

## Bone names

`DoTentacles` looks bones up in the pose by CRC-32/BZIP2 of `bone_string_N`,
indexed `[bonesPerTentacle * tentacle + bone]`. The static table holds exactly
20 entries (`bone_string_1` .. `bone_string_20`, i.e. 4 x 5) and is followed by
0x2C zero bytes and then the `"TentacleSimulator"` pool name.

`bone_string_1` hashes to `0x24CB9E5E`, which `DoTentacles` also uses as its
presence guard: no work happens unless that key resolves in the pose.

`UpdatePose` reads its `_Remove`/`_Store` keys straight out of that table, which
is why driving the stock sub-functions in groups of four requires swapping the
current group's names in for the duration of each call.

## Pose hash table

`PblHashTableCode::_Find(uint* table, int size, uint hash)` (modtools
`0x007E1A40`), `size` always `0x100`:

```
half = size >> 1                    // 128
p    = table + ((half - 1) & hash)
if hash == 0: return null
loop:
   if *p == hash: return p[half]    // values occupy the high half
   if *p == 0:    return null
   if p <= table: p += half         // wrap
   p -= 1
```

There is no iteration cap; a full table plus a missing key spins forever. The
reimplementation in `tentacle_limit.cpp` bounds the probe at `half` steps.

## The class bitfield

`EntitySoldierClass` packs all three tentacle properties into one word, at
`+0x8BC` on modtools and `+0x6C8` on Steam/GOG:

```
bits  7- 9   NumTentacles       0-7    against arrays dimensioned for 4
bits 10-13   BonesPerTentacle   0-15   against a fixed 4x5 stack array
bits 14-15   TentacleCollType   0-3
```

The limit patch takes one bit from `BonesPerTentacle`, whose useful range is
0-5, and gives it to `NumTentacles`:

```
bits  7-10   NumTentacles       0-15
bits 11-13   BonesPerTentacle   0-7
bits 14-15   TentacleCollType   unchanged
```

Every edit is length-neutral: a mask immediate, a shift count, or an `AND`
immediate. Sweeping all three `.text` sections for references to the class
displacement finds 21 sites per build that decode these two fields, and all 21
fall inside five regions:

| region | modtools | Steam / GOG |
|---|---|---|
| class field-by-field copy | `0x0053F120` / `0x0053F136` | `0x004F5FEF` / `0x004F6004` |
| `SetProperty` NumTentacles store | `0x00541D13` | `0x004FA2EE` |
| `SetProperty` BonesPerTentacle store | `0x0053FC2E` / `0x0053FC35` | `0x004F84C3` / `0x004F84CB` |
| `EntitySoldier` ctor extraction | `0x005347AD`, `0x005347F5`, `0x005347F8`, `0x005347FF` | `0x004DF8ED`, `0x004DF935`, `0x004DF939`, `0x004DF940` |
| display soldier extraction | `0x006745A4`, `0x006745F8`, `0x006745FB`, `0x00674602` | `0x0048DA35`, `0x0048DA8A`, `0x0048DA8E`, `0x0048DA95` |

Both `SetProperty` stores are the usual `field ^= (field ^ (value << shift)) & mask`
insert with no separate clamp on the value, so widening the mask is sufficient
and values above the field width are truncated rather than spilling.

### Neither field is clamped by the engine

modtools warns and does not clamp: `"Too many tentacles!"` behind
`0x00541CD3 CMP EAX,4 / JBE`, and `"Too many bones per tentacle!"` behind
`0x0053FBF5 CMP EAX,5 / JBE`. In both cases the `JBE` only skips the warning.
Retail compiled both checks out entirely, strings included, so a mod hits this
with no diagnostic at all.

That makes `NumTentacles` 5-7 and `BonesPerTentacle` 6-15 reachable from an ODF
typo against arrays dimensioned for 4 and 5 respectively; the bones case
overruns a stack array and reaches the saved return address. No stock ODF
exceeds either, so it is latent. `tentacle_limit.cpp` clamps both in its own
constructor, which covers the case while the feature is on; with the feature off
the stock behaviour, and the stock bug, are unchanged.
