# Triaging "struct too short" reports in the Ghidra database

A tool flagged 15 structs as too short, inferring a required size from where an overflowing
access lands, and 16 functions whose `RET imm16` disagreed with their recorded parameters.
Triaged 2026-08-21. **Result: 0 of 15 struct claims were real. 5 of 16 function claims were.**

The value here is not the specific list; it is that "a store lands past the recorded end" has at
least five causes and only one of them is fixed by growing the type. Growing a struct to silence
the symptom converts an obviously-broken decompile into a confidently-wrong one, which is worse.

## The five causes

**1. Base class mistyped as `this` on a derived method.** The dominant mode - 8 of 15. The base
size is correct; a DERIVED class's method has `this` typed as the base, so the derived object's
own fields render as overflow. Diagnostic: find a derived constructor, and check whether its
first own field sits at exactly the base's size.

Verified: `WeaponClass` is 772 (0x304), and `WeaponLaserClass` (`0x007BF5B0`) calls the base ctor
then writes its first own field at `0x007BF5C6 MOV dword [ESI+0x304],0x3F800000`. Exactly the
base size, so the base is right. Same shape confirmed for `OrdnanceClass` (0x10C),
`RedInterfaceElement` (0x70), `ElementBitmapBase` (0x11C), `Thread` (0x18), `EntityItemClass`
(0x418), `CollisionObject` (0x88), `RedSceneObject` (0x58).

Real fix: retype `this` on the derived methods, or create the derived types.

**2. Array-element value types.** A vertex or vector type exists to live in a buffer, so indexing
past element 0 is correct. Diagnostic: compiler-emitted stride arithmetic, which is the
strongest evidence available.

`PblVector3` is 12: `CollisionMesh::_GetTriNormal` does `LEA EAX,[EAX+EAX*2]` then
`[EDX+EAX*4]` = `i*12`. `FlareVertex` 24: `EmitRing` advances `ADD EAX,0x18`.
`FlareCompressedVertex` 16: `ADD EAX,0x10`. Growing any of these would corrupt every consumer.

**3. Bare-name collapse.** A name-keyed tool measures one type and reports another.
**`Node` has 102 distinct structs** in this database (sizes 4/8/12/16/20/28/64/80) and
**`Factory` has 55** (including both 308 AND 316). The flagged "20 needs 28" and "308 needs 316"
are each just another type in the same name bucket. Always resolve by full DataType path.

**4. Struct hack / variable-length payload.** `NetPacket` is a 12-byte header plus a
`char mData[4]` placeholder, allocated at BOTH 1024 and 128 bytes depending on pool. No single
struct size can be correct, so any "needs >= N" is meaningless.

**5. Interior gap** - the one case where the type IS wrong, but appending is still the wrong fix.
See `LoadDisplaySystem.md`: appending satisfies the size check while leaving every field after
the gap mislabelled.

## Conventions: `params x 4` is not the implied stack size

11 of the 16 flagged functions were already correct. The tool assumed each parameter occupies 4
bytes, which is false for MSVC member functions taking large by-value structs. Stored storage
sums to the `RET` imm16 exactly once real widths are used (`PABaseKey` 44, `CollisionPrimitive`
32, `DamageOwner` 28, `PblVector3` 12, `PblHandle` 8, `PblAngle` 8):

    PAAnimation::InterpolateKeys   44+44+4+4   = 96 = RET 0x60
    CollisionBodyPrimitive::Init   32+4+4+4    = 44 = RET 0x2C
    DamageDesc::DamageDesc         28+4        = 32 = RET 0x20
    EntityWalker::AdjustRotation   8           =  8 = RET 0x08

## Two traps for any validator

**Do not "repair" tightly packed arrays.** `RedSceneObject._iFrameNum` and `_uiOcclusionIndex`
are `int[1]` at `0x50` and `0x54`, indexed as `[EDI+EAX*4+0x50]` and `+0x54`. Lengthening either
would manufacture a genuine interior gap.

**Stack realignment hides parameters.** `EntityWalker::AdjustRotation` (`0x00593BC0`) does
`MOV EBX,ESP` / `AND ESP,0xFFFFFFF0` and then addresses its parameters off **EBX**
(`MOVSS XMM4,[EBX+0x8]`). An EBP-offset validator sees zero parameter references and would
report the function as having none.

## Applying signature fixes

Hidden-struct-return functions **cannot** be fixed with
`Function.updateFunction(..., DYNAMIC_STORAGE_FORMAL_PARAMS, ...)`. For a small return type
(`RedColor` 4 bytes, `Itor` 8) Ghidra decides it returns in `EAX`, DROPS an explicit
`__return_storage_ptr__` parameter and shifts the survivors, producing a worse signature than
before. Five functions were damaged this way and reverted. Use `CUSTOM_STORAGE` with explicit
assignments (`this` in ECX, buffer at stack 0x4), then set the purge size, then verify
`hasCustomVariableStorage()` and that the purge matches the `RET` imm16.
