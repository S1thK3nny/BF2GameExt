# Procedural animation: what a translation key is relative to, and what LOCAL does

The world-layer procedural animation system (`ProceduralAnimation.cpp`, reached from
`LoadUtil::ProcessWorld` via the `anim` / `anmg` / `anmh` chunks) - the one behind `.ANM` files,
doors, platforms and moving props.

**This is NOT the skeletal system** and shares no code with it. `RedAnimation`, `ZephyrAnimBank`,
`SoldierAnimator` and `.zaabin` banks are a different subsystem entirely; see
`docs/RE/AnimationResearch.md` for those. `EntityPropAnimated` / `EntityBuildingAnimated` are the
other way to animate a world object - Zephyr skeleton driven, ODF configured, no LOCAL concept.

Investigated 2026-08-25 from static analysis across all four builds. Section 5 marks what was
read out of instructions versus what is inferred. Nothing here was confirmed in play.

## 1. Relative to what

**A translation key is relative to the object's own transform captured at load — `mInitialMatrix`, snapshotted when the object's geometry is created — and the XYZ is laid along *that captured transform's* right/up/forward, not world axes.** That is true in **both** modes; it is never the previous key, never world origin, never a parent node. LOCAL does not change the reference point — it changes whether the key is read as an **absolute offset from that anchor** or as a **running integral of key-to-key differences** applied along the object's **currently animated** axes.

Proof, `PAAnimation::CalculateNextMatrix` (modtools `BF2_modtools_MemExt.exe` `0x0056A910`, Steam `BattlefrontII.exe` `0x00627370`, GOG `BattlefrontII_MemExt.exe` `0x00628400`, Phantom `0x0072D4E0`). The caller `PAAnimation::Update` (modtools `0x0056C190`) passes two matrices: `param_4` = `sPAAnims[i].mInitialMatrix` (modtools table base `0x00B81CF0`, stride `0x60`, `mInitialMatrix` at `+0x20`), and `param_5` = a scratch copy of the object's **live** world matrix from `GetMatrix` (vtbl `+0x40`), written back with `SetMatrix` (vtbl `+0x0C`).

The flag test is one byte:

```
modtools 0056a959   MOV AL,byte ptr [EBX + 0x9]     ; mLocalTranslation
         0056a95e   JNZ 0x0056ac7f                  ; -> LOCAL arm
```

Non-LOCAL arm — the live matrix is thrown away and every operand comes from base:

```
0056a9ae   MOVSD.REP ES:EDI,ESI            ; out := base, all 16 floats (ESI=[EBP+0x10], EDI=[EBP+0x14])
0056ab21   MOV EAX,dword ptr [EBP + 0x10]  ; EAX = BASE
0056ab24   FMUL dword ptr [EAX]            ; key.x * base.right.x
0056ab26   LEA ECX,[EAX + 0x30]            ; &base.trans
0056ab98   FMUL dword ptr [EAX + 0x10]     ; key.y * base.up.x
0056abf6   FMUL dword ptr [EAX + 0x20]     ; key.z * base.forward.x
```

> `out = R(t)·base`, then `out.trans = base.trans + kx·base.right + ky·base.up + kz·base.forward`

LOCAL arm — every operand comes from `out`, and `out.trans` is read-modify-write:

```
0056ad37   MOV ESI,dword ptr [EBP + 0x14]  ; ESI = OUT for the rest of the loop
0056b153   FLD [ESP+0xe8] / FSUB [ESP+0x30]  ; delta = sample(t) - prev
0056b17b   MOV EAX,[ESP+0xe8] / MOV [ESP+0x30],EAX   ; prev = sample
0056b1cf   LEA ECX,[ESI + 0x30]            ; &OUT.trans
0056b1d2   FMUL dword ptr [ESI]            ; delta.x * OUT.right.x
0056b24e   FMUL dword ptr [ESI + 0x10]
0056b2af   FMUL dword ptr [ESI + 0x20]
```

Steam `0x00627370` and GOG `0x00628400` are the same compile (SSE, `dt` in XMM1) and produce the same terms.

**Correction to your premise, and it matters for authoring: LOCAL is not per-key.** It is one bool per *animation*, the bare 4th integer of `Animation("name", totalTime, loop, local)`, stored as a whole byte at `PAAnimation+0x09`. It cannot be per-key or per-object, in three independent places: the ZeroEditor `.ANM` key writer and `WorldMunge.exe`'s key parse format (`" %f , %f , %f , %f , %d , %f , %f , %f , %f , %f , %f "` at WorldMunge `0x0041C394`) are exactly 11 fields with no flag slot; the runtime `PABaseKey` is 44 bytes whose only per-key discriminator is the transition enum; and the object binding record is `Animation("<anim>", "<object>");` — two strings. In ZeroEditor it is the **Local Translation** checkbox next to **Loop**, and every object bound to that animation name shares it. If you want per-key LOCAL, split into two animations bound to the same object in the same group.

---

## 2. What LOCAL changes

Two things, both translation-only. Rotation *keys* are `Rx·Ry·Rz(t) · base` — absolute from rest — in **both** arms (modtools `0x0056AAA6` and `0x0056AFD4` both call `D3DXMatrixMultiply` at `0x007CC2F9` with base as `pM2`). Rotation never accumulates.

| | non-LOCAL | LOCAL |
|---|---|---|
| Translation origin | `base.trans`, re-derived every frame | the object's **current** position |
| Translation axis frame | `base`'s right/up/forward (frozen at load) | `out`'s rows = `R(t)·base` if the animation has rotation keys, otherwise the object's **live** axes |
| Key read as | absolute value | first difference `P(t) − P(t−Δ)` |
| Function of time alone | yes, fully scrubbable | no — path-integrated state |
| Object's 3×3 | overwritten from base every frame, rotation keys or not | only written inside the rotation-key branch |

### Worked example — identical keys, both settings

Platform at world origin, rest facing +Z. Duration 5 s, no loop.

```
AddPositionKey(0.00, 0,0,  0,   1, ...);   AddRotationKey(0.00, 0,  0, 0, 1, ...);
AddPositionKey(5.00, 0,0,100,   1, ...);   AddRotationKey(5.00, 0, 90, 0, 1, ...);
```

**Non-LOCAL** — the offset is laid along the *rest* forward the whole time. At t = 5 the object is at **(0, 0, 100)**, yawed 90°. It slides in a dead-straight line down its original Z while spinning in place. The spin does not steer it.

**LOCAL** — each substep it advances 20 units/s along its *current* forward, which is yawing. The path is a quarter arc of radius `v/ω = 20/(π/10) ≈ 63.7`. At t = 5 the object is at **(63.7, 0, 63.7)** — about 90 units from the start in a straight line, having travelled 100 along the arc — yawed 90°.

Same keys, same orientation curve, endpoints **73.3 units apart**. That is the whole flag.

The stock example is `C:\BF2_ModTools\assets\worlds\KAM\world1\kamino1.ANM` — `Animation("Anim1", 310.00, 0, 1)`, one position key of pure +Z 3000 plus 30 rotation keys swinging heading ±280°, bound to `"transport1"`. A transport flying a curved route. Non-LOCAL could not express it.

---

## 3. Practical rules that fall out

**Sub-stepping.** LOCAL integrates in substeps **capped** at 1/60 s (`stepSize` `0x3C88889A` = 0.0166667: modtools `0x00ACEAD4`, Steam `0x007B1EE8`, GOG `0x007B2E60`, Phantom `0x00A981BC`). The loop is `do/while` with `sample = min(cursor, t0+dt)`, so at ≥60 fps there is exactly **one** substep per frame. Multiple substeps only occur on long frames.

**What accumulates, what does not.** `prev` is re-seeded from the curve at the top of every call, so the key *values* telescope exactly — there is no integrated-velocity error. What accumulates is float error in the repeated additions to `out.trans`, plus genuine path dependence from rotating each delta into a different frame. Rotation cannot drift at all; it is recomputed from base.

**Idempotence.** Non-LOCAL is fully idempotent — every frame is `f(t, base)`. LOCAL is not; it is history.

**Loop and end-of-play re-anchor.** One substep condition, **not gated on `mLoop`**: `cursor <= mTotalTime < cursor + stepSize` → `out.trans := base.trans` and `prev := (0,0,0)`.

```
0056ad67   MOV EDX,dword ptr [EBP + 0x10]  ; BASE
0056ad6a   ADD EDX,0x30                    ; &base.trans
0056ad6d   MOV EAX,[EDX] / MOV [ECX],EAX   ; out.trans.x := base.trans.x   (.y, .z follow)
0056ad84   MOV [ESP+0x30],0x0              ; prev := 0  (.34, .38 follow)
```

So a looping LOCAL animation snaps home once per cycle and never wanders. A *finished non-looping* one is clamped at `mTotalTime`, satisfies the condition every subsequent frame, and re-derives `base + key(mTotalTime)` forever — it parks stably and will stomp anything else trying to move it.

**Retrigger and scrubbing.** `SetCurrentTime` (modtools `0x00569EE0`, Phantom `0x0072F9E0`) writes only `*mTimer`; it touches no matrix. `RewindAnimation` = `SetCurrentTime(0)` + `SetEndTime(-1)` (the latter clears an end time left by a prior `PlayAnimationFromTo`, whose own guard silently no-ops unless `0 <= from < to`). Consequence: **rewinding a non-LOCAL animation snaps the object to that pose next frame; rewinding a LOCAL one does not move it at all** — it just replays the same *relative* motion from where the object currently stands. Repeated Rewind + Play walks a LOCAL object across the map, one animation-length per cycle. `PlayAnimation` does not reset the timer either.

**Moving the object by other means.** Non-LOCAL erases it next frame — the whole matrix, including orientation. LOCAL absorbs it permanently: position always, and orientation too if the animation has no rotation keys. Verified at the storage level — for `EntityProp` (modtools) `GetMatrix` is `LEA EAX,[ECX+0xE4]; RET` (`0x004D1140`) and `SetMatrix` (`0x004D1A20`) does a verbatim 16-dword store back to the same `CollisionObject+0xE4` = the entity's own `mMatrix`. No normalisation, no decomposition.

**Collision.** Yes, it follows. `UpdateTreeGrid` (modtools `0x0056BC80`) is called from `PAAnimation::Update` **unconditionally**, even when `CanProcedurallyAnimate` is false, and it re-inserts the object with a refreshed sphere. Animated doors and lifts genuinely block. But it is a **teleport**: no velocity is produced, and nothing carries riders.

**Rebasing.** `SetAnimationStartPoint("group")` → `UpdateStartPositions` (modtools `0x00569F40`) sets `mInitialMatrix := *obj->GetMatrix()` for every binding in the group. For non-LOCAL that visibly relocates the animation. For LOCAL it redefines "home" as wherever the object currently is — including any drift. There is no engine call that pulls a drifted LOCAL object back other than the wrap re-anchor.

**Networking.** Non-LOCAL replicates a quantized timer only. LOCAL additionally ships the transform — `WriteObject` (modtools `0x0056BD20`) tests `*(char*)(anim+9)` and calls `WriteMatrix` (modtools `0x00724210`), which writes a quantized quaternion (4 components) plus quantized position (3), i.e. **7 extra scalars per animated object per update**. The engine's own design choice is independent confirmation of the semantics: non-LOCAL is reconstructible from the clock, LOCAL is not.

---

## 4. Gotchas

0. **The t=0 requirement is the POSITION channel of a LOCAL animation, and nothing else.**
   Refined 2026-08-25 by reading `GetKeys` (modtools thunk `0x00412D9B` -> `FUN_005698D0`).
   It takes a **channel selector**: `param_5 == 0` reads the count/start byte pair at
   `this+4 / this+5`, non-zero reads `this+6 / this+7`. The seeding call at `0x0056ACCD`
   pushes **`param_5 = 0`**, and its two out-buffers are `0x2C` bytes each -- the bracketing
   KEY PAIR of one channel (`PABaseKey` is 44 bytes), not position-and-rotation. So the seed
   queries one channel only.

   Consequences, worth stating because the natural generalisation is wrong:
   - A missing or non-zero **rotation** key at t=0 cannot affect the position `prev`.
   - **Rotation is exempt structurally**, in both modes: it is recomputed as `R(t) . base`
     every frame, so there is no `prev` to seed and nothing to accumulate.
   - **Non-LOCAL has no t=0 requirement at all**, and a non-zero key 0 there is the correct
     way to author a start offset -- place the object where it should END, and let key 0
     carry the offset to where it should begin.
   - Under LOCAL a start offset is not merely fragile but **impossible**: the first cycle
     evaluates to `base + (key(t) - key(0))`, so at t=0 that is `base` whatever key 0 says.

1. **A LOCAL animation must have a position key at exactly t = 0.00.** The LOCAL arm seeds `prev` from `GetKeys(*timer, ...)` before the substep loop (modtools `0x0056ACCD`). `GetKeys` returns **false** when `t` is earlier than the channel's first key, and on that path the `prev` slots at `[ESP+0x30..0x38]` are never written (`0x0056ACD7 JZ 0x0056AD30` skips the only writes; same shape at Phantom `0x0072D9D7` reading `[EBP-0x18]` before the write at `0x0072DA04`). The first substep that *does* resolve keys computes `delta = value − <uninitialised stack>` and the object takes a one-frame jump of arbitrary magnitude. Every stock LOCAL animation starts its position track at 0.00, which is why nobody has hit this. (The separate *delta* accumulator is safely seeded from `gVectorZero` at modtools `0x0056B010`; it is the `prev` slots that are exposed.)

2. **Make that t=0 key `(0,0,0)`, not just present.** On the very first cycle LOCAL yields `base + (key(t) − key(0))`. After the first wrap the re-anchor zeroes `prev`, so it becomes `base + key(t)`. A non-zero key 0 therefore makes the object **shift by key(0) after its first loop** and stay there.

3. **LOCAL with no rotation keys is *not* the same as non-LOCAL during the first cycle, and is fragile afterwards.** Nothing ever writes `out`'s 3×3 in that case, so the deltas ride whatever orientation the object currently carries — including orientation applied by a hierarchy parent or any other system. Non-LOCAL re-imposes base's 3×3 every frame regardless of whether the animation has rotation keys.

4. **Never interleave `AddPositionKey` and `AddRotationKey` inside one `Animation` block.** Both bump the same global cursor `sNextAvailableKey` (modtools `0x00B81AC8`) and each channel stores only `(start, count)`. Writing P R P gives `mNumPosKeys = 2` with slot 1 holding a rotation key — both channels corrupt. All 301 `Animation` blocks across the 66 stock `.ANM` files on disk emit every position key
before every rotation key.

5. **255 keyframes per LEVEL, position and rotation sharing one pool**, and 255 (animation, object) bindings. Both bail silently at `0xFF` with no warning logged. 32 hierarchies (explicit check). Names truncated at 128 chars and matched by hash.

6. **Spline tangents both come from the LEFT key of the segment**, and the segment's transition mode comes from the left key too. In `AddPositionKey(t,x,y,z,2, t0…, t1…)` the first triple is the outgoing tangent at this key and the second is the incoming tangent at the *next* key. The right key's own slopes are ignored for that segment.

7. **If your last key's time is ≥ `mTotalTime`, the wrap segment is evaluated over a hard-coded 1.0 s gap.** The span rule is `gap = (k0.t < k1.t) ? k1.t − k0.t : (k0.t < mTotalTime ? mTotalTime − k0.t : 1.0f)`. `mTotalTime` is the true loop period, and the last key blends back to key 0 over whatever time is left.

8. **A channel whose first key is later than t = 0 contributes nothing before that time**, and the two channels can start at different times.

9. **Rotation keys in `.ANM` are DEGREES.** `WorldMunge.exe` converts to radians; the game feeds the stored floats straight into `D3DXMatrixRotationX/Y/Z`. Position keys are passed through unscaled — `WorldMunge`'s `AddPositionKey` handler (`0x00403876`–`0x004039B3`) contains **zero** floating-point instructions, while the `AddRotationKey` handler tail (`0x00403731`–`0x00403828`) has fifteen unconditional `fmul dword ptr [0x0041D8BC]` where that constant is `0x3C8EFA35` = π/180, the only occurrence in the binary.

10. **"Stops when object is controlled" (3rd arg of `AnimationGroup`, bit 0 of group `+0x12`) is not just a pause.** When it trips it zeroes the timer **and** re-captures `mInitialMatrix` from the object's live matrix every frame. The gate is `GameObject+0x234 & 0xF` non-zero — the low nibble, which `GameObject::SetTeam` (Phantom `0x005D65F0`, `SHL EAX,0x1C / SAR EAX,0x1C`) reads and writes, i.e. **`mTeam`**. Only stock use is `assets\worlds\SPA\world2\spa2_Turrets.ANM` (`AnimationGroup("Idlehover", 1, 1)`) — hangar craft bob while unclaimed and stop, re-anchored where they stand, once a team owns them.

11. **Hierarchies are the true "parent node" case and are completely orthogonal to LOCAL.** `mLocalMatrix = childWorld · parentWorld⁻¹` captured at load; each frame `child = mLocalMatrix · rootCurrent`, then `UpdateTreeGrid(child)` — and that runs *outside* the animate/skip branch, so children follow even when the team gate has frozen the root. Disabled per group by `DisableHierarchies()` / the `NOHI` chunk (bit 1 of group `+0x12`). No hierarchy code reads `mLocalTranslation`.

12. **This is not the skeletal system and shares no code with it.** `ProceduralAnimation.cpp` (source string modtools `0x00A441B8`, stamp `Feb 9 2006 15:23:54`) is its own translation unit reached from `LoadUtil::ProcessWorld` via the `anim` / `anmg` / `anmh` chunks — the same stream as `inst`, `regn`, `BARR`. Its complete callee set is D3DX matrix helpers, `PblFile`/chunk readers, the hash table, `NetPktGroup`, and four `CollisionObject` vfuncs (`+0x0C SetMatrix`, `+0x10 CanProcedurallyAnimate`, `+0x38 GetGameObject`, `+0x40 GetMatrix`). No bones, no skeleton, no blending, no `.zaabin`. The one name collision, `SoldierAnimator::ApplyProceduralAnimationAndBuildWorldMatrices` (Phantom `0x007518A0`), is skeletal IK and calls nothing `PA*`. `EntityPropAnimated` / `EntityBuildingAnimated` are the *other* way to animate a world object — Zephyr skeleton driven, ODF configured, no LOCAL concept at all. Different manual, too: `procedural animation mode.doc` vs `animation_guide.doc`, both shipped in `C:\BF2_ModTools\documentation`.

---

## 5. Confidence

**VERIFIED** (instructions read on the named build)

- `mLocalTranslation` at `PAAnimation+0x09`, one bool per animation, written only by the ctor (modtools `0x005697BD`) and `SetAnimationInfo` (modtools `0x005697F0`), read in exactly three places: `CalculateNextMatrix`, `WriteObject` (`0x0056BDDA`), `ReadObject` (`0x0056BF30`). Exhaustiveness proven via the six xrefs to the animation hash table `0x00B81ADC`, since any consumer must first obtain the object.
- Both arms of `CalculateNextMatrix`, including operand provenance (base vs out), on modtools `0x0056A910` and Steam `0x00627370`; GOG `0x00628400` byte-identical prologue to Steam.
- The re-anchor at modtools `0x0056AD67` and its condition, ungated by `mLoop`.
- Rotation = `R(t)·base` in both arms.
- `stepSize` = 1/60 as a cap, raw bytes on all four builds.
- The full authoring chain: ZeroEditor checkbox (`editor.animation.localtranslation`, anim record `+0x35`) → 4th `%d` of `Animation("%s", %.2f, %d, %d)` → WorldMunge record `+0x0D` → second `Write8` of the anim `INFO` chunk → second `PblFile::Read8` → `SetAnimationInfo` param 3 → `+0x09`. Loop is the 3rd integer via `+0x34` / `+0x0C` / `+0x08`.
- No per-key or per-object flag is expressible in the text grammar, the ZeroEditor writer, the ZeroEditor key UI id list, or the 44-byte `PABaseKey`.
- Degrees→radians on rotation keys only, in WorldMunge; unconditional, not gated on the flag.
- `GetMatrix`/`SetMatrix` round-trip to `CollisionObject+0xE4` verbatim.
- `mInitialMatrix` has exactly four writers in modtools (`AddAnimation` identity init, `GeometryCreated` `0x0056B9F0`, `UpdateStartPositions` `0x00569F40`, the team-gate path in `UpdateAnimations` `0x0056C33D`), proven by xref sweep of the entry-0 field plus the absence of xrefs to a mid-table entry.
- `GameObject+0x234 & 0xF` = `mTeam`, via `GameObject::SetTeam` (Phantom `0x005D65F0`). Offset identical in all four builds.
- `UpdateTreeGrid` called unconditionally.
- 255-key, 255-binding, 32-hierarchy caps as enforced checks.
- The uninitialised-`prev` code path (modtools `0x0056ACD7`, Phantom `0x0072D9D7`).

**INFERRED**

- The in-game *symptom* of the uninitialised-`prev` path. The path is verified; I did not run it.
- `NetCollisionObject::CanProcedurallyAnimate` returning true only outside a network session — read only on Phantom (`0x00686E20`); base `CollisionObject::CanProcedurallyAnimate` = `return true` (Phantom `0x0048C9C0`). The call site is verified on modtools. Ordinary `EntityProp`/`EntityBuilding` use the base version, so this does not affect normal props — but do not act on the Phantom reading for net-authoritative objects.
- The negative-`dt` guard at the top of `CalculateNextMatrix` (modtools `0x0056A947`, Steam `0x00627379`, absent in Phantom) appears **inert in the shipping builds**: Steam's global `0x01EFCF38` has one writer and it is a zero-init; modtools' `0x00B87CB4` has no writer at all, and the companion accumulator `0x00B87CB0` has no consumer. So `PAManagerInterpolator`'s rewind-and-replay of the whole PA system (Phantom `0x0072CFE0`) looks like a 2026-rebuild feature — meaning LOCAL is **not** being integrated backwards on turn boundaries in the builds you ship on. A `this`-relative write could in principle hide a writer, so treat this as strongly indicated rather than proven.
- The authoring *name* "stops when object is controlled" comes from the third-party WorldEdit reimplementation, not Pandemic. The mechanism is verified; the name is not.
- 32-entry hash tables for animation names and groups: the sizes are certain, the behaviour on overflow (`PblHashTableCode::_Store` on a full table) is not traced.
- Spline slope units — the 15 π/180 multiplies all sit inside the `AddRotationKey` block and none in the `AddPositionKey` block, which is proven; the mapping of specific multiplies to `mVal` vs `mSlope0` vs `mSlope1` is not. "Rotation slopes are degrees/sec" is strongly indicated.

**UNKNOWN**

- No `_lvl_pc` output exists on this machine, so nothing was read from a munged `anim` chunk on disk; the degrees→radians conclusion rests on the WorldMunge constant analysis, which is solid but is tool-side rather than end-to-end.
- Nothing here was confirmed empirically in play. Every statement is static analysis.


## Corrections and omissions found 2026-08-25

An adversarial pass re-checked 25 semantic assertions in this document against the binaries,
including on the **shipping** modtools build rather than only Phantom. **All 25 held**, and three
got stronger (base/out provenance is now proven at the call site via
`PAAnimationGroup::UpdateAnimations` `0x0072FE8C`; the span rule is confirmed ungated on `mLoop`
on both builds; the Hermite span-scaling of both tangents is proven algebraically).

Three things this document omitted:

1. **`runtime = 0` with `loop = 1` is a hard hang** - three separate unguarded loops of the shape
   `for (; total < t; t -= total)` with `total == 0`. WorldEdit creates new animations with
   `runtime = 0.0f` and leaves Loop a free checkbox, so this is one click away.
2. **"Snaps home once per cycle" is imprecise above 60 fps.** The re-anchor window has fixed
   width `stepSize` while the cursor advances by `dt`, so above 60 fps the condition is true on
   several consecutive frames.
3. **`ReadHierarchy` reads its node count with a signed `Read8` tested as `1 < cVar3`**, so a
   hierarchy declaring more than 127 members is silently dropped.
