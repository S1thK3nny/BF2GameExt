# WorldEdit 0.161: is its procedural animation faithful to the engine?

Compared 2026-08-25 against the verified engine semantics in
[ProceduralAnimation.md](ProceduralAnimation.md). WorldEdit source at
`C:/BF2_ModTools/WorldEdit/Source/WorldEdit-0.161`.

Three comparison passes plus three adversarial passes: one attacking every DIVERGES
verdict, one attacking every FAITHFUL verdict, one attacking OUR OWN engine reading.
The second overturned two verdicts the first three passes had all marked faithful.
The third failed completely - see section 5.

All static analysis. Nothing was run in game or in the editor.

---

# Does WorldEdit 0.161's animation preview tell you the truth?


> ## DISPUTED 2026-08-25 — item 6 / section 3.1 (coordinate space) is probably a FALSE POSITIVE
>
> The project owner reports that non-LOCAL animations play the same in WorldEdit and in game.
> That is field evidence against this finding and it likely wins. Two reasons to distrust the
> analysis:
>
> 1. **The mechanism and the predicted correction disagree.** `y_flip` is
>    `{-q.y, -q.z, q.w, q.x}`, documented in `quaternion_funcs.hpp:34` as `quat * {0,0,1,0}` — a
>    180 degree YAW applied after the stored rotation, determinant +1. The report's algebra
>    produced `(-kx, ky, kz)`, a MIRROR in X, determinant -1. If `y_flip` were the convention the
>    keys should use, the implied correction would be `(-kx, ky, -kz)`. The analysis is internally
>    inconsistent about its own mechanism.
> 2. **It would be impossible to miss.** `y_flip(identity)` is a 180 degree yaw, so the divergence
>    would show on any non-trivial key even for an axis-aligned object, not only on the exotic
>    cases the report highlighted.
>
> The reading that fits the observation: **`y_flip` is a MODEL-FACING correction applied at draw
> time** — `.msh` geometry faces opposite to WorldEdit's convention — not a world mirror.
> Animation keys live in the object's logical frame, which is the unflipped `object.rotation`.
> On that reading WorldEdit is right and the animation editor is the odd one out for a good reason.
>
> **What survives regardless:** `world_edit_ui_animation_editor.cpp:1832` really does use
> `normalize(object.rotation)` where ~18 other consumers use `y_flip(object.rotation)`. Whether
> that is a bug or correct depends entirely on what the flip is FOR, which was never established.
>
> Items 2, 3, 4, 5, 7, 8 and 10 do not depend on this and are unaffected. Item 9 (WorldMunge
> double-scaling rotation tangents) was disassembled directly and is independent.


## 1. Verdict

**Not faithful.** The preview is trustworthy for the *shape* of simple motion and nothing else: it moves things in the wrong direction, it lies about the end of every non-looping local animation, it draws spline rotation tangents the game throws away, and on 58 of the 170 stock animations it evaluates to NaN at the end of the timeline.

---

## 2. Item-by-item

| # | Item | Verdict | What you see |
|---|---|---|---|
| 1 | Composition order / matrix convention | **FAITHFUL** | `transform * delta_transform` composes in the object's local frame, exactly like the engine. One half-substep phase lag, sub-unit, shrinks with step count. |
| 2 | Rotation accumulation | **DIVERGES** | Single-axis rotation is exact. Multi-axis LOCAL rotation drifts up to ~60° on shipped content, and finer stepping does not fix it. |
| 3 | Substepping | **DIVERGES (usually small)** | 0.1–0.3% in-flight error on well-behaved stock loops. Blows up only in shapes that appear zero times in stock content but are trivially authorable. |
| 4 | Wrap re-anchor | **DIVERGES (severe)** | Non-looping LOCAL animations snap thousands of units at the end in game; the preview cannot show that pose at all. Looping ones go NaN over the last 10–20% of the timeline. |
| 5 | `last_position` / `last_rotation` | **DIVERGES-BUT-HARMLESS** | Not uninitialised. Zero-initialised, which encodes the engine's *post-wrap* semantics. Identical to the engine for every correctly-authored animation; it hides the engine's worst landmine rather than reproducing it. |
| 6 | Non-LOCAL fidelity | **DIVERGES (severe)** | The maths is right; the coordinate space is not. Position keys are mirrored in X and rotation keys reversed in Y and Z relative to the game. |
| 7 | Units / round trip | **DIVERGES** | Degrees-in-model is correct. Rotation *tangents* reach the game at 1.7% strength. Save rewrites every line ending, quantises 3 dp to 2 dp, and reorders animation-group entries. Per-layer `.ANM` files are invisible. |
| 8 | Segment span rule | **DIVERGES** | The normal wrap is emulated correctly. The engine's hard-coded 1.0 s fallback is missing, and its absence is the NaN in item 4. |
| 9 | Spline tangents from the left key | **FAITHFUL in sourcing and basis; outcome DIVERGES for rotation** | Bit-identical Hermite, both slopes and the mode from the left key. But WorldMunge double-scales rotation slopes, so the game plays a flat smoothstep where the preview draws your curve. |
| 10 | Authoring guard rails | **DIVERGES** | Only the position/rotation ordering constraint is enforced (accidentally). Every engine cap is absent or set 64–512× too high, and the default "add a key" flow manufactures the item-4 NaN. |

---

## 3. The divergences that actually matter

### 3.1 Everything moves in the wrong direction — position X, rotation Y, rotation Z

**What is wrong.** WorldEdit's world is the game world mirrored in Z, with an extra 180° local yaw baked into every object. That transform is applied to object transforms and to nothing else. `load.cpp:104-121` (`read_rotation`) and `load.cpp:79-102` (`read_location`) flip objects; `load.cpp:1113-1153` (`load_animations`) copies key values verbatim:

```cpp
.position = {child_key_node.values.get<float>(1),
             child_key_node.values.get<float>(2),
             child_key_node.values.get<float>(3)},
```

No sign change, no swap — contrast `load_measurements` twenty lines further down, which *does* negate Z. `save.cpp:787-798` writes them back raw. Then `animation.cpp:164` mixes the two conventions in one expression:

```cpp
position += base_rotation * local_position;
```

Verified over 4000 random unit quaternions: `max|R_we − Mz·R_g·Mx| = 4.4e-16`, where `Mz = diag(1,1,−1)` and `Mx = diag(−1,1,1)`. So WorldEdit's local axes relate to the game's as `right → −right`, `up → up`, `forward → forward`. The engine's own axis semantics come straight out of `CalculateNextMatrix` (Phantom `0x0072D4E0`):

```c
(param_4->trans).x = mVal.x*(param_3->right).x + (param_3->trans).x
                   + mVal.y*(param_3->up).x + mVal.z*(param_3->forward).x;
```

**The correction, if you want to reason about it by hand:**

| `.ANM` key | what WorldEdit *should* use | what it uses |
|---|---|---|
| position `(kx, ky, kz)` | `(−kx, ky, kz)` | `(kx, ky, kz)` |
| rotation `(rx, ry, rz)` | `(rx, −ry, −rz)` | `(rx, ry, rz)` |

The rotation signs follow from conjugating by `diag(−1,1,1)`, and because conjugation distributes over a product **the answer does not depend on the engine's Euler ordering**.

**What you see.** Base at origin, identity rotation:

- position key `(0,0,100)` — exact, 0.00 error
- position key `(0,100,0)` — exact
- position key `(100,0,0)` — **200 units off**, the sliding door opens the wrong way
- rotation key `(90,0,0)` (pitch) — 0.0°
- rotation key `(0,90,0)` (yaw) — **180°**, the turret tracks the wrong way
- rotation key `(0,0,90)` (roll) — **180°**
- rotation key `(30,45,60)` — **138.6°**
- the doc's worked LOCAL example (100 forward while yawing 0→90) — **127 units and 180° of heading**

Stock exposure: **139 position keys with a non-zero X and 252 rotation keys with a non-zero Y or Z**. `spa1` `turretevac` (key `(−9000, 9000, 9000)`) previews 18 000 units from where it goes. `hoth` `Escape` is 4000 units off in X with its yaw reversed. Every yawing LOCAL route — `rep_loop`, `MedFrig*`, `simpleloop*`, `flag_loop` — curves the wrong way; because they are closed loops the shape still looks plausible, which is why this has survived unnoticed.

**And it is not confined to the preview.** The movement gizmo (`world_edit_ui_animation_editor.cpp:1922-1941`) writes `key.position + movementWS` through `base_rotation`, and the rotation gizmo (`:1960-1975`) sets `.gizmo_rotation = base_rotation`. Dragging a key in the viewport writes a sign-flipped X into the file. Load and save are symmetric, so a plain round trip is safe; it is *authoring through the gizmo* that corrupts.

**What to do.** Author position keys with X = 0 and rotation keys as pure pitch wherever you can — those are the two cases that come out right, which is exactly why the bug is invisible on a first test. Otherwise, negate X on positions and Y/Z on rotations in your head, and verify in game before you commit. If you have already dragged keys with the gizmo on a rotated object, treat those values as suspect.

**The one open link.** That `.wld` `Rotation(w,x,y,z)` is object→world under standard Hamilton is inferred, not read from an instruction. Everything downstream of it is read from binaries. The conjugate hypothesis is excluded structurally (it would make `R_we = Mz·R_gᵀ·Mx`, anti-homomorphic in `R_g`, so the editor would be object-dependently wrong, and it plainly is not). The empirical test costs thirty seconds: author one key `(100,0,0)` and one `(0,90,0)`, munge, compare.

---

### 3.2 The preview goes NaN at the end of most looping animations — and the editor creates the condition for you

**What is wrong.** WorldEdit emulates the engine's loop wrap by cloning key 0 and stamping the runtime onto it (`animation.cpp:143`, `:279`, `:401`, `:447`, `:510`):

```cpp
b.time = animation.runtime;
```

then divides by the span (`animation.cpp:17`):

```cpp
const float local_t = (global_t - a_time) / (b_time - a_time);
```

with no guard. The engine has one. `PAAnimation::InterpolateKeys` (Phantom `0x0072ED20`, and `FUN_00569AE0` on modtools):

```c
fVar2 = param_3 - param_1.mTime;  fVar3 = param_2.mTime;
if ((param_1.mTime < param_2.mTime) ||
   (fVar3 = mTotalTime, fVar1 = 1.0f, param_1.mTime < fVar3)) { fVar1 = fVar3 - param_1.mTime; }
```

All three branches yield a strictly positive span. **The engine's span rule *is* the divide-by-zero guard, and WorldEdit ported the wrap without it.** When the last key sits exactly on `runtime` the engine uses a hard 1.0 s and evaluates at `u = 0`; WorldEdit computes `0/0`.

**What you see.** A NaN transform for everything from the wrap onward — the NaN poisons `last_position`, so every subsequent delta and every subsequent `transform` is NaN too. Measured on stock content:

| animation | file | NaN from |
|---|---|---|
| `flag_loop` | spa6 | t = 180.0 (runtime 180) |
| `rep_loop` | spa6 | t = 229.9 of 256 |
| `cis_loop` | spa6 | t = 250.0 of 260 |
| `MedFrig02`, `MedFrig03` | spa3 | t = 199.9 of 240 |
| `simpleloop01`, `simpleloop02` | spa1 | t = 60.0 of 60 |
| `MedFrig01` | spa3 | wrap block emits nothing, but `evaluate()` itself NaNs at t = runtime |

**8 of the 9 stock looping LOCAL animations**, and **58 of the 170 stock animations with runtime > 0** return a non-finite transform at `t == runtime` — this is not a LOCAL-only problem, the same `0/0` sits in the non-LOCAL evaluator at `animation.cpp:138-158`.

You do not have to scrub to hit it. `world_edit_ui_animation_editor.cpp:1984-1999` evaluates the solver **at every key's time** to draw the key markers, and `raycast_animation.cpp:19` / `:58` do the same for picking. Selecting one of those animations is enough.

**The editor manufactures the condition.** `world_edit_ui_animation_editor.cpp:1691-1702` and `:1748-1759`:

```cpp
if (_animation_editor_context.selected.new_position_key_time >
    selected_animation->runtime) {
   _edit_stack_world.apply(edits::make_set_value(&selected_animation->runtime,
       _animation_editor_context.selected.new_position_key_time), _edit_context);
```

Adding a key past the end sets `runtime := that key's time`. The default way to extend an animation produces the exact configuration that NaNs. The Runtime drag-drop target (`:602-616`) is a second route to it.

Two aggravating details: the wrap emits up to 1440 subkeys all stamped at (or a float-ulp either side of) `runtime`, so `_timepoints` is **not sorted** and `std::lower_bound` at `animation.cpp:180` is running on an unordered range; and the loop-closure visualiser at `:2043-2067` has no zero-length guard while the non-loop branch at `:2068-2080` explicitly tests `time_distance > 0.0001f` — the guard exists, it just was not copied into the loop branch.

**What to do.** Keep the last key strictly before `runtime`. After adding your final key, bump `runtime` up by 0.01. This is engine-legal either way — the game handles the degenerate case cleanly — so this is purely to keep the preview alive.

---

### 3.3 The game ignores your spline rotation tangents. The preview draws them in full.

**This is new and it flips a verdict two earlier passes had marked FAITHFUL.** `WorldMunge.exe` does not scale rotation keys uniformly. It scales **values once and tangents twice**.

Verified at the byte level this session in `C:\BF2_ModTools\ToolsFL\bin\WorldMunge.exe` (PE32, image base `0x400000`). There is **exactly one** occurrence of the float `0x3C8EFA35` (π/180) in the entire file, at `0x0041D8BC`, and **exactly 15** references to it, all `fmul dword ptr [0x41d8bc]`, all between `0x00403739` and `0x00403816`. A uniform degrees→radians conversion of `mVal`(3) + `mSlope0`(3) + `mSlope1`(3) would be **nine** multiplies. Fifteen is 3 + 6 + 6, and the instruction stream says which is which:

```
00403731  fld   dword [esp+0x28]        ; mSlope1.x
00403739  fmul  dword [0x41d8bc]        ; *K
00403743  fld   dword [esp+0x30]        ; mSlope1.y
0040374e  fmul  dword [0x41d8bc]        ; *K
0040376a  fstp  dword [esp+0x60]        ; spill  mSlope1.y*K
00403771  fld   dword [esp+0x38]        ; mSlope1.z
0040377c  fmul  dword [0x41d8bc]        ; *K
00403782  fstp  dword [esp+0x64]        ; spill  mSlope1.z*K
00403786  fmul  dword [0x41d8bc]        ; mSlope1.x * K * K      <-- twice
0040378c  fld   dword [esp+0x60]
00403790  fmul  dword [0x41d8bc]        ; mSlope1.y * K * K      <-- twice
00403796  fld   dword [esp+0x64]
0040379a  fmul  dword [0x41d8bc]        ; mSlope1.z * K * K      <-- twice
```

The identical spill-reload-remultiply shape repeats at `0x004037a7`–`0x004037e7` for `mSlope0` (args `[esp+0x10]/[0x18]/[0x20]`). `mVal` (args `[esp+0x34]/[0x1c]/[0x2c]`) is scaled once, at `0x00403800`, `0x0040380a`, `0x00403814`. The `AddPositionKey` branch contains **zero** fmuls, which is what pins the argument-to-slot map — and means position keys and position tangents pass through unscaled, so position splines are fine.

The engine consumes the slopes raw: `PblHermiteVector3::Set` (Phantom `0x0041644B`) sets `_c = m0` with no renormalisation, and `InterpolateKeys` passes `&param_1.mSlope0, &param_1.mSlope1` straight in. There is no second scaling anywhere.

Since Hermite is linear in `p0/p1/m0/m1`:

```
engine(t)   = h00·p0 + h01·p1 + K·(h10·T·m0 + h11·T·m1)
worldedit(t)= h00·p0 + h01·p1 +   (h10·T·m0 + h11·T·m1)
```

**The tangent term reaches the game at 1.745% strength.** A spline rotation segment in game is effectively a zero-tangent smoothstep between the two key values, no matter what you author.

**What you see.** Across the 28 stock `.ANM` files there are 43 spline rotation segments with non-zero tangents; **33 diverge by more than 1°, 15 by more than 10°**:

- `spa1.ANM` `fan_stop`, key at t=8.0, sampled t=8.66: preview yaw **−5279.87°**, game **−5134.32°** — 145.55°
- `kamino1`/`myg1` `FIG8`, key at t=0, sampled t=3.81: preview **76.77°**, game **2.24°** — 74.5°
- `kamino1` `Anim1` (LOCAL), key at t=300, sampled t=303.3: 33.5° of rotation error, which then drags the integrated position with it

This mis-renders shipped Pandemic content the moment you open it.

**What to do.** Do not use spline rotation tangents. The game will not play them. If you want eased rotation, add more keys, or use `spline` with the auto-tangents left at zero and accept the smoothstep — which is what the game gives you regardless. Position splines are safe.

---

### 3.4 Non-looping LOCAL animations park somewhere the preview never shows

The engine's wrap re-anchor is **not gated on the loop flag**. Confirmed on both Phantom (`0x0072D4E0`) and modtools (`FUN_0056A910`), neither of which reads the `mLoop` byte anywhere in the block:

```c
if ((local_98 <= mTotalTime) && (mTotalTime < local_98 + stepSize)) {
   local_1c = 0.0; local_20 = 0.0; local_18 = 0.0;          /* prev := 0 */
   (param_4->trans).x = (param_3->trans).x; ... .w = 1.0;    /* out.trans := base.trans */
}
```

For a finished non-looping animation the timer is clamped to `mTotalTime`, so this condition is true **every frame thereafter**. The whole integrated path is discarded and re-derived as `base.trans + lastKey.pos` laid along the *final rotated* axes.

WorldEdit's ladder is gated on `animation.loop` at every branch (`animation.cpp:41`, `:178`, `:223`, `:274`), so it never re-anchors; playback simply stops and resets `playback_time = 0.0f` (`:2247-2257`). **The parked pose is unreachable in the preview.**

Measured against a 60 fps engine integrator over the stock LOCAL set (base = identity, so this isolates items 2 and 4 from item 3.1):

| animation | runtime | path length | worst in-flight error | park error at t = runtime |
|---|---|---|---|---|
| `spa2_turrets` `Frig01` | 600 | 119 210 | 12 281 (10.3%) | **20 739** |
| `hoth` `Escape` | 20 | 12 090 | 25.6 (0.2%) | **3 826** |
| `kamino1` `Anim1` | 310 | 5 587 | 216 (3.9%) | **2 554** |
| `spa2` `repshipmove01` | 120 | 6 421 | 57.8 (0.9%) | **1 903** |
| `spa1` `trans03` | 210 | 5 288 | 1 137 (21.5%) | **1 836** |
| `spa5` `flight04` | 360 | 5 263 | 1 177 (22.4%) | **1 268** |
| `spa1` `Trans02` | 210 | 4 541 | 2.8 (0.1%) | **546** |

The near-zero in-flight rows are exactly the single-axis animations; the large ones are item 3.5 below, not substepping.

**What to do.** Assume every non-looping LOCAL animation ends by teleporting to `base + lastKey.pos` expressed in the final rotated frame. If that matters, make your last position key the pose you actually want it parked in, or use a looping animation and gate it elsewhere.

---

### 3.5 Multi-axis rotation drifts in LOCAL mode, and more substeps do not help

The engine sets rotation **absolutely** every substep — `out.3x3 := Rx·Ry·Rz(t) · base`, recomputed from base, with `out.trans` saved and restored around the write (`local_90`/`local_94`/`local_2c` in the Phantom decompile). WorldEdit accumulates Euler *first differences* (`animation.cpp:299-309`, mirrored at `:423-433` and `:532-542`):

```cpp
float3 delta_rotation = rotation - last_rotation;
float4x4 delta_transform = make_rotation_matrix_from_euler(delta_rotation * degrees_to_radians);
delta_transform[3] = float4{delta_position, 1.0f};
transform = transform * delta_transform;
```

Composing rotations built from Euler differences converges to the path-ordered exponential of the Euler *rate*, which is a different curve from the absolute Euler curve. At WorldEdit's own step count and at 8× that count:

| rotation key | steps | vs. absolute | at 8× steps |
|---|---|---|---|
| `(0,90,0)` / `(90,0,0)` / `(0,0,90)` | 360 | **0.00°** | 0.00° |
| `(10,20,30)` | 120 | 6.24° | 6.20° |
| `(0,45,45)` | 180 | 17.18° | 17.10° |
| `(30,45,60)` | 240 | 30.95° | 30.89° |

**Not quadrature error — it is the wrong algebra, and refinement does not touch it.** On stock content the measured rotation error reaches **59.6°** (`spa2_turrets` `Frig01`) and **48.1°** (`kamino1` `Anim1`).

There is a second, smaller problem underneath: WorldEdit's two halves use different Euler orders. `make_quat_from_euler` (`quaternion_funcs.hpp:132`) is `Rz·Ry·Rx` — **exactly the engine's** `D3DXMatrixRotationX·Y·Z` in row-vector form. `make_rotation_matrix_from_euler` (`matrix_funcs.hpp:83`), used only by the LOCAL path, is `Ry·Rx·Rz`. Verified numerically: `|WEquat − engine| = 0.000000000` on every key tried; `|WEmat − engine|` up to 0.79 per element. In practice this order error is second-order in the delta size and *does* wash out with substep count, so what you actually see is the accumulation above — but it means the same rotation key renders differently in the two evaluators.

**What to do.** Keep LOCAL rotation to one axis. Single-axis is exact, and it is what almost all hand-authored content uses anyway. If you need a multi-axis LOCAL banking route, the preview's heading is not the game's.

---

### 3.6 The Place tool writes a world-space delta into a local-frame slot

`world_edit_ui_animation_editor.cpp:2316-2321`:

```cpp
world::position_key new_key =
   {.time = _animation_editor_context.selected.new_position_key_time,
    .position = _cursor_positionWS - base_position};
```

No inverse `base_rotation`. The engine reads a position key as `base.trans + kx·right + ky·up + kz·forward`, and WorldEdit's own evaluator does `position += base_rotation * local_position` (`animation.cpp:164`). So a placed key lands at `base_position + base_rotation * (cursorWS − base_position)` — not under the cursor — for any animated object whose orientation is not identity. The two guide lines have the same omission (`:2297-2299`, `:2304-2306`), while the ordinary key visualisers apply the rotation correctly (`:2097-2101`, `:2136-2140`). A self-inconsistency inside one file, and it stacks on top of 3.1.

Place is already disabled for LOCAL animations with an explanatory tooltip (`:1022-1035`); it is non-LOCAL animations on rotated objects where this bites.

---

### 3.7 No engine limits are modelled, and one configuration hangs the game

`grep '\b255\b'` over `src/world/` and the animation UI returns nothing but colour constants and mesh indices. `world.hpp:37-39`:

```cpp
constexpr std::size_t max_animations = 16'384;
constexpr std::size_t max_animation_groups = 16'384;
constexpr std::size_t max_animation_hierarchies = 16'384;
```

| engine constraint | source | WorldEdit |
|---|---|---|
| 255 keyframes per **level**, position and rotation sharing one pool | `AddPositionKey` `0x0072D3E0` / `AddRotationKey` `0x0072D460`, `if (sNextAvailableKey == 0xff) return false;`, same global cursor | no check at all — plain `std::vector` |
| 255 (animation, object) bindings | `AddAnimation` `0x0072D2F0` | 16 384, and per-group not per-binding |
| 32 hierarchies | `ReadHierarchy` `0x0072F510`, `if ((1 < cVar3) && (mNumHierarchies != 0x20))` | 16 384 — **512× over** |
| 128-char names | `char local_9c[128]` in `ReadHierarchy` | uncapped `InputText` |
| LOCAL needs a position key at t=0.00, value (0,0,0) | `GetKeys` seeding hole | no check, no warning; the `Local Translation` checkbox at `:622` has no tooltip at all |
| `runtime = 0` with `loop = 1` **hangs the game** | three unguarded loops: the non-LOCAL `do { fVar18 -= fVar19; } while (fVar19 < fVar18)`, its LOCAL twin, and `GetKeys`'s `for (; fVar1 < param_1; param_1 -= fVar1)` | new animations start at `runtime = 0.0f`, the Runtime drag's minimum is `0.0f`, Loop is a free checkbox. Preview gives NaN; the game spins forever. |

All bails are **silent** in the engine. You will not get an error; the animation simply will not be there.

**What to do.** Budget keys per *level*, not per animation, and stay under 255 across every animation in the world. Never ship `runtime = 0` with loop ticked. Count hierarchies by hand.

Two smaller UI bugs worth knowing: the "Add Position Key at Time" context menu item at `world_edit_ui_animation_editor.cpp:1147-1157` actually sets `add_rotation_key = true` (the mirror-image item at `:943-951` is correct, so it is a copy-paste slip), which bypasses the rotation channel's own uniqueness check and can produce duplicate rotation times — NaN in the LOCAL precompute for spline transitions, and in game a zero gap that the span rule stretches across the rest of the animation. And `convert_to_smooth_spline`'s tooltip says "all position keys" (`:653-656`) while the function also converts every rotation key (`:181-196`), which silently flips the LOCAL substep heuristic and changes in-game rotation interpolation.

---

### 3.8 Per-layer `.ANM` files are invisible

`load_animations` is called only under `if (layer == 0)` (`load.cpp:1293`, call at `:1314-1316`); `save_animations` runs once at `save.cpp:1228`, before the per-layer loop; `garbage_collect_files` (`save.cpp:1177-1188`) lists only `.lyr .pth .rgn .lgt .hnt`.

| world | loaded | invisible |
|---|---|---|
| `SPA/world2` | `spa2.ANM` — 11 animations, 7 groups | `_Fighters`, `_Turrets`, `_capitalShips` → **8 of 19 animations, 6 of 13 groups** |
| `DEA/world1` | `dea1.ANM` — 20 animations | `dea1_animations.ANM` — 5 animations, 2 groups |
| `SPA/world7,8,9` | base only | one `_capitalShips`/`_Fighters` each |

`spa2_Turrets.ANM` holds `AnimationGroup("Idlehover", 1, 1)` — the only stock use of "stops when the object is controlled" — and WorldEdit cannot see it. They are not deleted, so they survive a save; you just cannot edit them.

---

### 3.9 The round trip is not byte-identical

Every field round-trips exactly — all 174 stock `Animation()` lines have 4 args, all 901 key lines have 11 fields, and every one of the 9010 numeric tokens is already at 2 dp, so `{:.2f}` reproduces each one. Three things still change:

- **Line endings.** `src/io/output_file.cpp:16` is `constexpr char linebreak_char = '\n';` and `write_ln` appends exactly that, through a raw `CreateFileW`/`WriteFile` with no CRT text translation. All 28 stock `.ANM` files are 100% CRLF (verified: `spa6.ANM` head is `Animation("flag_loop", 180.00, 1, 1)^M$`). Every line in the file changes on save.
- **3 dp → 2 dp on values.** `DragFloat3` resolves to `src/imgui_ext.cpp:126` with per-component format `"X:%.3f"`, and `DragFloat("Runtime", …)` uses the wrapper default `"%.3f"` (`src/edits/imgui_ext.hpp:57`). Key *times* stay at 2 dp — every time widget passes `"%.2f"` explicitly and ImGui rounds to the display format. So sub-0.01 nudges to positions, rotations, tangents, and runtime are silently discarded at save; the preview shows the unquantised state and the game sees the quantised one.
- **AnimationGroup entries get reordered.** `load.cpp:1752-1774` splits a group's file-ordered children into `entries` and `entries_broken_links`; `save.cpp:815` and `:823` write them in that order. Any group containing an unresolvable object has its resolved entries hoisted above its broken ones. `save.cpp:817` also **drops** entries whose object name is empty. Entry order determines slot order in the engine's 255-binding pool.

One robustness note: `values::get<T>` bottoms out in `absl::InlinedVector::at`, which throws, and `load.cpp:1210` converts that into a `throw_layer_load_failure`. By then `close_world()` has already run (`world_edit.cpp:4409`). **A hand-edited `.ANM` with a short key line kills the entire world load, leaving no world open.**

---

### 3.10 Small stuff, for completeness

- **`last_position` / `last_rotation` are not uninitialised.** `types.hpp:28-38` gives `float3` NSDMIs plus `constexpr float3() = default;`, so `float3 last_position;` at `animation.cpp:326-327` is `(0,0,0)`. WorldEdit does not have the engine's `prev` bug — it has the opposite one: hard-coding zero encodes the engine's *post-first-wrap* state. Since **all 32 stock LOCAL animations with position keys start at exactly t=0.00 with value (0,0,0)**, `base + (key(t) − key(0))` and `base + key(t)` are algebraically identical, and the divergence is zero on correctly-authored content. The only residual: if you omit a t=0 position key, the engine reads uninitialised stack (`local_28`, assigned only afterwards) and jumps by an arbitrary amount, while the preview shows a clean start. **The preview actively hides the single worst authoring landmine in the system** — but the engine's behaviour there is undefined, so there is nothing faithful to be.
- **Zero-step substeps are benign.** `steps = static_cast<uint32>(delta_max * 4.0f + 0.5f)` (`animation.cpp:345`) is 0 whenever consecutive rotation keys are equal — 21 of 144 rotation segments across the stock LOCAL set, including 5 of `rep_loop`'s 10. `inv_steps` becomes `+inf` but the loop body never runs, and across a constant-rotation span a single delta is exact because the frame does not change. Full-timeline simulation gives 0.1–0.3% in-flight error on those animations. The genuinely catastrophic shapes (all rotation keys equal, exactly one rotation key, rotation channel starting later than the position channel) have **zero instances in stock content** — but they are trivially authorable and produce 141% path error and 45° orientation error respectively.
- **Non-looping past the last key.** The engine's `GetKeys` non-loop branch copies k0 into k1 (both the past-the-end path and the in-loop `else`), so LINEAR holds constant — exactly matching `animation.cpp:243-245`. Earlier analysis claiming a 10-second lie on `kamino1` `Anim3` was wrong; that key is linear and both hold. SPLINE with `p0 == p1` does bulge in the engine and flat-lines in the preview, but the only stock instance is `dea1_animations` `TieLaunch`'s position channel: 1.67 units on a 250-unit path.
- **Cost.** `_animation_solver.init(...)` is inside the per-frame draw (`world_edit_ui_animation_editor.cpp:1838`). `kamino1` `Anim1` rebuilds **8524 subkeys every frame**; 45 007 across the whole stock LOCAL set.

---

## 4. What WorldEdit gets right that is easy to get wrong

Credit where it is due — several of these are things you only get right if you have actually looked at the engine.

- **The matrix convention and the composition frame.** `matrix_funcs.hpp:8-12` proves `rows[i]` is the image of basis vector `i`, so `rows[0..2]` are right/up/forward and `rows[3]` is translation — the same meaning as the engine's `PblMatrix`. `matrix_funcs.hpp:21-27` makes `a*b` mean "apply b first", so `transform = transform * delta_transform` expands to `t + dp.x·right + dp.y·up + dp.z·forward`, character-for-character the engine's LOCAL translation update. Getting this backwards is the most natural mistake in the whole file and it is not made.
- **The non-LOCAL Euler order is exactly the engine's.** `make_quat_from_euler` is `Rz·Ry·Rx`, which is `D3DXMatrixRotationX·Y·Z` in the engine's row-vector convention. Verified numerically at 0.000000000 across seven test keys including multi-axis ones. And `rotation = base_rotation * make_quat_from_euler(euler)` is `R_base · R_key`, the same order as `D3DXMatrixMultiply(out, Rx·Ry·Rz, base)`.
- **The Hermite basis is bit-identical.** `PblHermiteVector3::Set` (`0x0041644B`, and `FUN_0083E720` on modtools) builds `a = ((m0+m1)T + 2(p0−p1))/T³`, `b = (3(p1−p0) − (2m0+m1)T)/T²`, `c = m0`, `d = p0`, evaluated in absolute elapsed seconds. Substituting `x = uT` collapses it to `h00·p0 + h10·T·m0 + h01·p1 + h11·T·m1` — exactly `hermite_interpolate` at `animation.cpp:22-36`, **including the `t_delta` scaling of both tangents**. Two earlier passes flagged that scaling as unverified; it is correct.
- **Left-key tangent sourcing and left-key transition mode.** Every one of the ten `hermite_interpolate(a.position, …)` call sites passes `a.tangent` and `a.tangent_next`, and every switch is on `a.transition`. That matches `InterpolateKeys` exactly, which selects on `param_1.mTransition` and passes `&param_1.mSlope0, &param_1.mSlope1`. `convert_to_smooth_spline` (`world_edit_ui_animation_editor.cpp:164-179`) also gets the semantics right, giving key `i` a `tangent_next` computed at key `i+1` — that is real understanding, not a lucky guess.
- **The loop wrap gap.** `b.time = animation.runtime` reproduces the engine's `mTotalTime − k0.t` exactly for the normal case, which is a non-obvious thing to know. The bug is the missing 1.0 s fallback, not the wrap emulation.
- **`GetKeys` returning false before the first key.** The engine's scan requires `sPAKeys[i].mTime <= t`, so a time preceding the first key returns false even when looping and the object stays at base. WorldEdit's `index == -1` base fallback matches.
- **Position/rotation interleaving is structurally impossible.** Separate vectors on load, two separate loops on save (`save.cpp:784-801`). A hand-written file that would corrupt both channels via `sNextAvailableKey` is **silently repaired** by a round trip. Accidental, but genuinely protective.
- **Degrees in the model, converted once at the point of use.** Correct call — WorldMunge does the conversion, so degrees is the right on-disk and in-editor unit.
- **`Place` is disabled for LOCAL animations**, with the tooltip "Place can not be used with Local Translation animations." The one place in the editor that acknowledges LOCAL is a different beast.
- Key times are kept sorted on insert, and duplicate times are blocked on both Add buttons.

---

## 5. Where our ground truth might be the thing that is wrong

A full adversarial pass was run against `docs/RE/ProceduralAnimation.md` with the explicit goal of breaking it. **It failed.** Twenty-five specific semantic assertions were re-checked against the binaries; all twenty-five held, and the checks were done on the **shipping** modtools build (`BF2_modtools_MemExt.exe`) as well as on Phantom, which was the best available attack surface given Phantom is a 2026 rebuild with a PA feature absent from shipping.

Three things got *stronger*, not weaker:

- **Base/out provenance is now proven at the call site**, which no earlier pass had done. `PAAnimationGroup::UpdateAnimations` (`0x0072FE8C`) passes `&sPAAnims[i].mInitialMatrix` into `PAAnimation::Update` (`0x0072FA60`), which copies the object's live `GetMatrix()` into a stack scratch, passes base as arg 4 and the scratch as arg 5, then `SetMatrix`es the scratch back. Every downstream claim rests on this and it is no longer an inference. Struct layout confirms it: `PAAnimationData` is 0x60 bytes with `mInitialMatrix` at +0x20.
- **The span rule is genuinely ungated on `mLoop`** — confirmed on both builds; `InterpolateKeys` never reads the flag byte. This was previously flagged as uncertain, and its resolution is what makes WorldEdit's missing 1.0 s fallback a real divergence rather than an artefact of a doc misreading.
- **The engine's Hermite scales both tangents by the segment span.** Proven algebraically from the constructor on both builds. The doc was right and merely too modest about it.

Every point where WorldEdit and the doc disagree, the engine sides with the doc: absolute `R·base` rotation in both arms, the ungated re-anchor, the 1.0 s span fallback, per-frame `prev` seeding from `GetKeys`. The hypothesis that WorldEdit's author had better information does not survive contact with the binaries.

**Two real errors in the doc, both arithmetic and both inert:**

1. Line 84, "endpoints 76 units apart" — from the doc's own coordinates `(0,0,100)` and `(63.7,0,63.7)`, `sqrt(63.66² + 36.34²) = 73.30`. Should read ~73.3. ("about 90 units from the start" is right.)
2. Line 147, "All 64 stock .ANM files" — this machine has **66** (`find C:\BF2_ModTools -iname "*.anm"`), 28 under `assets\worlds`, containing 174 `Animation` blocks; 301 blocks across all 66. The substantive claim (none interleave) is true for all 301.

**Three omissions:**

1. `runtime = 0` with `loop = 1` is a hard hang in three separate unguarded loops. Never mentioned.
2. "Snaps home once per cycle" is imprecise above 60 fps — the re-anchor window has fixed width `stepSize` while the cursor advances by `dt`, so above 60 fps the condition is true on several consecutive frames.
3. `ReadHierarchy` reads its node count with a signed `Read8` tested as `1 < cVar3`, so a hierarchy declaring more than 127 members is silently dropped.

**The honest caveat.** All of this is static analysis. Nothing was run in game and nothing was run in the editor. The engine side is decompiler output cross-checked between two builds; the WorldEdit side is source plus a float-faithful transcription of both evaluators. Two independent transcriptions reproduced each other's headline figures (the 8524 subkey count, the 2554-unit `kamino1` park, the 58 NaN animations), which is reassuring but is not the same as watching a gunship fly.

---

## 6. VERIFIED / INFERRED / UNKNOWN

**VERIFIED** — read out of a binary or out of the source, this session or cross-checked between passes:

- WorldMunge's π/180 handling. One constant at `0x0041D8BC`, exactly 15 `fmul dword [0x41d8bc]` = 3 (`mVal`) + 6 (`mSlope0`) + 6 (`mSlope1`), with the spill-reload-remultiply sequences visible in the instruction stream; the `AddPositionKey` branch has zero fmuls. I disassembled this myself. Rotation tangents reach the game at 1.745% strength.
- Engine: base/out provenance at the call site; both arms' operand sources; absolute `R(t)·base` rotation in both arms; 1/60 s substep (`0x3C88889A`, byte-identical on modtools `0x00ACEAD4` and Steam `0x007B1EE8`); wrap re-anchor ungated on `mLoop`, on both builds; the `prev` seeding hole with the position channel selector; the span rule including the 1.0 s fallback, on both builds; both slopes and the transition mode from the left key; the Hermite basis with span-scaled tangents; the 255/255/32 caps and the shared `sNextAvailableKey` cursor; 44-byte key stride.
- WorldEdit: every line quoted above, checked in the file. `float3` is zero-initialised (NSDMIs plus a defaulted non-trivial default constructor). `output_file` writes `'\n'` and stock `.ANM` files are CRLF. `load_animations`/`save_animations` apply no coordinate transform to key values while `read_location`/`read_rotation` do transform objects. The save format is field-exact against stock.
- Corpus facts: 28 `.ANM` under `assets\worlds`, 174 `Animation` blocks, 901 keys, zero interleaved, zero unsorted times, zero duplicate times, all 32 LOCAL animations with position keys start at (0, `(0,0,0)`).

**INFERRED** — strong, but not read from an instruction:

- That `.wld` `Rotation(w,x,y,z)` is object→world under standard Hamilton. This is the last link in the coordinate-space finding (3.1) and the only load-bearing inference left. The conjugate hypothesis is excluded structurally; the `Mz·R·Mx` decomposition holds to 4.4e-16 over 4000 random quaternions; WorldEdit's own `save.cpp:1129` default of `ChildRotation(0,0,1,0)` for an editor-identity object is exactly what the decomposition predicts. Thirty-second empirical test given in 3.1.
- All the magnitude figures from the 60 fps simulations. They are float-faithful transcriptions of both sides, not the real code paths. The mechanisms behind them are verified; the exact numbers are one remove from the binary.
- ImGui's format-rounding behaviour on sliders and drags (`RoundScalarWithFormatT`), which underpins the "times stay at 2 dp" claim in 3.9.
- The `GetKeys` non-loop k0/k1 aliasing was traced through `&sNextAvailableKey == &sPAKeys − 4` addressing rather than read as a clean index.

**UNKNOWN:**

- Anything about actual in-game behaviour. Nothing here was run in game or in the editor.
- How a NaN `float4x4` presents in WorldEdit's visualiser — vanished geometry, garbage triangles, or a silent no-op. The NaN itself is arithmetic certainty; its appearance is not.
- Two corpus scans disagree on the count of rotation keys with a non-zero Y or Z (136 vs 252). Both are large; the conclusion does not turn on which is right.
- Whether the shipping builds match Phantom in `PblHermiteVector3::Set` and `AddPositionKey`/`AddRotationKey` specifically — `CalculateNextMatrix`, `GetKeys`, and `InterpolateKeys` were cross-checked on modtools, the others were read on Phantom only.