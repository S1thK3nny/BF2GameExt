# Star Wars Battlefront II (2005) — Particle System

Reverse-engineering map for BF2GameExt. Covers emitter object model, memory pools, LOD/culling, every
enforced cap, an audit of the existing **Particle Cache Increase** patch set, and a prioritised patch plan.

**Status:** the P0 sort-pool rebase, the P0b `RenderAllCaches` state-leak fix, the P1 `IsFull` neutralisation, the P2 LOD work and the P4 emitter budget (both now folded into a single `ParticleDensity` dial) and the P3 decoupling described in §8 are **implemented** in `patch_table.cpp`. Both are statically verified against the binaries; neither has been runtime-tested, because no retail install exists on the development machine. Everything else in §8 is still open.

**Builds referenced throughout. Every address is tagged with its build; nothing is presented as verified
in a build where it was not read.**

| Tag | Binary | Role |
|---|---|---|
| **Phantom** | `Battlefront2.exe` (dev build, PDB symbols) | Reference / naming authority. **Addresses do not transfer.** |
| **modtools** | `BF2_modtools_MemExt.exe` | Patch target. `file_offset = VA − 0x400000` |
| **Steam** | `BattlefrontII.exe` (Steam retail) | Patch target. `file_offset = VA − 0x400C00` |
| **GOG** | `BattlefrontII_MemExt.exe` (project path `/BattlefrontII_GoGt.exe`) | Patch target. `file_offset = VA − 0x400C00`. Verified directly: `FlushParticleCache` and `IsFull` are byte-identical to Steam, shifted. |

---

## 1. Overview — the pipeline end to end

The system is **stateless and re-simulated every frame**. There is no persistent particle storage anywhere.
`ParticleEmitter::Render` re-derives every live particle from the emitter seed, the burst clock, and the
recorded motion history, writes them into one global scratch array, submits them, and forgets them. A
particle "dies" by no longer being regenerated.

Numbered flow, with the bound at each stage:

1. **Effect creation.** `FLEffectClass::BuildEffect` → `ParticleEmitterClass::BuildEffect` →
   `ParticleEmitterObject::Create` (modtools `0x0066BA60`, Steam `0x0060BA30`).
   *Bounded by:* `ParticleEmitterObject::sMemoryPool` (192 items, hard gate `mUsed < mCount`, returns NULL
   silently) and a network gate that suppresses creation entirely on a dedicated host.

2. **Emitter allocation.** `ParticleEmitterObject` ctor pool-allocates the root `ParticleEmitter` and walks
   the class `mNext` chain in `ParticleEmitter::Reset` (modtools `0x006680E0`, Steam `0x0060A130`).
   *Bounded by:* `ParticleEmitter::sMemoryPool` (1024) and the **`mMaxParticles` clamp to 128** applied here.

3. **Per-frame tick.** `ParticleEmitterObject::Update` (Phantom `0x00706260`).
   *Bounded by:* `bParticlesEnable`; `RegisterStep`'s 10 Hz motion-history sampling and the
   `ParticleEmitterInfoData` pool (1024); `ParticleEmitter::Update` retires expired bursts and kills the
   emitter when the cumulative `mParticleCount ≥ mMaxParticles`.

4. **Scene LOD arbitration.** `RenderObject` → `RedLodManager::Add(obj, priority)` (class 3 = all particle
   effects) → `RedLodManager::Render`.
   *Bounded by:* class-3 priority-heap capacity and a per-frame **pop cap of the top N objects by screen
   area**; the rest are discarded. Phantom: heap 1500, pop 400. Steam: heap 300/1500 conditional on
   `Init(isUberMode)`. **Pop cap not located in either target — see §9.**

5. **Transparent list.** `RedSceneManager::_AddTransparentObject` → `_RenderTransparentObjects` → vtable+0x4C.
   *Bounded by:* 1024-entry transparent heap (Phantom `0x008BFD10`).

6. **Per-effect render entry.** `ParticleEmitterObject::Render_` (modtools `0x0066B4B0`, Steam `0x0060C6B0`).
   *Bounded by:* `if (mEmitter == NULL) return;` then **`if (RedParticleRenderer::IsFull()) return;`** — a
   whole-effect early-out at ≥200 particles in the current batch.

7. **Distance/LOD setup.** `ParticleSystem::PrepareForRender` (Steam `0x0060E3F0`).
   *Bounded by:* far-distance fade against `LodData->mMaxDist` (50/150/500 by BoundingRadius) — returns
   false and the whole emitter draws nothing; near-scene fade from the `lodDistance` video slider; sets
   `mCurrentLod` 0–3, `mMinParticleSize`, `mSizeMultiplier`.

8. **Spawn/simulate.** `ParticleEmitter::Render` (modtools `0x00668E80`, Steam `0x0060A860`), once per
   emitter node in the `mNext` chain, into the global `sParticleCache : Particle[128]`.
   *Bounded by:* **127 slots per emitter per frame**; per-burst clamp of 128; the running `mMaxParticles`
   cutoff; and the **`sLodMask` bucket cull** (100/75/50/25 % by distance LOD).

9. **Per-particle submit.** `ParticleGeometry::Render` (modtools `0x0066BF60`, Steam `0x0060CAB0`).
   *Bounded by:* alpha ≤ 0 cull; `mMinParticleSize` cull (PT_PARTICLE only); `GetLodAlpha` cross-fade.
   `PT_PARTICLE` → `ParticleSystem::CacheParticle`; everything else → `RedParticleRenderer::SubmitParticle`.

10. **Depth sort.** `ParticleSystem::CacheParticle` (modtools `0x0066D820`, Steam `0x0060E700`) buffers into
    `sCachedParticles[300]`; `FlushParticleCache` (modtools `0x0066DAF0`, Steam `0x0060E760`) heap-sorts
    back-to-front and submits.
    *Bounded by:* **300-entry cache** (silent drop); near-plane cull at 1.0 unit; the stack sort pool sized
    to exactly 301 records.

11. **Batching.** `RedParticleRenderer::SetCurrentCache` (modtools `0x00824D00`, Steam `0x006D32B0`) selects
    one cache by `(texHash, blendMode, renderFlags)`; `SubmitParticle` → `AddParticle`.
    *Bounded by:* **200 particles per cache** (silent drop) and **15 caches** (past which `currentCache =
    NULL` and *all* further submission is a no-op).

12. **Draw.** `RedParticleRenderer::RenderAll` (modtools `0x008276F0`, Steam `0x006D30C0`) builds one
    `pcRedPrimitive` per non-empty cache from shared dynamic D3D vertex buffers, then resets
    `s_cacheIndex = 0; currentCache = NULL`.
    *Bounded by:* vertex-buffer lock failure (bails without resetting state — a latent state leak, **fixed, see §8 P0b**);
    16-bit index buffer.

**Critical timing detail:** `RenderAll` is called at the tail of `Render_` for every **non-child** PEO. So
the 300-entry sort cache and the 15×200 batch budget are **per top-level effect object**, not per frame.
Child PEOs (`mIsChildPEO`) return before the flush, so an emitter-of-emitters chain accumulates into one
batch set and starves its own later children.

---

## 2. Object model

| Struct | Size | Notes |
|---|---|---|
| `Particle` | 64 (0x40) | `mPosition`(0) `mVelocity`(12) `mSize[1]`(24) `mRotation[1]`(28) `mRotVelocity[1]`(32) `mColor[1]`(36, RedColorValue 16B) `mLifeTime`(52) `mUseHSV`(56) `mNoRegisterStep`(57) `mLod`(58) `mFrameIndex`(59, signed byte) `mInfoData`(60). Stride proven by `SHL EAX,0x6`. **Exists only inside `sParticleCache`.** |
| `ParticleEmitter` | 64 (0x40) | `mParticleCount:u16`(0) `mMaxParticles:s16`(2) `mSpawnDelay`(4) `mColor`(8) `mSize`(24) `mInheritVelocityFactor`(28) `mClass`(32) `mSeed`(36) `mSpawnerSeed`(40) `mFirstInfo`(44) `mStopTime`(48) `mNext`(52) `mAmbientSound`(56) `mSoundStarted`(60). Pooled. |
| `ParticleEmitterClass` | 448 (0x1C0) | Loaded `.fx` asset. `mStartDelay`(0x0C) `mBurstDelay`(0x14) `mBurstCount`(0x1C) `mMaxParticles`(0x24) — all `VarianceValue{float mValue; float mVariance}`; `mSpawner`(0x5C, 212B) `mTransformer`(0x130, 32B) `mGeometry`(0x150, 64B) `mLodData`(0x190) `mMinDistLOD`(0x194) `mMaxDistLOD`(0x198) `mBoundingRadius`(0x19C) `mNext`(0x1A4). Heap, `operator new(0x1C0)`. |
| `ParticleEmitterObject` | 272 (0x110) | `Thread` + `PblHandled` + `FLEffectObject` + `RedSceneObject`. Retail offsets: `mEmitter`+0xD8, `mIsChildPEO`+0xE7 (verified in both targets). Pooled. |
| `ParticleEmitterInfoData` | 128 (0x80) | Refcounted, doubly-linked motion-history keyframe. `mMatrix`(0x10) `mVelocity`(0x50) `mStartTime`(0x68) `mLifeTime`(0x6C) `mNext`(0x70) `mPrev`(0x74). Pooled. |
| `CacheParticle` | 36 (0x24) | `mPos`(0) `mColor`(0x0C) `mSize`(0x1C) `mRotation`(0x20). Element of the 300-entry depth-sort cache. |
| `RedParticleInfoType` | 68 (0x44) | `mPit`(0) `mPos`(4) `mColor`(0x10) `mSize`(0x14) `mRotation`(0x18) `u[2]`/`v[2]`(0x1C..0x28) `mRight`(0x2C) `mUp`(0x38). |
| `RedParticleRenderer` | 13656 (0x3558) | `m_particleCache[200]` at offset 0 (0x3520 bytes), then `m_particleIndex`(0x3520) `m_texHash`(0x3524) `m_blendMode`(0x3528) `m_uiRenderFlags`(0x352C) `m_numVerts`(0x3530) `m_numIndices`(0x3534) `m_blurBounds`(0x3538) `m_blurValue`(0x3550) `m_blurParam`(0x3554). **200×0x44 == 0x3520 exactly** — the cap and the header offset are the same number. |
| `MemoryPool` | 84 (0x54) | `mLabel[32]`(0x10) `mSize`(0x30) `mCount`(0x34) `mGrow`(0x38) `mUsed`(0x3C) `mPeak`(0x40) `mHeap`(0x44) `mPool`(0x48) `mFree`(0x50). |

### Where particles live

| Array | Type | modtools | Steam | Phantom |
|---|---|---|---|---|
| `sParticleCache` | `Particle[128]` (8,192 B) | `0x00B9B150` | `0x01EF2730` | `0x00BA0560` |
| `sCachedParticles` | `CacheParticle[300]` (10,800 B) | `0x00B9DB78` | `0x01EF5120` | `0x00BA2F90` |
| `mNumCachedParticles` | `int` | `0x00B9DB54` | `0x01EAED7C` | `0x00BA2F5C` |
| `s_caches` | `RedParticleRenderer[15]` (204,840 B) | `0x00E5F650` | `0x009661E0` | `0x026BD0D0` |
| `currentCache` | `RedParticleRenderer*` | `0x00E5F644` | `0x009661B4` | `0x026BD0A0` |
| `s_cacheIndex` | `int` | `0x00E5F648` | `0x009661B8` | `0x026BD0A4` |
| `mCurrentLod` | `int` | `0x00B9DB2C` | `0x01EAED8C` | `0x00BA2F44` |
| `sLodMask` / `sLodFadeMask` | `bool[4][4]` ×2 | `0x00AD6354` / `0x00AD6364` (.data) | `0x0078AA94` / `0x0078AAA4` (**.rdata**) | `0x00A96710` / `0x00A96720` |

`sEmitterCache : Particle[32]` (Phantom `0x00B9FC48`) has **no code xrefs** outside its dynamic
initialiser — dead in this build. The 31-child cap in `RenderAsEmitter` is real and enforced independently.

Not part of this system, despite the names: `DustEffectClass::SpawnParticle`/`UpdateParticle`
(Phantom `0x00500300`/`0x005009C0`), `SpaceDustEffect`'s `ParticleData`, and `ClothData::GetNumParticles_`.

---

## 3. Memory pools

Defaults come from a hard-coded `MemoryPool::Setup` table at mission init — **not** from Lua, contrary to a
first-pass reading. Byte-decoded in Phantom (~`0x005CC9B8`+) and cross-verified in both targets:
**modtools `0x0073462E`**, **Steam `0x00531510`** (`FF 35 …` / `B9 <pool>` / `FF 35 …` / `68 C0 00 00 00` /
`E8 …` = `Setup(pool, 0xC0, mSize, mGrow)`).

| Pool | Item | Count | Total | Consumer | Exhaustion behaviour |
|---|---|---|---|---|---|
| `ParticleEmitterObject` | 272 | **192** | 52,224 B | One per live effect instance | `Create` gated by `mUsed < mCount` **before** `Allocate` → returns NULL, **pool never grows, nothing logged**. `BuildEffect` returns NULL, the effect never spawns. *modtools pool `0x00B9DA38`, Steam `0x01EF5090`* |
| `ParticleEmitter` | 64 | 1024 | 65,536 B | One per emitter node in an `.fx` `mNext` chain | Same gate. `mEmitter = NULL`; the PEO is scene-registered and ticks forever but `Render_` bails on the first line. A permanent invisible zombie. *modtools `0x00B9AFE8`, Steam `0x01EF26C8`* |
| `ParticleEmitterInfoData` | 128 | 1024 | 131,072 B | ~10 nodes/sec per moving emitter | Same gate. **First** allocation failure → `mFirstInfo` stays NULL → `ParticleEmitter::Render` early-outs, emitter permanently silent. **Later** failure degrades gracefully (stale matrix, smeared trails). *modtools `0x00B9D9C0`, Steam `0x01EF5040`* |
| `ParticleTransformer::PositionTransformer` | 32 | 1024 | 32,768 B | Load-time only | **Ungated** — grows from the Red heap, logs `Memory pool "%s" is full`. Load-time `Allocate` result is dereferenced without a null check. |
| `…SizeTransformer` | 24 | 1024 | 24,576 B | Load-time only | as above |
| `…RotationTransformer` | 24 | **0** | 0 | Load-time only | Grows from the very first rotation transformer, 3 items at a time; trips `PoolErrorPostLoad`'s "expansion during loading is not allowed" |
| `…ColorTransformer` | 36 | 1536 | 55,296 B | Load-time only | as above |

Particle-system total ≈ **361 KB** of RunTimeHeap.

Key asymmetry: the three per-frame pools are the *only* particle pools with a `mUsed < mCount` pre-gate,
which makes `MemoryPool::Allocate`'s grow path and all three of its warning strings **unreachable** for
them. Every particle pool exhaustion is therefore silent.

`Lua_Callbacks::SetMemoryPoolSize(name, n)` (Phantom `0x00654250`) can raise any of these by label, but
only before the pool's first allocation (`SetCount` refuses once `mPool != NULL`). Since the default table
runs at mission init and pools are created lazily, mission Lua *can* legitimately override them.

Adjacent hazard: `ParticleEmitterEmitterObject::RegisterStep` (Phantom `0x00704CE0`) **null-derefs** on
InfoData pool exhaustion — `0x00704E09` zeroes EAX, `0x00704E15` reads `[EAX+0x68]`. Not located in targets.

---

## 4. LOD and culling

Everything here is **distance-driven**. There is no frame-rate-adaptive path anywhere in the particle
system — searched exhaustively across the call graph and found no read of frame delta feeding a spawn or
cull decision.

| Mechanism | Cuts | Detail |
|---|---|---|
| **`sLodMask` bucket cull** | **Spawn** (and therefore render) | `IsLodActive(index & 3)` — modtools `0x0066D790`, Steam `0x0060E680`. Table rows `{1,1,1,1}/{1,1,1,0}/{1,0,1,0}/{1,0,0,0}` = **100/75/50/25 %**. Culled particles are never created **but still advance the running counter**, so they consume the `mMaxParticles` budget. `PT_STREAK` is exempt. |
| **LOD ramp** | selects the above | `PrepareForRender`: `lodF = 4.0 / (min(zoom·mMaxDist, 90) + mFadeBias − 20) · ((dist − radius) − 20)`, saturated to 3. With `mFadeBias = 0` (LOW quality) LOD3 is reached at ≈72 m; MEDIUM ≈102 m; HIGH ≈132 m. |
| **`sLodFadeMask` cross-fade** | render alpha | Fades the exact bucket the *next* LOD step will kill, via `mLodFadeVal = 1 − frac(lodF)`. This is why the 25 % steps read as smooth thinning rather than popping. |
| **`particleQuality` video option** | the ramp | Only reader is `ApplyPostLoadSettings` (Phantom `0x0071EF30`): LOW→`mFadeBias = 0`, MEDIUM→40.0, HIGH→80.0. That single float is the *entire* effect of the setting. |
| **`SetParticleLODBias(x)` (Lua)** | render (whole emitter) | `SetMaxDistanceBias` rewrites the three shared `LodData.mMaxDist` values to `x+50` / `x+150` / `x+500`. Idempotent, not cumulative. |
| **Effect far-distance fade** | render (whole emitter) | Fade begins at `0.8 · zoom · mMaxDist`, alpha 0 at `1.0 ·`. At alpha ≤ 0 `PrepareForRender` returns false and **nothing** of the emitter draws. Class chosen at load from `BoundingRadius`: ≤5 → 50 m, ≤20 → 150 m, else 500 m. **Default when the `.fx` omits BoundingRadius is 5.0 → the 50 m class.** |
| **Near-scene fade (`lodDistance` slider)** | render (whole emitter) | `start = lerp(72, 300, lodDistance)`, `end = lerp(120, 500, lodDistance)`. At slider 0 **all** particle effects are gone past ~120 m regardless of LOD bias. |
| **`mMinParticleSize`** | render (per particle) | `0.002·(dist − radius) + 0.01` world units. PT_PARTICLE only. The particle is **skipped, not clamped**, and the test uses raw `mSize[0]` *before* `mSizeMultiplier` is applied. |
| **`mSizeMultiplier`** | compensation | `0.1·clamp(lodF,1,3) + 1.2` → 1.3×–1.5×, inflating distant particles to mask the thinning. |
| **`RedLodManager` class-3 pop cap** | render (whole effect objects) | Class 3 (all particle effects) takes a special branch that **ignores the cost budget and the screen-size/vertex-density culls entirely** and pops only the top N by screen area, then zeroes the heap. Phantom N = 400 (`MOV EDX,0x190` @ `0x008D04B8`), heap 1500. Steam `SetClassMaxCost` @ `0x006BC867` is conditional on `Init(isUberMode)`: uber → heap 1500, else **heap 300**. **The pop cap itself was not located in either target.** |
| **`rendering.minScreenSize` / `maxVertexDensity`** | **nothing (negative result)** | Both live in the `class != 3` arm of `RedLodManager::Render`. Particle effects branch away first. Tuning these will **not** change particle density. |
| **Distortion / `s_blurEnable`** | render (whole cache) | When off, `RenderAll` skips every `BLEND_BLUR` cache — the particles still cost a cache slot and up to 200 entries while drawing nothing. |
| **Splitscreen** | spawn (at load) | `LoadEmitter` halves `MaxParticles` (floored at 1) when `m_iNumCam > 1`. Baked into the loaded class. |
| **`MinLODDist` / `MaxLODDist` `.fx` keys** | **nothing** | Parsed into class +0x194/+0x198; **no reader found anywhere in the image** across three independent byte-pattern searches. Dead content knobs. Draw distance comes solely from `BoundingRadius`. |

---

## 5. The limit table

Severity reflects impact on *"many particles on screen with no dropouts"*. `—` = not located in that build.

### 5a. Spawn side (per emitter)

| Name | Kind | Bounds | Value | Phantom | modtools | Steam | Failure mode | Sev |
|---|---|---|---|---|---|---|---|---|
| `sParticleCache` slot cap | fixed-array | Particles ONE emitter can present in ONE frame | **127** of 128 | `0x0070290A`, `0x00702AC7` | outer `0x006691EC` (imm8 `0x006691F0`); inner `0x00669320` (imm32 `0x00669321`); warn-cmp `0x0066932B` | outer `0x0060AAD3` (imm8 `0x0060AAD5`); inner `0x0060ABA4` (imm32 `0x0060ABA5`) | Silent break out of the burst replay. Replay runs **oldest→newest**, so the **newest** particles (the dense core at the emitter) are the ones lost. Effect keeps its faded outer shell, loses its hot centre. | **High** |
| `mMaxParticles` clamp | count-clamp | Emitter **lifetime total** emission budget | **128** | `0x00703FB9` | `0x006681AC` + `0x006681B8` (two imm16) | `0x0060A23F` (imm32 of `MOV EDX,0x80`) | `.fx` asking 500 gets 128; `ParticleEmitter::Update` then returns false and the emitter is **destroyed**. Continuous effects cut out early rather than thinning. `-1` = unlimited, preserved (signed `JLE`). | **High** |
| `mMaxParticles` running cutoff | count-clamp | Replay stop + burst trim | ≤128 | `0x007028F6`–`0x00702904` | `0x006691D7`–`0x006691E6` | `0x0060AAC0`–`0x0060AACD` | Tail of the timeline empty; emitter dies next tick. | Med |
| Per-burst count clamp | count-clamp | Particles from one burst event | **128** | `0x00702A7A`/`0x00702A83` (Render), `0x007044EE`/`0x007045F2` (Update) | Render `0x006692B5`, `0x006692BC`; Update `0x00667DE4`, `0x00667DEB` | Render `0x0060AB44`, `0x0060AB4E`; Update `0x0060A791`, `0x0060A79A` | `BurstCount > 128` silently truncated. | Med |
| `sLodMask` bucket cull | lod-cull | Whether each particle is created at all | 100/75/50/25 % | table `0x00A96710`, gate `0x00702ADC` | table `0x00AD6354`, gate `0x006693B8` | table `0x0078AA94` (.rdata), gate `0x0060ABF9` | Up to 75 % of every effect never exists past ~72 m (LOW quality). Culled particles still burn the `mMaxParticles` budget. | **High** |
| `sLodFadeMask` cross-fade | lod-cull | Alpha of the doomed bucket | — | `0x00A96720` | `0x00AD6364` | `0x0078AAA4` | Not a drop by itself; drives that bucket's alpha to 0, at which point the zero-alpha cull removes it. | Low |
| PT_STREAK minimum run | count-clamp | Streak emitters need ≥3 live particles | 3 | `0x0070301F` | — | — | Streak draws **nothing at all**, not a short streak. Flickers as other caps steal particles. | Med |
| PT_EMITTER child cap | count-clamp | Lifetime sub-emitters from one emitter | **31** | `0x007035AA`, `0x00703710` | **not located** (counter is in a stack slot; six encodings searched, no hit in `0x00668320`–`0x00668C30`) | `0x0060B346` (`66 83 F9 1F`) | Compound effects stop producing children permanently after 31. | Med |
| RenderAsEmitter per-call spawn | count-clamp | Children per `RenderAsEmitter` call | 128 | `0x0070371B`, warn `0x00703744` | warn reachable from `0x00668696` | — (warning strings stripped) | Skips the child; logs `"ParticleEmitter: Spawning too much at once (max = %d)!"`. **The only self-reporting limit in the whole system.** | Low |
| Splitscreen `MaxParticles` ×0.5 | count-clamp | Class value at load | ×0.5, floor 1.0 | `0x007197D3` | present in `LoadEmitter` `0x00670FD0` | — | Every effect emits half as many in splitscreen. Baked in at load. | Med |
| `PT_ANIMATED` frame index | other | Animated frame selection | signed byte; `TotalFrames` `IDIV` | `0x00702F19`–`0x00702F67` | — | — | `TotalFrames(0)` on a looping emitter = divide-by-zero crash; `>127` wraps negative → particle never drawn. | Low |

### 5b. Render side

| Name | Kind | Bounds | Value | Phantom | modtools | Steam | Failure mode | Sev |
|---|---|---|---|---|---|---|---|---|
| `RedParticleRenderer::AddParticle` | fixed-array | Particles in ONE `(tex, blend, flags)` batch | **200** | `0x008AE70B` (imm32 `0x008AE70C`) | `0x00824E16` (imm32 **`0x00824E18`**) | `0x006D0C6B` (imm32 **`0x006D0C6C`**) | Returns false; `SubmitParticle` discards it with no retry, no flush, no counter. `FlushParticleCache` submits up to 300 sorted sprites into this one batch, so the **last 100 — which the back-to-front sort makes the NEAREST to camera — vanish**. | **High** |
| `IsFull()` whole-emitter skip | frame-budget | Whether an entire PEO renders at all | **200** (same constant, second copy) | `0x008B1189` (imm32 `0x008B118F`), call `0x00705986` | `0x00824DF1` (imm32 **`0x00824DF3`**), call `0x0066B4C4` in `0x0066B4B0` | `0x006D309C` (imm32 **`0x006D30A2`**), call `0x0060C6CA` in `0x0060C6B0` | **Whole effects blink out**, not individual particles. Worse: the `return` fires *before* the `FlushParticleCache(); RenderAll();` tail, and `RenderAll` is the only thing that resets `s_cacheIndex`/`currentCache` — so a full cache is **self-perpetuating** and cascades to every subsequent PEO in the phase. | **High** |
| `s_caches` batch count | pool-capacity | Distinct `(tex, blend, flags)` batches in flight | **15** | `0x008B1483` (imm8 `0x008B1485`) | `0x00824D3B` (imm8 **`0x00824D3D`**) | `0x006D32E8` (imm8 **`0x006D32EA`**) | Sets `currentCache = NULL`; **every** subsequent `SubmitParticle` from **every** system is a no-op until `RenderAll`. Total blackout, not partial thinning. `IsFull()` returns *false* for a NULL cache, so nothing detects it. | **High** |
| `sCachedParticles` sort cache | fixed-array | PT_PARTICLE sprites buffered per texture batch | **300** | `0x00708AC9` | `0x0066D826` (imm32 **`0x0066D828`**) | `0x0060E707` (imm32 **`0x0060E709`**) | Silent drop, no early flush. Since only PT_PARTICLE uses it and 3 chained emitters × 127 exceed it, this is the wall for multi-emitter smoke/dust. | **High** |
| `FlushParticleCache` stack sort pool | fixed-array | Depth-sort heap records | **301** (sentinel + 1..300) | frame `SUB ESP,0x980` @ `0x00708B53`, pool `EBP−0x980` | frame `SUB ESP,0x994` @ `0x0066DAF5` (imm32 `0x0066DAF7`), pool `ESP+0x34`, `ADD ESP` @ `0x0066DD1C` (imm32 `0x0066DD1E`) | frame `SUB ESP,0x980` @ `0x0060E778` (imm32 `0x0060E77A`), pool **`EBP−0x988`** (`LEA` @ `0x0060E7C5`, disp32 `0xFFFFF678`) | Cannot overflow at stock (the 300 cap gates it). `PblHeap::mMaxCount` is **never enforced** — `SortDown` stores unconditionally. Raising the 300 without resizing the pool **smashes the return address**. | **High** (as a patch hazard) |
| `mMinParticleSize` cull | size-cull | PT_PARTICLE size vs distance threshold | `0.002·(d−r)+0.01` | set `0x0070917E`/`0x0070919C`, enforced `0x007069B6` | — | — | Particle **skipped, not clamped**; test uses raw size before `mSizeMultiplier`. Fine spray/spark/dust vanishes with distance while large smoke survives. | Med |
| Zero-alpha cull | other | Any non-PT_STREAK particle | `a ≤ 0` | `0x00706991` | — | — | Silent skip. Combines with LOD fade and both distance fades. | Med |
| `FlushParticleCache` near-plane cull | distance-cull | Camera-forward distance − 1.0 | 1.0 unit | `0x00708C2B`–`0x00708C32` | present in `0x0066DAF0` | present in `0x0060E760` | Particles within 1 m of the camera never enter the sort heap. Point-blank effects lose their nearest layer. | Low |
| `RenderAll` VB-lock bail | alloc-failure | All remaining caches that frame | — | `0x008B1237` → `0x008B137D` | `0x00827768` → `0x008278A5` | `0x006D3131` → `0x006D326D` | Early `return` **skips the `s_cacheIndex = 0` / `currentCache = NULL` resets**. Stale full caches persist, `s_cacheIndex` ratchets toward the cap → permanent particle blackout with no recovery short of level reload. Present identically in all three builds. | Med |
| `BLEND_BLUR` skip | other | Every blur cache | `s_blurEnable` | write `0x0071EF5B`, flag `0x026BD0A8` | flag `0x00E5F640` | flag `0x009661BC` | Distortion particles invisible with distortion off, but still consume a cache slot and up to 200 entries. | Low |
| 16-bit index buffer | vertex-buffer | Indices per draw batch | 65535 | — | — | — | Not reachable at 200/batch (1200 indices). Becomes the ceiling only if `AddParticle` is raised past ~10,900. | Low |

### 5c. Scene / whole-effect side

| Name | Kind | Bounds | Value | Phantom | modtools | Steam | Failure mode | Sev |
|---|---|---|---|---|---|---|---|---|
| `RedLodManager` class-3 pop cap | frame-budget | Particle effect objects submitted per view | **400** | `0x008D04B8` (`MOV EDX,0x190`) | **not located** (byte-pattern for `MOV EDX,0x190` finds nothing — different codegen) | **not located** | Top-400 by screen area survive; the rest are discarded and the heap zeroed. Load-adaptive and churny → effects near the cut line flicker on/off frame to frame. **Strong candidate for the classic "particles thin out under load".** | **High (unverified in targets)** |
| Class-3 priority heap | pool-capacity | Effects queued for LOD consideration | Phantom 1500 (unconditional); Steam 300 (non-uber) / 1500 (uber) | `0x0088D9BB` | — | `SetClassMaxCost` @ `0x006BC867`, branch on `Init(bool isUberMode)`; non-uber pushes `0x3E8,0x3E8,0x3E8,0x12C` | `RenderObject` logs "too many objects of class %d" and drops. In Steam non-uber (300) this binds *before* the pop cap. | Med |
| Transparent object heap | pool-capacity | All transparent scene objects | 1024 | `0x008BFD10` | — | — | Logs "too many transparent objects…" and drops. | Med |
| Effect far-distance cull | distance-cull | Whole PEO | `mMaxDist` 50/150/500 + bias | `0x0070904B`; class pick `0x007199DD`/`0x00719A01` | — | — | Whole effect vanishes. Default class (no `BoundingRadius` in `.fx`) is **50 m**. | **High** |
| Near-scene fade | distance-cull | Whole PEO | `lerp(72,300)`→`lerp(120,500)` | `0x00709087` | — | — | At `lodDistance = 0`, all particles gone past ~120 m regardless of LOD bias. | **High** |
| `bParticlesEnable` | other | All PEO update+render | bool, default 1 | `0x00A965D0`, tested `0x00706266` | — | — | Master off switch (`particles.Enable`). | Low |
| `sRenderEmitters` | other | Whole PT_EMITTER recursion | bool, default 1 | `0x00A9657C`, tested `0x00702606` | — | — | Compound effects produce nothing. | Low |
| Network gate on PEO creation | other | Whether an effect object is created | predicate | `0x00704BC0` | — | — | Dedicated host creates no particle effect objects at all. Invisible to clients. | Low |
| Effect class hash table | pool-capacity | Distinct effect class names | 256 | `_Store(… ,0x200, …)` @ `0x008644D0`, table `0x00AF9388` | — | — | Silent non-registration below capacity; the backwards linear probe has **no termination condition** at exactly full. | Med |
| `AttachedEffects` global | fixed-array | Total attach entries across all ODFs | 64 | `0x004908F2`, array `0x00ABBB48` | — | — | Logs "AttachEffects: too many effects"; 65th+ attached effect never spawns. | Med |
| `RegisterStep` throttle | frame-budget | Motion-history sample rate | 0.1 s | `0x00705210` / `0x00705491` | — | — | Quantises fast-moving emitter paths into a 10 Hz polyline; main consumer of the InfoData pool. | Low |

---

## 6. Diagnosis — why the current patch does not deliver

### 6.1 The modtools half is internally consistent. The fault is not in the patch's execution.

Every claim in the patch table's modtools entries was independently verified:

* `0x26D828` → VA `0x0066D828` **is** the imm32 of `81 F9 2C 01 00 00` (`CMP ECX,0x12C`) at `0x0066D826`.
* `0x26DAF7` / `0x26DD1E` → VA `0x0066DAF7` / `0x0066DD1E` **are** the imm32s of `SUB ESP,0x994` and
  `ADD ESP,0x994`.
* All **18** disp32 references into `sCachedParticles` element-0 fields (`0x00B9DB78`/`7C`/`80`/`84`/`88`/
  `8C`/`90`/`94`/`98`) were enumerated by exhaustive byte scan — the scan returns exactly 18 sites and the
  patch table lists exactly those 18, one for one. **There is no missed reference.** The count global
  `0x00B9DB54` is correctly left alone (it is a scalar).
* The stack-frame arithmetic is exact. `FlushParticleCache` on modtools is a pure ESP frame (no EBP, no
  SEH, no /GS cookie), pool at `ESP+0x34`, return address at `ESP+0x99C`: `0x99C − 0x34 = 0x968 = 301×8`.
  Patched: `0x25B4 + 8 − 0x34 = 0x2588 = 1201×8`. Sentinel + 1..1200. Correct to the byte.

Two cosmetic notes: the `mMaxCount` write at `0x26DB6D` is a **dead store** — the compiler inlined
`PblHeap::Insert` to `mCount = ++n; SortDown(n, …)` and elided the capacity check entirely, so
`mMaxCount` is never read. Harmless but inert; the real bound is the stack pool size. And the 120-slot
`s_caches` redirect wastes 105 × 0x3558 ≈ 1.4 MB unless the clamp is raised (see 6.3).

### 6.2 It raises a limit that is not the binding one.

`FlushParticleCache` calls `SubmitTexture` **exactly once** before its render loop, then pushes every
sorted particle into that single cache with `SubmitParticle(PIT_PARTICLE, …)`. Each type-0 submit consumes
one 0x44-byte entry, and:

```
modtools 0x00824E10:  8B 91 20 35 00 00   MOV EDX,[ECX+0x3520]
         0x00824E16:  81 FA C8 00 00 00   CMP EDX,0xC8      ; 200
         0x00824E1C:  7C 05               JL  ok
         0x00824E1E:  32 C0               XOR AL,AL         ; return false
         0x00824E20:  C2 20 00            RET 0x20
modtools 0x008251BC:  84 C0               TEST AL,AL
         0x008251BE:  74 56               JZ  0x00825216    ; silently discard
```

Steam is identical at `0x006D0C6B` (`3D C8 00 00 00`) with the drop at `0x006D33E2`.

So **a single flush can never emit more than 200 particles regardless of what `CacheParticle` accepts.**
Vanilla already threw away 100 of its 300; the patch alone would throw away 1000 of 1200. On its own,
`ParticleCacheIncrease = 1` produces **zero visible change**.

### 6.3 It only does anything because of a *differently named* INI key.

The 15-cache clamp is untouched by any of the three patch sets — `83 FA 0F` at modtools `0x00824D3B` /
Steam `0x006D32E8`. What actually makes the 120-slot buffer usable is `gc_visual_limits.cpp`:

* `:441-447` raises the imm8 clamp to `RENDERER_CACHE_SLOTS` (120),
* `:155-204` installs a `SubmitParticle` detour that spills into a fresh cache slot when the current one
  fills, lifting the effective ceiling to ≈120 × 196 ≈ 23,500 entries.

But `:351` returns early on `!g_gcVisualLimitsEnabled` (INI key **`GCVisualLimits`**), `:406` returns early
if `verify_and_apply()` fails on any of ~16 **unrelated Galactic Conquest galaxy-map** byte-patch sites —
and that check runs *before* the clamp raise and *before* the detour install. So:

> A GC-map-only signature mismatch, or a user turning off `GCVisualLimits`, silently reverts the entire
> particle patch to a no-op while the install log still reports "Particle Cache Increase" applied.
> `ParticleCacheIncrease` has no self-sufficiency.

Also note the clamp is written into a **sign-extended imm8** (opcode `0x83`). `RENDERER_CACHE_SLOTS ≥ 128`
would write a byte that sign-extends negative, making the `JL` fail for every value and pinning
`currentCache = NULL` forever — a permanent total blackout. 120 is safe; there is no assertion guarding it.

### 6.4 The whole-effect kill switch is untouched in every configuration.

`RedParticleRenderer::IsFull()` is a **second, unsynchronised copy of the 200 constant** —
modtools `0x00824DF1`, Steam `0x006D309C` — and `ParticleEmitterObject::Render_` calls it as its second
instruction group and returns outright:

```
modtools 0x0066B4B6:  8B 86 D8 00 00 00   MOV EAX,[ESI+0xD8]   ; mEmitter
         0x0066B4BC:  85 C0               TEST EAX,EAX
         0x0066B4BE:  0F 84 CF 02 00 00   JZ  0x0066B793
         0x0066B4C4:  E8 17 99 1B 00      CALL 0x00824DE0      ; IsFull
         0x0066B4C9:  84 C0               TEST AL,AL
         0x0066B4CB:  0F 85 C2 02 00 00   JNZ 0x0066B793       ; WHOLE PEO SKIPPED
```

The spill detour swaps `currentCache` only *inside* `SubmitParticle`. If a batch reaches exactly 200 and
no further submit occurs, `currentCache` stays full, `IsFull()` returns true, and the next PEO bails —
**before** its `FlushParticleCache(); RenderAll();` tail, which is the only code that drains the pool. That
is a cascade, not a single dropped effect. This is almost certainly the residual "whole explosions blink
out" symptom.

### 6.5 The Steam and GOG entries are actively dangerous.

Steam `FlushParticleCache` (`0x0060E760`) is an **EBP frame with SEH**, and the sort pool is
**EBP-relative**, not ESP-relative:

```
Steam 0x0060E778:  81 EC 80 09 00 00        SUB ESP,0x980          ; <-- the ONLY thing the patch rewrites
      0x0060E7C5:  8D BD 78 F6 FF FF        LEA EDI,[EBP-0x988]    ; pool base
      0x0060E7DD:  C7 85 78 F6 FF FF …      MOV [EBP-0x988],0x7F7FFFFF
      0x0060E8DA:  MOVSS [EBP+EDX*8-0x988],XMM3
      0x0060E8E8:  MOV   [EBP+EDX*8-0x984],EAX
```

Growing `SUB ESP` extends the frame **downward**; the pool still starts at `EBP−0x988` and still grows
**upward** by index. Capacity stays at 301 records (indices 0..300) while `CacheParticle` is now
permitted to feed it 1200.

Overwrite mapping, derived from the frame layout (locals occupy `EBP−0x20` upward):

| Index | Lands on |
|---|---|
| 301 | `PblHeap.mCount` / `.mMaxCount` (`EBP−0x20` / `EBP−0x1C`) |
| 302 | `PblHeap.mPool` pointer (`EBP−0x18`) — the render loop reloads from here |
| 303 | colour bytes and the saved SEH handler chain (`EBP−0xC`) |
| 304 | SEH trylevel (`EBP−0x4`) |
| 305 | **saved EBP and the return address** |

The ten disp32 fields, read byte-exactly from both retail images (`0xFFFFF678` for `.mKey`, `0xFFFFF67C` for `.mObj`):

| Instruction | Steam | GOG |
|---|---|---|
| `LEA EDI,[EBP-0x988]` (pool base) | `0x0060E7C7` | `0x0060F867` |
| `MOV [EBP-0x988],0x7F7FFFFF` (sentinel) | `0x0060E7DF` | `0x0060F87F` |
| `COMISS XMM3,[EBP+ECX*8-0x988]` (sift) | `0x0060E8A6` | `0x0060F946` |
| `MOV EAX,[EBP+ECX*8-0x988]` | `0x0060E8B3` | `0x0060F953` |
| `MOV [EBP+EDX*8-0x988],EAX` | `0x0060E8BA` | `0x0060F95A` |
| `MOV EAX,[EBP+ECX*8-0x984]` | `0x0060E8C1` | `0x0060F961` |
| `MOV [EBP+EDX*8-0x984],EAX` | `0x0060E8C8` | `0x0060F968` |
| `COMISS XMM3,[EBP+ECX*8-0x988]` (loop) | `0x0060E8D4` | `0x0060F974` |
| `MOVSS [EBP+EDX*8-0x988],XMM3` | `0x0060E8DF` | `0x0060F97F` |
| `MOV [EBP+EDX*8-0x984],EAX` | `0x0060E8EB` | `0x0060F98B` |

The table's own comment acknowledged the frame difference but drew the wrong conclusion from it. GOG was subsequently loaded and verified: its `FlushParticleCache` (`0x0060F800`) is instruction-for-instruction identical to Steam's, so it was affected in exactly the same way. **Both are now fixed — see §8 P0.**

---

## 7. The bottleneck chain

Ordered by what binds first as you push density. **Raising anything below the first live entry changes
nothing visible** — which is exactly the reported symptom.

### Worked example: one dense PT_PARTICLE emitter, BurstCount 20 / BurstDelay 0.05 s / LifeTime 2 s
Authored steady-state ≈ **800** live particles.

| # | Wall | What happens | Survivors |
|---|---|---|---|
| 0 | `sLodMask` (distance) | At LOD3 only index&3==0 spawns → a burst of 20 yields 5 | ÷4 past ~72 m |
| 1 | `sParticleCache` 127/emitter/frame | Replay truncates; the **newest** particles are lost | **127** |
| 2 | `AddParticle` 200/batch | 127 < 200 → all pass | 127 |
| 3 | `CacheParticle` 300 | 127 < 300 → all pass | 127 |

**Now chain two such emitters in one `.fx` (the normal case for smoke/fire):**

| # | Wall | What happens | Survivors |
|---|---|---|---|
| 1 | 127 × 2 emitters | | 254 |
| 2 | `CacheParticle` 300 | 254 < 300 → pass | 254 |
| 3 | **`AddParticle` 200** | one `SubmitTexture`, 254 into a 200-slot batch → **54 dropped, and the sort makes them the NEAREST to camera** | **200** |
| 4 | **`IsFull()` now true** | the **next whole PEO in the frame is skipped**, and it never flushes, so the cascade continues | 0 for that effect |

That is the wall, and it is reached with **two** dense emitters.

### The chain, in order

1. **`RedParticleRenderer` 200/batch + `IsFull()` whole-effect skip** — *the same constant, two sites.*
   Binds at ~200 particles of one material per top-level effect object. This is the real ceiling today, and
   the `IsFull()` half converts a partial thinning into a whole-effect dropout **plus a cascade**, because
   the early return skips the only code that drains the caches.
   *modtools `0x00824E18` / `0x00824DF3`; Steam `0x006D0C6C` / `0x006D30A2`.*

2. **`sParticleCache` 127 per emitter per frame** — binds any single dense emitter *below* the 200 wall.
   For one-emitter effects this is first in the chain. It is why `MaxParticles > 127` in an `.fx` never
   produces a denser-looking burst.
   *modtools `0x006691F0` / `0x00669321`; Steam `0x0060AAD5` / `0x0060ABA5`.*

3. **`sLodMask` decimation** — cuts 25/50/75 % *at spawn*, so it multiplies every downstream limit's
   effective severity and cannot be recovered by any renderer change. Cross-cutting with 1 and 2.
   *modtools table `0x00AD6354`; Steam table `0x0078AA94`.*

4. **`s_caches` 15 batches** — binds on *material variety*, not count. Once tripped, `currentCache = NULL`
   and **everything** submitted afterward vanishes, from every system. Raised to 120 today, but only via
   `GCVisualLimits`.
   *modtools `0x00824D3D`; Steam `0x006D32EA`.*

5. **`sCachedParticles` 300** — the one the current patch raises. Binds only once 1 is lifted, and only for
   effects with 3+ chained emitters sharing a texture (3 × 127 = 381 > 300).
   *modtools `0x0066D828`; Steam `0x0060E709`.*

6. **`mMaxParticles` 128 lifetime clamp** — binds **duration**, not density. Long-lived emitters stop and
   are destroyed early. Independent of 1–5; fix it if effects cut out, not if they look thin.
   *modtools `0x006681AC`+`0x006681B8`; Steam `0x0060A23F`.*

7. **`ParticleEmitterObject` pool = 192** — binds the number of *concurrent effect instances* in the world.
   In sustained heavy combat the 193rd explosion/impact simply never spawns, silently.
   *modtools `0x0073462E` region; Steam `0x00531510` region.*

8. **`RedLodManager` class-3 pop cap (400) / heap (300 non-uber on Steam)** — binds how many effect objects
   reach the transparent list per view at all. Load-adaptive and screen-area-ordered, so it produces
   frame-to-frame flicker of whole effects rather than thinning. **Addresses not located in either target —
   this could be sitting above item 1 in real scenes and we cannot yet say.**

9. **Distance culls** (`mMaxDist` 50/150/500, near-scene fade at `lodDistance = 0` ≈ 120 m) — content and
   settings issues rather than engine caps, but they remove whole effects and no patch above helps.

---

## 8. Recommended patch plan

Ordered by impact per unit of risk. **P0 is a safety fix, not an improvement.**

### P0 — Rebase the Steam/GOG `FlushParticleCache` sort pool (crash fix) — **IMPLEMENTED**

**The problem:** the retail entries raise the `CacheParticle` cap to 1200 while the sort pool is
EBP-relative and does not move with `SUB ESP`. See §6.5 for the overwrite mapping.

**What shipped:** rather than disarming the feature, the pool was moved. The base goes from
`EBP−0x988` to `EBP−0x25A8`, which holds 1201 records (indices 0..1200, `1201×8 = 0x2588` bytes)
ending exactly at the lowest local, `EBP−0x20`. The pre-existing `SUB ESP 0x980 → 0x25A0` entry
already covers it: ESP lands at `EBP−0x25AC`, giving the new base the same 4 bytes of slack the
vanilla frame had. All ten disp32 fields per build (§6.5) are rewritten `0xFFFFF678 → 0xFFFFDA58`
and `0xFFFFF67C → 0xFFFFDA5C`.

Nothing else needed patching: the render loop and the heap-pop helper (Steam `0x0060EA00`) reach
the pool through `mPool` at `EBP−0x18`, which the rebased `LEA` writes.

**Verification:** each of the 20 entries was re-parsed from `patch_table.cpp`, converted back to a
VA, and checked against the bytes in the Ghidra image — 20/20 matched, with the `+0`/`+4` field
relationship preserved. Not runtime-tested.

**Type:** `patch_table` byte patches (no installer hook needed). **Memory:** none — stack only,
~9.6 KB per `FlushParticleCache` call, the same as modtools has always used.

---

### P0b — `RenderAllCaches` pool-state leak on mesh-acquisition failure — **IMPLEMENTED**

A stock engine bug, independent of the cache patches, but one they make easier to reach.

`RenderAllCaches` acquires a dynamic mesh per cache. If that returns NULL it branches straight to the
epilogue — **past** the two stores that end the function:

```
modtools 0x0082775F  CALL 0x0084AF80          ; acquire dynamic mesh
         0x00827766  TEST ESI,ESI
         0x00827768  JZ   0x008278A5          ; bail
         0x00827899  XOR  EAX,EAX
         0x0082789B  MOV  [s_cacheIndex],EAX  ; skipped
         0x008278A0  MOV  [currentCache],EAX  ; skipped
         0x008278A5  POP EDI ...              ; bail lands here
```

`s_cacheIndex` therefore keeps whatever height it reached and `currentCache` stays stale. On the
following frames `SetCurrentCache` starts from that height, hits its allocation clamp, and sets
`currentCache = NULL` — after which **every `SubmitParticle` in the game silently no-ops** until some
later `RenderAllCaches` completes in full. Global, persistent, and invisible in any log.

**Fix:** retarget the branch from the epilogue to the reset block. The failure path then drops that
frame's particles — which it did anyway — without poisoning the next, and falls through into the same
epilogue. The reset block only zeroes two globals, so entering it early is safe.

| Build | `JZ` | rel32 at | Old → new | Target moves |
|---|---|---|---|---|
| modtools | `0x00827768` | `0x0082776A` (file `0x42776A`) | `0x137` → `0x12B` | `0x008278A5` → `0x00827899` |
| Steam | `0x006D3131` | `0x006D3133` (file `0x2D2533`) | `0x136` → `0x122` | `0x006D326D` → `0x006D3259` |
| GOG | `0x006D41D1` | `0x006D41D3` (file `0x2D35D3`) | `0x136` → `0x122` | `0x006D430D` → `0x006D42F9` |

Shipped as the `Particle Cache Reset Fix` set, `[Fixes] ParticleCacheResetFix`.

**Unproven:** the mesh acquisition has not been observed failing. The code path and its consequences are
verified; the trigger *rate* is not, so this may be latent rather than active. Worth having either way —
the failure mode is total particle loss.

**Type:** one rel32 per build. **Memory:** none. **Risk: low.**

---

### P1 — Neutralise the `IsFull()` whole-effect skip — **IMPLEMENTED**

*Highest impact per byte in the plan.* Shipped as the `Particle Effect Skip Fix` patch set, toggled by `[Fixes] ParticleEffectSkipFix`.

**Sites (imm32 of the `CMP … ,0xC8`):**

| Build | Address | Orig | New |
|---|---|---|---|
| modtools | `0x00824DF3` (file `0x424DF3`) | `C8 00 00 00` | `FF FF FF 7F` |
| Steam | `0x006D30A2` (file `0x2D24A2`) | `C8 00 00 00` | `FF FF FF 7F` |
| GOG | `0x006D4142` (file `0x2D3542`) | `C8 00 00 00` | `FF FF FF 7F` |

The cascade was confirmed directly on modtools: `Render_`'s early-return target `0x0066B793` is the
epilogue (`POP ESI; ADD ESP,0x44; RET 0xC`), which sits **past** the `FlushParticleCache`
(`0x0066B77F`) and `RenderAll` (`0x0066B784`) calls that end the function. A full batch therefore
skips the drain that would have emptied it, so it stays full and every later effect in the pass
takes the same early return.

**Coupled constants:** none. This is safe by construction — `AddParticle` independently caps
`m_particleIndex` at 200, so the batch array can never overrun; `m_numVerts`/`m_numIndices` are only
incremented on a successful `AddParticle`; and `IsFull` has **exactly one caller** in every build (verified
by xref). After the patch, a PEO arriving at a saturated cache renders what fits and **still reaches its
`FlushParticleCache(); RenderAll();` tail — which drains the caches**, breaking the cascade so the next PEO
gets a clean slate.

**Memory:** 0 B. **Type:** plain `patch_table` byte patch. **Risk: very low.**
**Expected effect:** whole effects stop blinking out; flush cadence is restored; strictly more particles,
never fewer.

*(Equivalent 3-byte alternative: stub `IsFull` to `XOR AL,AL; RET` at modtools `0x00824DE0` / Steam
`0x006D3090`. Pick one, not both.)*

---

### P2 — Unmask the `sLodMask` decimation — **IMPLEMENTED** (default OFF)

Shipped as `Full Particle LOD`, `[Features] FullParticleLOD`, **default 0**.

Two `bool[4][4]` tables, indexed `[currentLod][bucket]`, verified through their only two readers:

```
IsLodActive   0066d794  MOV ECX,[0x00b9db2c]                 ; current LOD 0..3
              0066d79a  MOV AL,[EAX + ECX*0x4 + 0xad6354]    ; sLodMask[lod][bucket]

GetLodAlpha   0066d7ba  MOV DL,[EAX + ECX*0x4 + 0xad6364]    ; sLodFadeMask[lod][bucket]
              0066d7c3  JZ  -> full alpha
              0066d7cb  FMUL [0x00b9db30]                    ; else alpha *= fade
```

Stock contents, **identical in all three builds** (byte order as stored, low address first):

| Row | `sLodMask` | Effect | `sLodFadeMask` |
|---|---|---|---|
| LOD0 | `01 01 01 01` | all four buckets spawn | `00 00 00 01` |
| LOD1 | `01 01 01 00` | 25 % culled | `00 01 00 00` |
| LOD2 | `01 00 01 00` | 50 % culled | `00 00 01 00` |
| LOD3 | `01 00 00 00` | 75 % culled | `00 00 00 00` |

The fade table marks the bucket the *next* LOD will cull, so it cross-fades before dropping. That is why
the two tables must move together: unmask a bucket and leave its fade bit set and those particles spawn
ghost-faded, then pop to full alpha at the boundary — visually worse than the cull.

Six dword writes per build: mask rows LOD1–3 → `0x01010101`, fade rows LOD0–2 → `0`. Row LOD0 of the
mask and row LOD3 of the fade are already correct.

| | modtools | Steam | GOG |
|---|---|---|---|
| `sLodMask` base | `0x00AD6354` | `0x0078AA94` | `0x0078BA3C` |
| `sLodFadeMask` base | `0x00AD6364` | `0x0078AAA4` | `0x0078BA4C` |

Expected dword values as patched: `0x00010101`, `0x00010001`, `0x00000001` (mask LOD1–3) and `0x01000000`,
`0x00000100`, `0x00010000` (fade LOD0–2).

> An earlier revision of this document listed those constants **byte-reversed**, which would have failed
> every site verification. They are as above.

**Steam/GOG caveat:** the tables are in `.rdata`. Fine from `apply_patches`, which runs inside `dllmain`'s
`PAGE_READWRITE` window; a *runtime* toggle would need its own `VirtualProtect`. modtools' copy is in
`.data`.

**Memory:** 0 B. **Risk: low** — indices are hard-bounded by the callers. **Cost:** up to 4× the particles
at distance, which is why it is off by default.
**Prerequisite:** worth little without `ParticleEffectSkipFix` and `ParticleBatchSpill`, or the recovered
particles are re-dropped at the 200-per-batch wall.
---

### P3 — Decouple the batch-cache work from `GCVisualLimits` — **IMPLEMENTED**

The `s_caches` clamp raise and the `SubmitParticle` spill detour used to live inside
`gc_visual_limits_install()`, behind **two** unrelated gates: the `GCVisualLimits` INI key, and that
function's `verify_and_apply()` early return over ~16 galaxy-map patch sites. Either one failing silently
reverted the batch-cache behaviour for *every particle in the game*, while the install log still reported
success.

**What shipped:** both halves moved to `PatcherDLL/src/render/particle_batch_spill.cpp`, installed from
`dllmain` ahead of `gc_visual_limits_install()` and gated only by its own `[LimitIncreases]
ParticleBatchSpill` key. It still reads the live `SetCurrentCache` base operand to decide whether the
`Particle Cache Increase` redirect is active, so it works with that patch set on (120 slots) or off (the
stock 15). The GC map is now a consumer: its log line reports the slot count via
`particle_batch_cache_slots()` and its stats line pulls counters via `particle_batch_spill_stats()`.

Two defects found while moving it:

* `particle_batch_spill_uninstall()` had to be wired into `lua_hooks_uninstall()` alongside
  `gc_visual_limits_uninstall()`. That path *is* live (DLL detach), so moving the detour without moving
  its detach would have left `SubmitParticle` hooked into an unloading DLL.
* On a clamp-immediate mismatch the old code logged the problem but left `g_cacheSlots` at 120, so the
  spill would hand out slots past a clamp still set to 15 — `SetCurrentCache` would then NULL
  `currentCache` and kill all particle rendering. It now falls back to 15 to match the clamp it could not
  raise.

The `RENDERER_CACHE_SLOTS ≤ 127` guard is now a `static_assert`. The clamp is an imm8 read by a **signed**
branch (modtools `0x00824D3B: CMP EDX,0xF` / `JL`), so a count above 127 sign-extends negative, the
free-slot test never passes, and nothing is drawn at all.

**Memory:** 0 B new. **Type:** DLL source change. **Risk: low** — but not runtime-tested.

---

### P4 — Raise `mMaxParticles` from 128 — **IMPLEMENTED** (default OFF)

Shipped as `Emitter Particle Budget`, `[Features] EmitterParticleBudget`, **default 0**, raising the clamp
to 1024.

`ParticleEmitter::mMaxParticles` (short at emitter+0x2) is the emitter's *lifetime* budget, read from the
.fx and clamped as it is stored. Effects asking for more are silently cut short.

| Build | Site | Encoding |
|---|---|---|
| modtools | `0x006681AC` **and** `0x006681B8` | two imm16 — `CMP AX,0x80` (`66 3D 80 00`) and `MOV word[ESI+2],0x80` (`66 C7 46 02 80 00`) |
| Steam | `0x0060A23F` | imm32 of `MOV EDX,0x80`; `CMP AX,DX` and the clamp store both read it |
| GOG | `0x0060B2CF` | identical to Steam |

modtools carries the constant twice and a 32-bit store at `0x006681AC` would run into the following
`MOV word ptr [ESI+2],AX`, so `patch_flags` gained a `values_are_16bit` width.

**Safety, verified rather than assumed:** the spawn loop bounds its write cursor against the end of
`sParticleCache` on *every* iteration, independently of this value — modtools `0x00669320: CMP
EAX,0xB9D12C / JGE exit`. A larger budget therefore cannot overrun the array; it only lets the emitter keep
spawning across more frames. The per-frame 127 gate at `0x006691EC` is separate and still holds.

**Hard ceiling:** the field is a signed short and every comparison is 16-bit signed, so a value ≥ `0x8000`
reads negative and every emitter dies on its first burst.

> **Escape hatch that needs no patch at all:** `0xFFFF` (−1) is the engine's "unlimited" sentinel —
> `0x006691DB: CMP AX,0xFFFF / JZ` skips the budget test entirely. It also survives the load-time clamp,
> because `CMP AX,0x80` is signed and −1 compares as less. Content can set `MaxParticles` to −1 today and
> get an unbounded lifetime budget on stock BF2.

**Memory:** 0 B. **Risk: low.** Off by default because existing maps were authored against the 128 clamp,
so unclamping changes how their effects look and cost.
---

### P5 — Raise the per-emitter 127 cap (installer hook required) — **DEFERRED**

> Held deliberately. It is the only item here needing a code cave and a relocated array, it is rated
> medium-high risk, and it would stack on six changes that have had no runtime testing yet. The
> analysis below stands; do it once the cheaper items are confirmed working in game.

This is the real density knob for a single dense emitter, but it is the most invasive change here.

**Why it cannot be a byte patch:** the outer guard is `CMP r/m32, imm8` in both targets, so **127 is the
maximum representable value**. `0x80`–`0xFF` sign-extend negative and **disable the bound entirely**
(unbounded overrun of a global array). Re-encoding to imm32 does not fit — modtools has 11 bytes available
and needs 13; Steam has 9 and needs 12. Any N > 127 requires a 5-byte `E9 rel32` detour into a code cave.

**Also:** the array cannot grow in place beyond ~161 slots (modtools, next live global `0x00B9D9A4`) or
~167 (Steam, next live global `0x01EF5120`). Beyond that it must be relocated.

For `N` particles, `SLOTS = N+1`, `NEWBASE` = fresh `SLOTS×0x40` allocation:

**modtools:** rewrite imm32s at `0x00669309` (→`NEWBASE+0x1C`), `0x00669315` (→`+0x3B`), `0x00669321`
(→`+N·0x40+0x1C`), **`0x0066932C` (→`+SLOTS·0x40+0x1C`, MANDATORY)**, `0x00669843` (→`+0x3B`),
`0x00669958` (→`+0x2C`), `0x006699C7` (→`+0x00`); detour the outer guard at `0x006691EC` (11 bytes).

> `0x0066932C` is a trap: in stock code the `CMP EAX,0x00B9D16C` (`base+128·0x40+0x1C`) is dead because the
> 127 break fires first. Raise the 127 without raising this and every slot ≥128 falls into the
> `RedWarning("Spawning too much at once")` branch — a per-frame log flood and **zero extra particles**.

**Steam:** rewrite imm32s at `0x0060AB96` (→`+0x1C`), `0x0060AB9D` (→`+0x3B`), `0x0060ABA5`
(→`+N·0x40+0x1C`), `0x0060AF6F` (→`+0x3B`), `0x0060B013` (→`+0x2C`), `0x0060B09E` (→`+0x00`); detour the
outer guard at `0x0060AAD3` (9 bytes). **Steam has no second pointer compare** — the warning branch was
compiled out. Do not look for one.

**Memory:** N=512 → 32 KB. **Type:** installer hook + code cave. **Risk: medium-high.**
**Prerequisite:** worthless without P1 and the spill detour — 127→512 just re-drops at the 200 wall.

---

### P6 — Raise the `ParticleEmitterObject` pool above 192 (optional)

Fixes "explosions stop having smoke" in sustained combat. Two routes:

* **Mission Lua** (no code patch): `SetMemoryPoolSize("ParticleEmitterObject", N)` before the first
  particle effect. Legitimate and already supported by the engine.
* **Binary**: the `PUSH 0xC0` inside the `MemoryPool::Setup` sequence at **modtools `0x0073462E`** /
  **Steam `0x00531510`**. The imm32 offset within the sequence is **inferred, not read** — re-derive it by
  disassembling on the live binary before writing.

**Memory:** 272 B per additional slot (N=512 → +87 KB). **Type:** `patch_table` once the offset is verified.
**Risk: low** — `MemoryPool::Create` sizes the slab from `mCount`, and the gate is a pure comparison.

---

### Deliberately NOT recommended

* **Raising `AddParticle`'s 200 in place.** `m_particleCache[200]` is at struct offset 0 and
  `m_particleIndex` sits at `0x3520` immediately after the last element. Writing slot 200 lands on the batch
  header and then on `s_caches[i+1]`. A real raise means relocating the whole array *and* rewriting ~40–60
  disp32 field operands across six functions plus four stride constants plus two **pre-biased** base
  constants (modtools `0x00824D1F` = base+0x3528 and `0x0082770A` = base+0x3530; Steam `0x006D32CC` and
  `0x006D30DC`). Missing a folded base leaves lookup and allocation walking different buffers — a silent
  failure, not a crash. **The existing spill detour achieves the same result without any of this.**
* **`rendering.minScreenSize` / `rendering.maxVertexDensity`.** Particle effects branch away before those
  tests. Confirmed negative result.
* **`MinLODDist` / `MaxLODDist` in `.fx` files.** No reader exists. Use `BoundingRadius` instead.

---

## 8b. What actually shipped, and where it lives

The plan above is written as discrete patches. Several were merged on the way in, so this is the
as-built map.

### Runtime layout

| Concern | Lives in | Toggle |
|---|---|---|
| Sort-cache size + retail pool rebase | `patch_table.cpp` set `Particle Cache Increase` | `[Particles] ParticleFixes` |
| `IsFull()` whole-effect skip | `patch_table.cpp` set `Particle Effect Skip Fix` | `[Particles] ParticleFixes` |
| `RenderAllCaches` state leak | `patch_table.cpp` set `Particle Cache Reset Fix` | `[Particles] ParticleFixes` |
| Batch-cache pool + spill detour | `render/particle_batch_spill.cpp` | `[Particles] ParticleFixes` |
| LOD masks, LOD distance, emitter budget | `render/particle_density.cpp` | `[Particles] ParticleDensity` |

Three patch sets share the one `ParticleFixes` key on purpose: `ini_lookup_patch_set` resolves each set
name to the first registry row naming it, and `generate_ini.py` dedupes on `(section, key)` so the key
is emitted once.

### The density dial

P2 (LOD masks) and P4 (emitter budget) are no longer byte-patch sets. They are generated at install time
by `particle_density.cpp` from one level, because the mask and fade tables must stay consistent and
hand-maintained hex had already produced one byte-reversed error in this document:

| Level | Buckets per LOD | LOD numerator | Emitter budget |
|---|---|---|---|
| 0 stock | 4 / 3 / 2 / 1 | 4.0 (stock) | 128 |
| 1 balanced | 4 / 4 / 3 / 2 | 2.0 (LOD starts 2x further out) | 1024 |
| 2 maximum | 4 / 4 / 4 / 4 | 4.0 (stock) | 1024 |

Both tables come from `fade[L][b] = mask[L][b] && !mask[L+1][b]`, which the stock data satisfies, so
level 0 reproduces the shipping bytes exactly.

### LOD distance: repoint, never edit

The LOD curve numerator is a shared literal (modtools `0x00A2A0BC` = 4.0, ~90 xrefs across the engine
including turn speeds, sound distances and `RGBtoHSV`). The **operand** of the instruction that reads it
is repointed at a DLL-owned float instead, so exactly one call site changes:

| Build | Instruction | disp32 operand |
|---|---|---|
| modtools | `D8 3D` `FDIVR dword ptr [0x00A2A0BC]` @ `0x0066D5B2` | `0x0066D5B4` |
| Steam | `F3 0F 10 15` `MOVSS XMM2,[0x007B2238]` @ `0x0060E550` | `0x0060E554` |
| GOG | `F3 0F 10 15` `MOVSS XMM2,[0x007B31B0]` @ `0x0060F5F0` | `0x0060F5F4` |

The installer verifies the operand currently reads 4.0 before repointing, and declines with a log line
otherwise.

### Field status

Confirmed in play on modtools (`BF2_modtools_NoDVD_NoConsole.exe`): particles stay, no dropouts, no
crashes. Density level 2 caused frame drops, which is what level 1 exists to solve. **Steam and GOG
remain unverified in play** — every site is byte-verified against their images, but no retail session
has been run.

---

## 9. Open questions and what could not be verified

**Blocking for a complete picture:**

1. **`RedLodManager` class-3 pop cap in the targets.** Phantom pops only the top **400** particle effects
   per view (`MOV EDX,0x190` @ `0x008D04B8`) and zeroes the heap. Byte-pattern searches for that constant
   find nothing in either target — the codegen differs. Steam's `SetClassMaxCost` is *conditional* on
   `Init(isUberMode)` (heap 300 non-uber, verified from raw bytes at `0x006BC86B`–`0x006BC89E`), which
   suggests the pop cap may also differ. **If this binds first, none of P1–P6 will fully fix the symptom.**
   This is the single highest-value remaining investigation.

2. **GOG binary not loaded.** The EBP stack-overrun is *confirmed* on Steam and matches Phantom's codegen
   exactly. GOG is inferred from the identical `0x980` constant and the patch table's own comment.
   Verify at GOG file offset `0x20EC1A` (VA `0x0060F81A`) before shipping any fix.

3. **Retail `CacheParticle` / `FlushParticleCache` frame layout re-derivation.** One pass reported failing
   to locate the retail flush (a `SUB ESP,0x994` hit at `0x00647D5B` was an unrelated SEH-framed function);
   a later pass located it at `0x0060E760` with an EBP frame. **Reading: the later pass is correct** — the
   ten disp32 pool accesses at `EBP−0x988` are unambiguous. But the exact new displacement for a resize
   must be measured on the binary, not computed from this document.

**Secondary:**

4. **modtools `RenderAsEmitter` 31-child cap not located.** Six encodings searched across
   `0x00668320`–`0x00668C30` with no hit; the counter lives in a stack slot. Steam is confirmed at
   `0x0060B346`.

5. **Shipped `.fx` content.** No pass could read shipped assets. It is *proven* that the `mMaxParticles`
   clamp is unconditional, silent and on the live path, and that `-1` is deliberately special-cased — but
   not proven that any stock Pandemic effect authors > 128. For mod content the ceiling is trivially reached.

6. **Whether any shipped mission Lua calls `SetMemoryPoolSize` for the particle pools.** If it does, the
   in-binary 192 is not the operative cap.

7. **Per-view vs per-frame.** `RedLodManager::Render` clears each class heap at its end, which is consistent
   with per-view. If it is per view, every count-based cap here (400 pops, 1024 transparent, 200/cache,
   300 sort cache) resets per split-screen viewport. Not confirmed.

8. **Dynamic vertex-buffer depth.** `RenderAll` bails on `BeginVertexData == NULL` **without resetting
   `s_cacheIndex`/`currentCache`** (modtools `0x00827768`, Steam `0x006D3131`, Phantom `0x008B1237`). Raising
   the cache count from 15 to 120 increases exposure to this eightfold. `m_maxVertices` for the particle
   vertex formats (`0x282` / `0x2A2`) was never measured. Watch for whole-batch blackouts reappearing after
   raising counts — that would indicate the VB limit has been traded for the cache limit.

9. **`RenderAll` never null-checks `BeginIndexData`** — only the vertex pointer is tested. Whether an index
   lock can fail while the vertex lock succeeds was not determined; if it can, `AddParticleToBuffers`
   receives a NULL pointer and faults.

10. **`ParticleSpawner::mNumSizes`/`mNumColors`/`mNumRotations`** are hard-set to 1 in the ctor and only
    index [0] is read, but `PE_ChunkLoader::LoadSpawner` was not fully traced. If `mNumColors` could exceed
    1, the colour loops would write past `Particle::mColor[0]` into `mLifeTime`/`mLod`/`mInfoData` —
    `Particle` is a fixed 64 bytes with no room for a second `RedColorValue`. Check before shipping any
    patch that touches the spawner loader.

11. **`ParticleTransformer` sub-transformer array index is unchecked** in `LoadTransformer` (Phantom
    `0x0071B03A`). Arrays hold exactly 1; a non-zero index from the `.fx` scribbles into `mNumSizeTrans` /
    `mNumColorTrans` / `mGeometry`. Also, `ParticleTransformer::ParticleTransformer` (Phantom `0x00709370`)
    **never initialises `mLifeTime` at +0x1C** — an `.fx` with a `Transformer()` scope omitting `LifeTime`
    gets uninitialised heap as its particle lifetime.

12. **`OrdnanceXxx::Build` pushes `0x168`** against an `Ordnance` pool item size widened only to `0x144` —
    a possible 36-byte overrun of every allocation from that path. Not exhaustively checked; a later
    `SetSize` may raise it. Adjacent system, flagged in passing.
