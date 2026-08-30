# Multiplayer safety: BF2's authority model and every BF2GameExt patch

Investigated 2026-08-25. BF2's netcode had never been examined on this project before
this pass, so treat the model itself as first-pass rather than settled - section 5 marks
what is verified against a shipping build.

It also surfaced four defects in our OWN code; see section 4, 'Fix in the DLL'.

---

# BF2GameExt in Multiplayer — Safety Guide

Builds referenced: **modtools** = `BF2_modtools_MemExt.exe`, **Steam** = `BattlefrontII.exe`, **GOG** = `BattlefrontII_MemExt.exe`. Addresses are VAs on the named build. `Battlefront2.exe` (Phantom, 2026 dev rebuild with PDB) is reference only and is never cited alone for a shipping claim.

---

## 1. The authority model in two sentences

**BF2 is server-authoritative with client-side prediction, not lockstep**: the only authoritative simulation in the game is the fixed-timestep turn loop inside `GameLoop::Update` (Steam `0x005323E0`, modtools `0x00735A20`, GOG `0x00533140`), which exists only on the `NetGame::IsHost()` arm — a pure client has no turn loop at all, uploads raw pad state via `SubmitMove` (Steam `0x005B9620`), and re-simulates only what it owns in `NetGame::Predict` (Steam `0x005BA810`). There is no checksum, no version-of-content exchange, and no desync detector anywhere in the join handshake (`ReadShellUpdate`, Steam `0x005E6690`, carries only mission-name hashes), so a client that computes something differently mispredicts *itself* and gets overwritten by the next host snapshot — it cannot corrupt anyone else's world.

It is mixed per subsystem, and the split matters:

| Subsystem | Authority |
|---|---|
| Damage & death | **Host only, unforgeable.** `Damageable::SetCurHealth` (Steam `0x00488820`) clamps any client-side write of ≤ 0 to `+1e-4`. |
| Your own soldier | **Predicted** — fully re-simulated every predicted turn, reconciled against the host snapshot. `NetGame::IsSkipped` (Steam `0x005B7D70`, modtools `0x006E48F0`) returns "do not skip" when any controllable has `GetJoystickIndex(mPlayerId) >= 0`. |
| Other players' soldiers, sim | **Coarsely extrapolated on the client.** `IsSkipped` returns "do not skip" only on catch-up turns and substitutes an aggregated delta (`DAT_01FA6588`); on other turns the object is not stepped at all. |
| Other players' soldiers, render/targeting | **Interpolated one turn behind.** `EntitySoldier::GetSmoothedMatrix` (Steam `0x005EB3A0`, modtools `0x0071D700`) → `GameObjectInterpolator::GetMatrix` (Steam `0x00537460`): position LERP + quaternion SLERP weighted by `NetGame::GetTurnRatio` (Steam `0x005BA740`). Consumers are not render-only — `UpdateTargetVisibility` (modtools `0x00454E8C`) and `UpdateWeaponEvents` (modtools `0x006B3E4E`) are in the caller list. |
| AI | **Host only.** `EntitySoldier::SetupController` (Steam `0x004F0830`) and `EntityDroid::SetupController` (Steam `0x0049C900`) allocate no controller at all for `mPlayerId < 0` on a net client. A MP client runs **zero** AI. |
| Host input handling | **Selectable.** `netWaitLate` (modtools `0x00BE14D5`, Steam `0x01E62F50`, image byte = 0) chooses between rollback (default: force-fill missing input, then `ApplyLateMoves` Steam `0x005BA110` re-simulates from an 8-turn state ring at `&DAT_01EE5280 + (T&7)*0x1C`) and a 3-turn all-players-present barrier inside `NetGame::IsTurnReady` (Steam `0x005B99D0`). The two are mutually exclusive. |

Timing: `netTurnsPerSecond` defaults to **20** (`NetGameP::SetTurnRate` Steam `0x005B8790`, host floor constant `0x007E8E94` = 20), so `netSecondsPerTurn` = 50 ms; `GetMaxSimTurns` (Steam `0x005B99C0`) literally returns 4; prediction horizon `MAX_PREDICT_STEPS` = 13 turns ≈ 0.65 s.

---

## 2. Does the host need the identical patch set?

**No — but every client needs at least the host's limit-raising patches, and the host's gameplay patches silently become the server's ruleset for everyone.** The three reasons a patch could need to match are not equally real here:

### (a) Wire format — the smallest risk, and nothing in the current set touches it
The protocol is a pure bit-packed stream with no per-field sync marks (`NetPktGroup` only does `MarkWriteAlign` between whole objects) and no object id inside `EntitySoldier::WriteObject` (modtools `0x00724320`) / `ReadObject` (modtools `0x00724FE0`, Steam `0x005EA140`). One width disagreement corrupts everything after it in the update. The hard ceilings, all verified:

- entity class index **8 bits → ≤ 254** (`WriteCreate` modtools `0x007027C0`)
- player id **6 bits → ≤ 63**; slot ids **6 bits with `0x3F` as sentinel → 63 replicated objects per client** (`WriteObjects` modtools `0x00700770`)
- soldier `mState` **4 bits**, weapon index **3 bits**, health/shield **11 bits each**
- **800 bytes** of object state per client per update (`netUpdateSize` = 1024, modtools `0x00ADABA8`)

None of the shipped patches widens any of these. But note the live tripwire is not a limit at all: **field presence is decided by each side's own local data.** `ReadObject` reads 12 bits of shield only `if (0.0 < this->Damageable.mMaxShield)` on the *receiver's* copy, and quantises `mCurHealth` against the *receiver's* `mMaxHealth`. A host/client **content** mismatch (different ODFs for the same unit) desynchronises the bitstream mid-object. That is a mod-distribution requirement, not a DLL one — but it is the failure mode people will blame the DLL for.

### (b) Simulation divergence — real but self-correcting; it costs feel, not integrity
There is no consensus, so nothing "desyncs" in the lockstep sense. The host's copy of a gameplay rule is the only one that decides outcomes; a client running a different rule mispredicts its own soldier and is snapped back within 50 ms. Two patches are genuinely in this class — **Chunk Push Fix** (Steam `0x004E19B0`, patch at `0x004E1A24`: `5E 8B E5 5D` → `EB 25 90 90`, which lets a chunk-rolling soldier also receive the push, writing `mVelocity` and possibly `SetState(TUMBLE)` / `mInputLockTime`) and **Soldier Height Ceiling Removal** (Steam `COMISS XMM3,[0x007B23C8]` = 1000.0f at `0x004E96D8`, six-byte `JA` at `0x004E96DF` NOPed). Both are ungated by net role, so an unpatched client *does* run the vanilla rule locally and will visibly disagree until corrected.

Related, and worth one line: `NetGame::Create` deliberately reseeds the **simulation RNG to zero** when a net game starts (modtools `0x006E1ECC` → `0x00E5F570`; Steam `0x005B60FC` → `0x009CEF48`; GOG `0x005B709D` → `0x009D03E8`). That is the signature of a design that expected machines to draw the same values in the same order. It no longer has to, because snapshots overwrite — but it means any patch that adds or removes a sim-RNG draw degrades prediction quality on a client, which is felt as rubber-banding.

### (c) Content parsing / capacity — **this is the one that actually breaks people, and it is worse than "crash"**
The host chooses the map. Every client loads that map itself. BF2's hash primitives have no capacity guard: `PblHashTableCode::_Store` (Steam `0x00726F60`) probes backward with wraparound and exits only on an empty slot or a key match — **full table + new key = infinite loop**. `_Find` (Steam `0x00726E00`) is the same shape.

`EntityEx::mIdMap` (Steam `0x01EB9874`) holds **1024** entries keyed by hashed instance name (`EntityEx::EntityEx` Steam `0x00491720`, `_Store(..., 0x800, ...)`). The net-client skip in that constructor does **not** protect a client: `netOnClient` (Steam `0x01E62EAB`) has exactly two write xrefs in the whole image — `EnterClient` `0x005B75E0` and `LeaveClient` `0x005B7D45` — it is a transient scope flag around the predict/receive window, and it is **false during level load**. A vanilla client joining a patched host's over-1024-entity map wedges inside `_Store` at load: 100% of one core, no exception, no dialog, no log line, and there is no engine-side overflow diagnostic anywhere in the image. The player must kill the process, and will report it as a download or connection problem.

**The rule to state to users:** *the host's patch set defines the minimum patch set every client must already have.* Gameplay rules only matter on the host. Presentation patches are free on either side.

---

## 3. Every patch and INI feature

Labels answer one question: **what happens if only some machines have it.**
`SAFE` = mismatch is fine · `MATCH REQUIRED` = everyone needs it or content breaks · `ADVANTAGE` = no desync, but an edge over unpatched players · `UNSAFE` = can desync, crash a peer, or corrupt shared state · `UNKNOWN` = not established.

### Byte patches

| Patch | Verdict | Reason |
|---|---|---|
| Object Limit Increase | **MATCH REQUIRED** | Hardest case in the set: `mIdMap` is 1024 unguarded hash slots; an unpatched client **hangs at level load** on a >1024-named-instance map (Steam `_Store` `0x00726F60`, `mIdMap` `0x01EB9874`). Does not touch the wire — the net `objMap` is a separate 256-bucket table (modtools `0x00BE2484`). |
| String Pool Increase | **MATCH REQUIRED** *(and non-functional on retail)* | Retail `StringDB::Add` (Steam `0x00651F60`) has no bounds check at all — `mPoolSize` (`0x01EAFB98`) has exactly one xref, the write in `Init`. Over 32 KB of localise strings is a straight heap overflow. **Our Steam/GOG entries are mis-aimed**: they patch VA `0x0053B143`/`0x0053BE93` (`PUSH 0x1770` → `InitPool` `0x006DB770`, the glyph cache), not `StringDB::Init` (Steam `0x00651ED0`, GOG `0x00652F70`). Only the modtools entry works. |
| SkyObjectClass Limit Extension | **MATCH REQUIRED** | `FUN_00638D50` (Steam) stores into `&DAT_01EAF06C + count*4` with no bound, on both the success **and** the `operator new` failure path. Our patch only neutralises the success-path increment (`0x00638D9E`), leaving `0x00638DC9` live. Sky data is map content each peer loads itself. |
| RedMemory Heap Extensions | **MATCH REQUIRED** | Heap sizing for map content; an unpatched peer exhausts on exactly the content the host chose. |
| LOD Limit Extension | **MATCH REQUIRED** | Map-driven model capacity; unpatched peer overruns on the host's map. |
| Matrix/Item Pool Limit Extension | **MATCH REQUIRED** | Map-driven pool. **Also carries our own arithmetic bug**: Steam/GOG `0x006B028A` (`CMP ECX,0xBF6`) and `0x00407597` (`MOV EDI,0xBF5`) compare **element indices**, but the table writes a byte size — 64× too permissive. Correct values are `0xBF600` and `0xBF5FF`. |
| High-Res Animation Limit | **MATCH REQUIRED** | Content-driven animation capacity, same load-time exposure. |
| Combo Anims Increase | **MATCH REQUIRED** | Content-driven. Whether an overflowing combo index can reach the wire is **not** established (see §5). |
| DLC Mission Limit Extension | **MATCH REQUIRED** | The only limit patch that touches the join path: `ReadShellUpdate` (Steam `0x005E6690`) reads a null-terminated list of 32-bit mission ids and feeds each to `DownloadableContent::IsMissionDownloaded` (Steam `0x0048EBC0`). A client whose mission table is smaller than the host's cannot match the list. |
| Attached Effects Overflow Fix | **MATCH REQUIRED** | An overflow, not a dropout; unpatched peer takes the vanilla overflow on the same effect-heavy content. Not re-derived this pass. |
| Particle Cache Increase | **MATCH REQUIRED** | Client-local pool, but it exists because the vanilla cache is overrun by heavy content the host can serve. Not re-derived. |
| Particle Cache Reset Fix | **MATCH REQUIRED** | Same class; a reset defect on an unpatched peer is not a graceful dropout. Not re-derived. |
| Particle Effect Skip Fix | **MATCH REQUIRED** | Same class. Not re-derived. |
| PropGenerator Update Loop Exit Condition | **MATCH REQUIRED** | Fixes a loop-exit condition over map-loaded props; the unpatched peer walks the same prop array. Not re-derived — deserves the same audit as Chunk Push. |
| Soldier Height Ceiling Removal | **MATCH REQUIRED** | A simulation rule, **ungated by net role** — there is no `netOnClient` reference anywhere in `EntitySoldier::Update` (Steam `0x004E8920`) besides the `IsSkipped` early-out. An unpatched client locally `Kill()`s a soldier above Y=1000 that the patched host keeps alive; the next snapshot restores health, so the symptom is an intermittent death-flicker, not a desync. Match both sides if the map uses vertical space. |
| Chunk Push Fix | **MATCH REQUIRED** | Real physics change: writes `mVelocity`, can force `TUMBLE`/`Fly`/`mInputLockTime` (`ApplyPush` Steam `0x004E1690`). RNG draw count is unchanged (the `GetFloat` at Steam `0x004E1A01` precedes the branch on both paths), so it is not an RNG hazard — it is a movement-rule mismatch. Host's copy governs; unpatched clients rubber-band. |
| Network Timer Increase | **SAFE** | Settled in detail in §5. Raises the local UDP drain / message-dispatch rate from 30 to 120 Hz. Only controls how often *this* process services *its own* socket. Host and client need not match. |
| Explosion VisibleRadius Increase | **SAFE** | Client-local presentation. It is the `ExplosionClass` **constructor default** (60.0f → 10000.0f; modtools VA `0x0060363D`, retail `0x0051BF5F`), so ODFs that set `VisibleRadius` are unaffected. Determinism-clean: both the `_Global` effect-index draw and the sim-RNG chunk draws in `CreateExplosion` (Steam `0x0051DC10`) sit outside the visibility branch. **Caution**: the flag it flips also gates a 0x150-byte pooled `LightFlash` allocation and the camera-shake enqueue (Steam `0x0044F4C0`, 4-slot ring) — so it raises local pool pressure and shakes the camera you aim through on every un-overridden explosion. |
| Sound Limit Extension | **SAFE** | Audio pool; exhaustion on an unpatched peer is a dropped sound. |
| Audio Stream Limit Increase | **SAFE** | Same. |
| SoundParameterized Layer Limit Extension | **SAFE** | Proven graceful: `Snd::SoundLayered::SetProperties` (modtools `0x00895A20`) null-checks the layer allocator's return and unwinds cleanly. The FEATURES.md claim that unpatched peers *crash* on flyer-heavy maps is not supported by this call path and should be re-sourced or softened. |
| Lightsaber Block Direction Fix | **SAFE** | Block outcome resolves into damage, which is host-authoritative; the client's copy is inert. The host's setting is the server ruleset. |
| EntityPath Branch Region Fix | **SAFE** | AI path-region resolution. A MP client allocates zero AI controllers, so it is dead code there. Not re-derived — flagged for a sim-RNG audit. |

### AI (all host-only — `[AI]` is dead config on a multiplayer client)

| Feature | Verdict | Reason |
|---|---|---|
| AIDecisionRate | **SAFE** | Host-side AI tuning; a client allocates no AI controllers (`SetupController` Steam `0x004F0830` returns before allocation for `mPlayerId < 0` on a net client). |
| AIUpdateBudget | **SAFE** | Same — host-only, inert on a client. |
| PlayerVisionFairness | **SAFE** | Host-only; applies equally to every human player. |
| PlayerPriorityFairness | **SAFE** | Host-only; applies equally. |
| PlayerAwarenessFairness | **SAFE** | Host-only; applies equally. |
| PlayerThreatFairness | **SAFE** | Host-only; applies equally. |

### AimAssist

| Feature | Verdict | Reason |
|---|---|---|
| AimAssist **on a client** | **SAFE** (but useless — it hurts you) | What a client uploads is the raw `ProcessedInputs` block, not the derived control axes. Meanwhile `mControlTurn` (+0x88), `mControlPitch` (+0x8C), `mTurnAdjusted`, `mPitchAdjusted`, `mTurnAuto`, `mPitchAuto`, `mUsingTurnAdjusted` and `mTargetLockedObj` (+0x138) all arrive **host→client** in `EntitySoldier::ReadObject` (Steam `0x005EA140`). Your assist is overwritten every update. Net result is jitter, not an exploit. |
| AimAssist **on the host** | **UNSAFE** | `PlayerController::Update` (Steam `0x0061A2B0`) runs for **every** player with `mPlayerId >= 0`, remote included, and the `GetJoystickIndex >= 0` block inside it only guards rumble. `PatcherDLL/src/controller/aim_assist.cpp:416` (`hooked_PCUpdate`) has **no local-player gate** — the only guards are `s_aimAssistEnabled`, `g_controllerEnabled`, `is_joystick_connected()`. A host with a pad plugged in applies aim assist and auto-lock to remote players' soldiers, computed from the *host's* camera (`s_cameraGlobal`), with DLL-wide smoothing statics (lines 309–310) clobbered across players — and the result is authoritative and transmitted. This is a code defect, not a config note. **Fix: gate on `NetGame::GetJoystickIndex(controllable->mPlayerId) >= 0` in `hooked_PCUpdate` and `checkAutoLock`.** |

### Features

| Feature | Verdict | Reason |
|---|---|---|
| Prone | **MATCH REQUIRED** *(with a live unknown)* | Posture rides in the replicated 4-bit `mState`; `STATE_PRONE = 2` (`soldier_prone.cpp:41-43`) fits, so framing is safe and the value **does** reach every peer. What a vanilla client does with `mState = 2` — animation lookup, state-machine handling — was **never established**. This is the only feature in the set with a plausible remote-crash path. Everyone or nobody. |
| DisableDeadBodyShooting | **SAFE** | Damage resolution is host-authoritative; the client copy is inert. Host's setting is the ruleset. |
| DeadBodyShootingAllFactions | **SAFE** | Same. |
| DisableAwardBuffs | **MATCH REQUIRED** | **Not host-only**, contrary to the first pass. `award_disable.cpp` installs a second, read-side hook precisely because on a MP client the 16-bit award mask arrives over the wire (the `MedalsMgr` block at the tail of `ReadObject`) and the client applies the buffs itself. A client with it on against a host without will suppress buff effects locally while the host simulates them. |
| DisableAwardWeapons | **MATCH REQUIRED** | Same dual-path grant/apply structure; asymmetry produces inconsistent loadout state between what the host simulates and what the client shows. |
| GameLogging | **SAFE** | Local file output only. Costs frame time and disk on a busy host. |
| EnableSoundWarnings | **SAFE** | Local diagnostic output only. |

### Fixes

| Feature | Verdict | Reason |
|---|---|---|
| BarrelFireOriginFix | **SAFE** | Corrects the fire origin. Ordnance creation is host-side (clients suppress ordnance except for the local player), so the host's setting governs the actual projectiles. *Inferred* — the retail wire payload for `CreateEvent_CreateOrdnance` was not read. |
| CommandPostNullFix | **SAFE** | A null guard; strictly better wherever it runs, no shared state touched. |
| MemoryPoolHeapFix | **MATCH REQUIRED** | Allocator/capacity class; unpatched peers hit the same growth path on the same content. Not re-derived. |
| ImpactSoundWaterFix | **SAFE** | Proven local: the shimmed value is consumed by exactly one comparison guarding a `GameSound::Play` inside the ordnance impact handler (Steam site `0x005F7B5E`, function `0x005F7890`). Damage, explosion, effect spawn and FoleyFX are all upstream. The fix redirects the **call site** rather than hooking `RedWater::GetWaterHeight`, which is why it does not perturb `CreateExplosion`'s own water/splash branch — a genuine design win worth keeping. |
| ReticleCorrection | **ADVANTAGE** *(minor)* | Local HUD only, affects nobody else — but it aligns the reticle with the true fire vector, which vanilla players do not have. Real, small, and not worth banning; it is unenforceable anyway. |
| DroidekaDeathAnimation | **SAFE** | Presentation/animation; the host's copy governs what remote peers are told. No crash path known. Not re-derived. |
| SaberBlockFix | **SAFE** | Same class as Lightsaber Block Direction Fix — host-side outcome, inert on a client. |
| ScreenshotFix | **SAFE** | Local only. |
| TerrainTextureFix | **SAFE** | Local render only. |
| BlurDownsizeClamp | **SAFE** | Local post-process only. |
| ErrorDialogFix | **SAFE** | Local only. Note it can mask a real fault on a headless host — keep logging on if you suppress dialogs. |

### LimitIncreases

| Feature | Verdict | Reason |
|---|---|---|
| ReservationPoolSize | **SAFE** | Reinforcement/reservation counts are host-authoritative; the client copy is inert. |
| VoiceLimit | **SAFE** | Local voice-chat audio capacity. |
| SoldierHeightCeiling | **MATCH REQUIRED** | Identical to the byte patch above — an ungated simulation rule; mismatched peers disagree about who died. |
| GCVisualLimits | **SAFE** | Local render capacity. |
| *(remaining keys)* | **MATCH REQUIRED** by default | Any further `LimitIncreases` key that sizes a **map-content** array follows the Object Limit rule: the host's map is what an unpatched client will choke on. Keys that size a **runtime presentation** pool follow the VoiceLimit rule and are SAFE. Classify each by which side of that line it sits on before shipping it. |

### Lightsaber / Particles

| Feature | Verdict | Reason |
|---|---|---|
| LightsaberIllumination (+ intensity/radius) | **SAFE** | Local dynamic lighting; nothing replicated. |
| ParticleDensity — at or above default | **SAFE** | Local render density. |
| ParticleDensity — reduced | **ADVANTAGE** | Fewer particles means seeing through smoke, explosions and saber effects that obscure vanilla players' view. Real competitive edge, unenforceable, and the only genuine one in the set besides the host-side AimAssist bug. |
| ParticleFixes | **SAFE** | Local render correctness. |

### Diagnostic (all read-only; none touch the wire)

| Feature | Verdict | Reason |
|---|---|---|
| SoundDiagnostic | **SAFE** | Read-only logging; costs frame time and log volume. |
| BranchRegionDebug | **SAFE** | Read-only. |
| ContentCensus | **SAFE** | Read-only — and it is the right tool for the two ceilings that matter: point it at the **entity class registry index** (must stay ≤ 254, the 8-bit wire field) and the **live named-instance count** (must stay ≤ 1024 for vanilla clients). Run it on every map before hosting it. |
| ContentCensusNames | **SAFE** | Read-only; adds name resolution to the above. |
| AIUpdateDiag | **SAFE** | Read-only; host-side only in practice since clients run no AI. |
| PoolGrowthDiag | **SAFE** | Read-only; the fastest way to find which pool an unpatched client would have overrun. |

---

## 4. The short list

### Turn on, on the host
- **All limit raisers**: Object Limit, RedMemory, LOD, Matrix/Item Pool, High-Res Animation, Combo Anims, SkyObjectClass, DLC Mission, Sound/Audio/Voice/Layer, Particle Cache group, Attached Effects, MemoryPoolHeapFix, GCVisualLimits, ReservationPoolSize.
- **All crash/null fixes**: CommandPostNullFix, ImpactSoundWaterFix, ScreenshotFix, TerrainTextureFix, BlurDownsizeClamp, ErrorDialogFix, ParticleFixes.
- **Network Timer Increase** — most valuable exactly here. It quadruples socket-drain headroom on the machine with 64 peers.
- **Gameplay rules as your announced ruleset**: Chunk Push Fix, Soldier Height Ceiling, Lightsaber/Saber block fixes, BarrelFireOriginFix, EntityPath Branch Region Fix, DeadBodyShooting settings, the six AI knobs, DisableAwardBuffs/Weapons.

### Ban / leave off, on the host
- **AimAssist — hard off.** With a controller plugged into the host it applies to and transmits assist for every remote player. Off until the local-player gate is added to `hooked_PCUpdate`.
- **Prone — off unless you control the client population.** It is the only feature that transmits a posture value to peers whose handling of it was never verified.
- **Diagnostics off** in production (SoundDiagnostic, BranchRegionDebug, ContentCensus\*, AIUpdateDiag, PoolGrowthDiag, GameLogging). Enable them on a test host, not a live one.
- **Do not ship a map over 1024 named instances or 254 entity classes** unless you require the DLL in your join rules. Run ContentCensus first.

### Tell joiners
> "You need BF2GameExt with at least the limit patches enabled. Without them the game will **hang at the loading screen** — 100% CPU, no error — not crash. Everything else is your choice."

There is no way to enforce this: the protocol has no attestation and no content hash. It is a documentation and community problem, not a technical one.

### Fix in the DLL before the next release
1. **AimAssist local-player gate** — `aim_assist.cpp:416` and `checkAutoLock`. Highest priority; it is a live defect that degrades other people's play.
2. **String Pool Increase is non-functional on Steam and GOG** — `patch_table.cpp:2040` / `:1384` aim at `InitPool` (glyph cache), not `StringDB::Init` (Steam `0x00651ED0`, GOG `0x00652F70`). Mark the toggle non-functional until re-aimed; it currently gives false confidence about an unchecked heap write.
3. **Matrix/Item Pool arithmetic** — three sites write a byte size into an element-index compare (`0x006B028A`, `0x00407597`; correct `0xBF600` / `0xBF5FF`).
4. **SkyObjectClass** — the alloc-failure increment at Steam `0x00638DC9` is still live with the patch on.
5. **Documentation tiering** — Soldier Height Ceiling Removal and Chunk Push Fix are simulation changes filed under "fixes" in `docs/user/FEATURES.md`; they belong in a tier with an explicit "everyone should match" note. SoundParameterized's stated crash symptom is unsupported.
6. **Network Timer Increase's comment and description** are wrong in three places — see below.

---

## 5. "Network Timer Increase" — settled

**The patch name is right. The source comment is wrong, and so is the current INI description — in both directions.**

Patch site verified byte-for-byte on all three shipping builds (`PUSH 0, PUSH 0x1E`; the `0x1E` imm8 is the target):

| build | timer-init fn | bytes | patched imm8 | Timer 1 (net-aware 15/30) | Timer 2 (30 Hz, patched) |
|---|---|---|---|---|---|
| modtools | `0x00449B20` | `0x00449B58: 6A 00 6A 1E` | `0x00449B5B` ✓ | `0x00B301E0` | `0x00B301E8` |
| Steam | `0x0052D480` | `0x0052D4BF: 6A 00 6A 1E` | `0x0052D4C2` ✓ | `0x01E55F80` | `0x01E55F88` |
| GOG | `0x0052D480` | `0x0052D4BF: 6A 00 6A 1E` | `0x0052D4C2` ✓ | `0x01E57428` | `0x01E57430` |

The containing function is labelled `TTYScroll` in the Ghidra DBs. **That label is a bad symbol match** — the real `TTYScroll` is a text-buffer row scroller elsewhere. This is the frame-timer initialiser. That mislabel is where the "not netcode" comment came from.

The irony that made this confusing: **Timer 1 is the net-aware one** (`freq / (netEnabled ? 15 : 30)`) and it is **completely dead** — its branch in `FrameUpdate::Update` (Steam `0x0052D510`, modtools `0x00449D10`) does nothing but re-stamp its own timestamp. **Timer 2 is the fixed `freq / 30`, and it is the one that does all the work.** The patch changes the live timer.

The timer-2 branch calls exactly three things:
1. `GameVoiceChat::Update(dt)` (Steam `0x0053C3F0`) — voice jitter buffer, decode, talker timeouts.
2. **`NetGame::Update()` (Steam `0x005CFA10`)** — the packet pump: per-connection outbound queue flush (`FUN_005B2DB0`) and the UDP receive drain (`FUN_005B31C0` → `FUN_00617F50`, a `recvfrom` loop capped at **80** datagrams on the primary socket and **64** on the secondary, with everything past the cap read off the socket and **discarded**).
3. **The DirectInput keyboard poll** (Steam `FUN_006C6590`, modtools `FUN_00816C80`) — `GetDeviceState(0x100, …)`, the 256-key edge scan, key-event dispatch, capslock, and auto-repeat.

Both prior readings of that third callee were wrong: it is **not** force feedback, and it **does** mean the patch touches input timing. Because auto-repeat emits at most one event per call, the poll rate is the auto-repeat ceiling: **30 events/s stock, 120/s patched.**

**Why it is safe:**
- **Packet count is unchanged.** The snapshot producers `NetGame::SendHost` (Steam `0x005B9250`) and `SendClient` (`0x005B8920`) are called from `GameLoop::Update` at turn cadence, not from this timer. One queued item = one datagram either way (the flush loop's accounting is `(len − hdr)*8 + 0xE0` bits; `0xE0` = one IP+UDP header).
- **The bandwidth throttle is untouched.** `FUN_005B2750` is a wall-clock leaky bucket driven by `QueryPerformanceCounter`, called from the send paths, not from the pump.
- **The simulation tick is untouched.** That lives in `GameLoop::Update`, driven by `netSecondsPerTurn` (Steam `0x01E62EAC`), from the Lua-settable `iTurnsPerSecond` (default 20). The pump was already running at 30 Hz against a 20 Hz sim — it was never the bottleneck on packet *count*, only on packet *latency*.
- **Net win under load.** Stock ingest ceiling is ~(80+64) datagrams per 33 ms window with the remainder thrown away; at 120 Hz the same absolute traffic fits in four windows. On a 64-player host that is a real robustness gain, and worst-case receive latency drops from ~33 ms to ~8 ms.

**Known side effects, all minor:** voice chat and keyboard serviced 4× more often; on a *dedicated* server the master-server property push in `FUN_005D6880` is gated by a per-call counter (`if (4 < DAT_007EC514)`) so it fires ~4× more often (~42 ms instead of ~167 ms); `HeroRulesUpdate` (Steam `0x005CEDC0`, host-and-not-client gated) polls 4× more often but every threshold in it is absolute, so unlock timing does not change; on modtools only, it perturbs `PblJournal` playback.

**Verdict: SAFE. Host and client do not need to match. Recommended on, especially on the host.**

Doc text to correct: `PatcherDLL/src/core/patch_table.cpp:593`, `:1328`, `:1991` (names a mislabelled function, hides that this is `NetGame::Update`); `PatcherDLL/src/util/ini_registry.hpp:50` (says "input/voice-chat" — input is right, but the patch's main job is the network pump); `docs/user/FEATURES.md:30` and `docs/user/CONFIGURATION.md:35` (claim "nothing about netcode changes" — the *simulation* tick genuinely is untouched, but this timer **is** the network I/O tick). Suggested replacement: *"Raises `FrameUpdate::Update`'s inner tick from 30 Hz to 120 Hz. That tick gates `NetGame::Update` (UDP `recvfrom` drain, 80+64 datagrams per call with the remainder discarded), `GameVoiceChat::Update`, and the DirectInput keyboard poll and auto-repeat. It does not change send rate, packet count, turn rate, or the simulation."*

---

## 6. VERIFIED / INFERRED / UNKNOWN

**Netcode had never been investigated on this project before this pass. Treat everything here as first-pass, and treat §3's per-row confidence as uneven.**

### VERIFIED on a shipping build (instructions or bytes read; modtools carries real symbols)
- The authority model: `GameLoop::Update` host/client arms, `NetGame::Predict`, `NetGame::IsSkipped` and its per-object LOD, `Thread::ActivateThread`'s `PER_FRAME / PER_LOCAL_TURN / PER_TURN` taxonomy, host rollback via `ApplyLateMoves` and the 8-turn ring, `netWaitLate`'s two selectable input models, turn rate = 20 Hz, `GetMaxSimTurns` = 4.
- Clients cannot forge damage (`SetCurHealth` Steam `0x00488820`).
- Clients allocate no AI controllers in a net game.
- No content, executable, or protocol checksum anywhere in `ReadShellUpdate` (Steam `0x005E6690`).
- Entity interpolation exists and feeds targeting and weapon events, not just render (`GetSmoothedMatrix` → `GameObjectInterpolator::GetMatrix`, `GetTurnRatio`). **This refutes the earlier "the client is snapped" claim in both directions** — `ReadObject` writes raw state, but consumers read through a one-turn-late LERP/SLERP.
- All wire widths in §2(a), and the fact that shield/jetfuel field *presence* is decided from each side's own local values.
- The Network Timer analysis in §5, on all three builds.
- The unguarded hash primitives, `mIdMap`'s 1024 slots, the fact that `netOnClient` is false during level load, `StringDB::Add`'s absent bounds check, the mis-aimed retail String Pool patch, the unbounded SkyObjectClass store, the Chunk Push and Soldier Height patch sites, the `ExplosionClass` ctor default and what its visibility flag gates, `SoundParameterized`'s graceful failure, `ImpactSoundWaterFix`'s locality, the AimAssist propagation path.
- The sim-RNG reseed-to-zero on `NetGame::Create`.

### INFERRED (reasoning is sound, one link not read on a shipping build)
- **BarrelFireOriginFix is host-side-only.** The hook target is confirmed; that the corrected `mFirePos` reaches the wire is Phantom-derived.
- **`WeaponDestruct::EnterFire` suppresses client ordnance** (Steam `0x00681C60`) — carried forward, not re-read.
- **Per-subsystem `PER_FRAME` vs `PER_TURN` classification.** The mechanism and enum are verified; the ~182 `ActivateThread` call sites were never swept. Every row in §3 that says "presentation, therefore cosmetic on a client" rests on subsystem *type*, not on a measured thread class. That sweep is the single highest-value follow-up for tightening this table.
- **`NetRandom::LeaveClient` never restores the host seed** — confirmed on modtools (`0x006E3EC0`, `hostRandom 0x00BDC6DC` has exactly one xref, the write) and structurally on Steam (`0x005B7580`), but the RNG swap was not isolated among `LeaveClient`'s ~19 singleton swaps on retail.
- **The MATCH REQUIRED verdicts for Particle Cache ×3, Attached Effects, PropGenerator, MemoryPoolHeapFix, Combo Anims, High-Res Animation** — classified by content-driven-vs-presentation, not individually re-derived. Conservative by design.

### UNKNOWN — say so plainly
- **What a vanilla client does when it receives `mState = 2` (prone).** It fits the field and it does arrive. Whether it plays, ignores, or faults on an animation it does not have was never established. **This is the highest-value open item in the whole answer** and the only reason Prone is not simply SAFE-on-host.
- **Whether combo-animation indices ever reach the wire**, and whether an overflowing combo state changes sim-RNG draw count relative to a peer that plays it.
- **Whether the camera-shake queue `Explosion VisibleRadius` unlocks perturbs the aimer.** The path is proven; the outcome is not.
- **The client damage stub.** `Damageable::ApplyNetClientDamage` is not present by name on any shipping build; the "client computes no damage" conclusion is supported by `SetCurHealth`'s clamp and the replicated health fields, not by reading the stub.
- **`netUpdateSize` on Steam/GOG** — verified as `0x400` on modtools (`0x00ADABA8`); the retail global was not located.
- **The 12-bit protocol version and `JoinRejectReason` enum** — Phantom-only. No shipped patch sits near the join handshake either way.
- **Whether the particle system draws from the sim RNG.** No particle or render function appeared among the sim RNG's ~96 Steam xrefs, but the list was not exhausted.
- **GOG coverage.** Apart from the timer set, `GameLoop::Update` `0x00533140`, `StringDB::Init` `0x00652F70`, and the one-world flag `0x007E902C`, GOG addresses were not individually re-derived. Assume GOG behaves as Steam; verify before relying on a GOG-specific address.
- **Lightsaber Block Direction Fix, PropGenerator Update Loop Exit, EntityPath Branch Region Fix, Particle Effect Skip Fix, Particle Cache Reset Fix, Attached Effects Overflow Fix** — none were audited against the criterion that mattered most this pass ("does this change how many values this machine pulls from the sim stream, or in what order"). They are rated by category, not by measurement.