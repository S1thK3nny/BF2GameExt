# Hover suspension rig (`Suspension*` ODF properties)

What the five `Suspension*` ODF properties do. Investigated 2026-09-04 from the
Phantom build; **all addresses below are Phantom**, not modtools, and have not been
ported. Field names come from the PDB.

Applies to `ClassLabel = "hover"` only. The stock users are the hailfire and
snail tank (which are hover-class despite the `tread` in their names) and the
republic/empire fighter tanks. Nothing in `data_BF3` sets them at the moment;
several stock ODFs carry them commented out.

```
SuspensionNodeName         = "CIS_HailFire_Droid_center"   // 0xE534C2CB
SuspensionLeftArmNodeName  = "armL"                        // 0xABB24116
SuspensionRightArmNodeName = "armR"                        // 0x580B459F
SuspensionMaxOffset        = "-0.5"                        // 0xF0856C5D
SuspensionMidOffset        = "-0.125"                      // 0x8EDA7A09
```

---

## Parsing -- `EntityHoverClass::SetProperty` (`0x00546af0`)

The three `*NodeName` properties do the same thing for three slots:

```c
mSuspensionCenterHash = PblTEMPHash(value);           // stored FIRST
RedPose pose; RedModel::SetupPose(mModel->_mLod[0], &pose);   // bind pose of LOD0
bone = PblHashTableCode::_Find(pose, 0x100, hash);
if (!bone) return;                                    // silent
mBaseSuspensionPos = bone->trans;                     // matrix +0x30/+0x34/+0x38
```

Left and right arms fill `mSuspensionLeftArmHash` / `mLeftSuspensionPos` and the
right-hand pair. `SuspensionMaxOffset` and `SuspensionMidOffset` are plain
`sscanf("%f")` into `mMaxSuspensionOffset` / `mMidSuspensionOffset`.

Note the order: **the hash is written before the bone lookup**, and a miss returns
without clearing it. A misspelt node name therefore still enables the whole
suspension path (everything at runtime is gated on `mSuspensionCenterHash != 0`)
with a zero base position. `Render` then fails its own pose lookup and draws
nothing for that bone, so the symptom is "no visible effect", not a warning.

Class fields (`EntityHoverClass_data` at `+0x6AC`):

| field | abs | |
|---|---|---|
| `mMaxSuspensionOffset` | `+0xD8C` | float |
| `mMidSuspensionOffset` | `+0xD90` | float |
| `mSuspensionCenterHash` | `+0xD94` | uint, the enable gate |
| `mSuspensionLeftArmHash` / `Right` | `+0xD98` / `+0xD9C` | uint |
| `mBaseSuspensionPos` | `+0xDA0` | PblVector3 |
| `mLeftSuspensionPos` / `mRight...` | `+0xDAC` / `+0xDB8` | PblVector3 |

Instance state (`EntityHover_data` at `+0x494`):

| field | abs | |
|---|---|---|
| `mLastSuspensionOffset` | `+0x4B0` | written by Update, read by nothing found |
| `mNextSuspensionOffset` | `+0x4B4` | the bounce bound |
| `mCurrSuspensionOffset` | `+0x4B8` | vertical displacement, `Max <= curr <= 0` |
| `mSuspensionVelocity` | `+0x4BC` | |
| `mProceduralPoseMatrix[3]` | `+0x1B90` | center, left arm, right arm |

---

## Excitation -- `EntityHover::CollisionCallback` (`0x00541160`)

Runs on an approaching contact (`vn < 0`, alive) that was **not** absorbed by a
`ColliderBody` spring, and only while moving faster than 1% of `ForwardSpeed`
(`speed^2 > 1e-4 * ForwardSpeed^2`):

```c
impulse = dot(matrix.up, sepNormal) * vn;               // vn is the closing speed, negative
inMask  = CollisionMask(mCollisionModel, type 0x80) has receiver id;
mSuspensionVelocity += inMask ? impulse : -impulse;
mNextSuspensionOffset = (mSuspensionVelocity > 0) ? 2*Mid - Max : Max;
```

A hit kicks the spring and sets the bounce bound to the far extreme: fully
compressed (`Max`) or its mirror about `Mid` (which the 0 clamp below turns into
"fully extended").

---

## Integration -- `EntityHover::Update` (`0x00549740`, block ends `0x00549ff3`)

```c
v    += ((curr - Mid) * -205.0f - v * 6.5f) * dt;      // spring to Mid: k = 205, c = 6.5
curr += v * dt;
if (curr > 0)   { curr = 0;   v = 0; }
if (curr < Max) { curr = Max; v = 0; }

if (Next >= Mid) {                                     // bound is in the upper half
    if (Next < curr)            curr = Next;           // crossed it: snap
    else if (v >= 0)            return;                // still rising: nothing
} else {                                               // bound is in the lower half
    if (curr < Next)            curr = Next;
    else if (v <= 0)            return;
}
Last = curr;
Next = 2*Mid - curr;                                   // reflect the bound about Mid
if (v < 0) v = 0;
```

**The spring and damper are compiled in.** `LiftSpring` / `LiftDamp` are the hover
ride and play no part here, despite the stock ODF comment on `LiftSpring` reading
"Suspension tightness". `Next` is a moving bound reflected about `Mid` each time
the body crosses it or reverses, so the motion is a ping-pong bounce whose bounds
converge on `Mid`; the snap means it never overshoots the current bound.

Offsets must satisfy `Max < Mid <= 0`. Anything positive is clamped to 0 on the
first tick.

---

## Drawing -- `EntityHover::Render` (`0x00545e90`)

Each of the three bones has its `mPose` hash-table entry **redirected**, once, to
the matching `mProceduralPoseMatrix[i]` (`Remove` + `Store`, original matrix
copied in). From then on those bones are procedurally driven and any animation on
them is overridden.

- **Center**: rotation is the bind rotation; translation =
  `mBaseSuspensionPos + (0, curr, 0)`. It bobs vertically.
- **Each arm**: translation stays at the arm's bind position;
  orientation = `DirectionMatrix(dir = (-ArmPos.x, curr, 0), up = Y, ArmPos)` with
  an axis shuffle (right = -forward, forward = right; the right arm mirrors x/z).
  The arm pivots in place to point at the displaced center line. Arms are
  optional and independent of each other.

---

## Not resolved

- Modtools carries the string `"Suspension not allowed here"` (`0x00a965e8`,
  modtools) in a string-pointer table at `0x00a95580`. No code reference to that
  slot was found and the table base was not located, so the condition it guards
  is unknown. Retail has no such string.
- Constructor defaults for `Max`/`Mid` when the offset properties are omitted.
  Moot in practice: without `SuspensionNodeName` the hash is 0 and nothing runs.
