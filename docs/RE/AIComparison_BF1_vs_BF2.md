# SWBF1 vs SWBF2 AI - Comparative RE Notes

Source programs:
- **BF2**: `Battlefront2_Phantom.exe` (dev build, real PDB). All BF2 addresses below are **Phantom** and must be ported before patching (see `project_steam_hook_porting`).
- **BF1**: SWBF1 PPC decomp build with real symbols.

Companion doc: `AISystem.md` (BF2 architecture in depth).

---

## Verdict on the two community claims

**"BF2 AI focuses the player over everything else."**
Confirmed, and it is not a perception artifact. There are **five distinct code sites** that privilege a human player over an AI bot, plus one that partially counterbalances them. BF1 has **zero** player terms anywhere in its target selection.

**"BF2 AI is more advanced than BF1's."**
Also confirmed. BF2 adds a threat-memory layer, a strategic goal layer, objective-driven agents, a squadron system, and a 6x finer priority space. BF1's AI is genuinely stateless by comparison.

Both claims are true at once. BF1 does not feel fairer because it is smarter - it feels fairer because it is **indifferent**. It literally cannot tell you apart from a bot.

The identity test is `Controllable::mPlayerId` (offset **+208 / 0xd0**, confirmed against the `Controllable` struct): `>= 0` means a human player, `< 0` means AI.

---

## Shared ancestry

BF2's AI is an evolution of BF1's, not a rewrite. Both share:

- `ControllerManager` -> `UnitController` -> `UnitAgent` + `AILowLevel` hierarchy
- `AttackHelper`, `CombatUtil::SelectCombatResponse`, `AIUtil`, `AIPath`, `ConnectivityGraph`
- The full hint-node set: `LandHint`, `MinefieldHint`, `JetJumpHint`, `CoverHint`, `PatrolHint`, `SnipeHint`
- `AIUtil::DumbDown` (deliberate aim degradation)
- Squad commands (`SquadCommandGroup`, `IsInCrowd`), per-team difficulty
- `VisionManager` ray-request budget + `CanPotentiallySee` + `GetRelativePriority` type matrix
- The **turret-disturbance override**: an AI gunner in a player-driven Walker/Hover engages whatever the human driver's reticule is on. Present in *both* games - not a BF2 addition.

The `GetRelativePriority` type-matchup matrix is near-identical in both (values 0-20, keyed on attacker EntityType x target EntityType). BF2 only reshuffles a few cells and adds `ENTITY_TAUNTAUN` handling.

---

## The five player biases in BF2

### 1. Detection range: players are visible twice as far
`VisionManager::MaxVisibleDist` @ `0x007a1b90`

```c
mult = mGlobalVisibilityModifier;                    // SetAIViewMultiplier
if (target->controllable->+0xc4)  mult *= class_visibility_modifier;
if (target->controllable->mPlayerId >= 0)  mult = mult + mult;   // <-- HUMAN PLAYER: 2x
if (me is Flyer)             mult *= 2;
else if (me is BuildingArmed) mult *= 10;
return (targetRadius * 20 + 30) * mult;
```

BF1's inline equivalent, inside `UpdatePotentiallyVisible` @ `0x00131950`:

```c
fVar1 = (me is Flyer) ? 2.0 : 1.0;
fVar1 = fVar1 * (targetRadius * 20.0 + 30.0);       // no player term, no globals
```

**This is the single most impactful bias.** It is why AI appear to spot you across a map while ignoring a bot standing next to them - they genuinely cannot see the bot yet.

### 2. Target ranking: everyone else pays double
`VisionManager::GetVisualPriority` @ `0x007a18b0` (verified against disassembly - Ghidra's decompiler loses register tracking in this function and renders it wrongly)

```asm
007a19d2: MOV EAX, 0x15            ; 21
007a19d7: SUB EAX, [ESP+0xc]       ; 21 - relPriority
007a19db: IMUL EDI, EAX            ; cost = min(dist,1500) * (21 - relPriority)
007a19e5: JZ  -> double            ; no controllable   -> x2
007a19ee: CMP [EAX+0xd0], 0x0      ; mPlayerId
007a19f5: JL  -> double            ; AI bot            -> x2
007a1a01: CALL AIUtil::IsVehicle
007a1a0b: JNZ -> double            ; I am a vehicle    -> x2
007a1a0d: MOV EAX, EDI             ; human player + I am infantry -> cost UNDOUBLED
```

The return is a **cost** (lower = better target). Every candidate is doubled *except* a human player evaluated by non-vehicle AI. To an infantry bot, a player at 40 m ranks like a bot at 20 m. Vehicle AI are exempt from this bias.

BF1: `return dist * (0x15 - relPriority);` - no doubling, no player check.

Also here: if the AI carries a `WeaponTowCableClass` and the target is not an AT-AT, `relPriority` is forced to 0, giving max cost. That is the hardcoded Hoth snowspeeder tow-cable preference.

### 3. Threat registration: players enter at a higher status
`AI::UnitThreatManager::AddThreat` @ `0x007943a0`

```c
if (obj->controllable && obj->controllable->mPlayerId >= 0) {
    ...
    UpdateThreat(this, obj, TS_SEEN);        // human player
    return;
}
UpdateThreat(this, obj, TS_UNKNOWN);          // AI bot
```

Threat priority includes a `mThreatStatus * 18` base term, so a player starts with a standing head start over a bot registered the same frame.

### 4. Reaction latency: players jump the update queue
`AddThreat` @ `0x007943a0` and `UpdateThreat` @ `0x00794e60`

Both call `ControllerManager::RequestControllerUpdateASAP(mMyCtrl)` when the object entering the threat list is player-controlled. `AddThreat` additionally fires it when the AI was previously tracking a bot and a player appears. AI therefore react to players with lower latency than to bots, independent of any priority scoring.

### 5. Damage response: a hard target override
`UnitAgent::DamagedEventPlayerCentricness` @ `0x00783700`

```c
if (attacker->mPlayerId >= 0 && attacker is enemy-affiliated) {
    AI::UnitThreatManager::AddThreat(&ctrl->mThreatManager, attacker);
    UnitController::ForceSetAttackHelperTarget(ctrl, attacker);   // hard override
    return 1;
}
```

`ForceSetAttackHelperTarget` -> `AttackHelper::SetVisibleTarget` writes the target directly, bypassing `UpdateTarget`'s priority comparison and hysteresis entirely. **Shooting an AI makes it switch to you unconditionally.**

Additionally, if the player attacker is more than 50 m away (`2500.0` squared), the damage event is **re-broadcast to every AI within 20 m of the victim** with `AFFIL_ALL`. One sniper shot alerts a 20 m bubble. This path is player-only.

### 6. The counterweight: `Threat::GetPriority`
`AI::Threat::GetPriority` @ `0x00794550` - the only site that *reduces* player priority.

```
if (I am a Flyer && threat is behind me)  return 0
if (status == TS_LOCKED)                  return (missionTime - lastSeen < 4.0) ? 255 : 0

base   = status * 18
score  = 100 - (GetVisualPriority(me, threat) * 100 / 60000)

if (threat is a human player):
    dot   = dot(normalize(myPos - threatPos), threatForward)   // is the player facing me
    floor = max(0, 2 * AIDifficulty::GetPlayerThreatAngle() - 1)
    score *= clamp((dot - 0.6) / 0.4, floor, 1.0)
else:
    score /= 2

if (threat carries a FlagItem)  score = 100

priority = base + 100 + score
if (status != TS_UNKNOWN):
    priority -= (missionTime - lastSeen) * 10
    if (status == TS_FORCEDSEEN)  priority -= (missionTime - lastSeen) * 10   // applied twice
return clamp(priority, 0, 255)
```

`GetPlayerThreatAngle()` = `GetFloatSkill_RangedLinear(NULL, 0.0, 0.7, true)`, so `floor` ranges 0.0 (lowest skill) to 0.4 (highest).

Read the player branch carefully - it is the most defensible piece of design in the whole system:

| Player state | Multiplier | vs a bot's flat 0.5 |
|---|---|---|
| Aiming straight at the AI (dot = 1.0) | 1.0 | **2x a bot** |
| ~53 deg off (dot = 0.6) | floor | 0 to 0.4 - *less* than a bot |
| Facing away, lowest difficulty | 0.0 | AI ignores the player entirely |
| Facing away, highest difficulty | 0.4 | still slightly under a bot |

So this function alone is close to fair: it prioritises the player **only while the player is a live threat to that specific AI**. The oppressive behaviour comes from the other five sites, which are unconditional.

Verified detail: `TS_LOCKED == 4`, and the locked path compiles to `SETBE AL; DEC AL` - exactly `255` while seen within 4 s, `0` after. That is an intentional binary latch, **not** a uchar overflow bug.

---

## What BF2 added over BF1

### Threat memory (BF1 has none)
BF2's `AI::UnitThreatManager` holds **6 `Threat` slots** (32 bytes each):

```
Threat (32 bytes):
  +0   PblHandle<GameObject> mThreatObject
  +8   ThreatStatus          mThreatStatus     // TS_UNKNOWN=0 ... TS_LOCKED=4
  +12  float                 mLastStatusTime
  +16  float                 mLastSeenTime
  +20  PblVector3            mLastSeenPos
```

with `ForceThreat`, `LockThreat`, `RayhitResult`, `ShouldRaytestUnit`, time-decay, and last-seen position tracking. BF1 has **no `Threat` type and no threat manager at all** - a search for `Threat` in the BF1 build returns zero functions.

This is the largest single capability gap. BF1 AI cannot remember an enemy it stopped seeing; BF2 AI can track six, remember where each was last seen, and decay them over time.

### Strategic goal layer (BF1 has none)
`AI::AIGoalManager` - up to 20 weighted goals, `AssignUnit` proportional fill-ratio solver, `GetSpawnLocation`, CP lifecycle callbacks. BF1 has **no goal functions whatsoever**. BF1 unit behaviour is purely local and reactive.

### Objective-driven agents
| BF1 agents | BF2 agents |
|---|---|
| Trooper, Combat, Fly, Board, UnBoard, Follow, Hold, Random, Wait, WaitSecondary, **Scout**, **Repair**, **DroidRepair** | Trooper, Combat, Fly, Board, UnBoard, Follow, Hold, Random, Wait, WaitSecondary, **CaptureCP**, **Defend**, **Destroy**, **Deathmatch**, **CTFOffense** |

BF1's roster is *role*-based; BF2's is *objective*-based, matching its goal manager.

**Careful with the repair agents - this is a refactor, not a cut.** BF2 has no `UnitRepairAgent`/`DroidRepairAgent` *class*, but repair capability is fully intact: it was folded into `UnitTrooperAgent` as **state 6 ("Repairing")**, dispatched from `UnitTrooperAgent::UpdateStatePostVision` into `RepairHelper::Update`. BF1's `RepairHelper::DoRepair` was split into BF2's `RepairHelper::Update` + `RepairHelper::CheckForRecipientAndInit` (Phantom `0x00738ef0`). Confirmed live in modtools with call-site hooks - see `repair_target_selection`.

**`UnitScoutAgent` is the only agent genuinely absent from BF2.**

> Tooling warning: `xrefs_list` on this build does **not** report CALL references - an unfiltered query returns only thunks and intra-function jumps, and `type="CALL"` returns zero. Do not conclude a function is uncalled from it. This nearly produced a false "RepairHelper is orphaned dead code" verdict here.

### Capacity and resolution
| | BF1 | BF2 |
|---|---|---|
| Priority key ceiling | 20 000 | 120 000 |
| Non-engageable penalty | +10 000 | +60 000 |
| Candidate array cap | 175 (`0xaf`) | 1200 (`0x4b0`) |
| Candidate heap cap | - | 150 (`0x96`) |
| Combat range clamp | hard `[75, 350]` | unclamped `GetMaxCombatRange` |
| Distance clamp in cost | none | 1500 |

BF2 scaled the priority space 6x, which is what `Threat::GetPriority`'s `/60000` normalisation consumes.

### Other BF2-only additions
- `EntitySquadron` - leaders, `CreateSquadronFromPath`, `CreateSquadronForFlyby`, `CanJoinSquadron`
- Per-LOD vision ray budgets, queued ray tests (`AIUtil::mQueueVisionRayTests`), `IsUberMode` reduced budget
- `mLodHumanPlayer` on `UnitController`: at `AILOD_HIGH`, friendly AI **auto-see whatever the player's reticule is on**, skipping the `CanPotentiallySee` check. A squad-assist feature, gated by `IsValidEnemy` so it only helps AI on your team.
- `AIDifficulty` with full skill curves (`GetFloatSkill_RangedLinear/Cubed`), auto-balance, and the `SpaceAssault_CheatLikeABastard` family
- `mGlobalVisibilityModifier` + per-class visibility modifier

### What BF1 does better
- **`ProcessVisionCandidates` random load-shedding**: with more than 4 candidates, BF1 coin-flips (`GetRandomInt(1,2)`) to randomly drop candidates - but never the current target, which also gets a ray-priority boost. Cheap unpredictability plus target stickiness. BF2 replaced this with a strict priority heap, which is more correct but more deterministic.
- **A dedicated `UnitScoutAgent`**, absent in BF2.

Repair is **not** on this list. BF1's `DoRepair` and BF2's `Update` use a structurally identical reach gate (same `GetCurrentCollision` touch test, same hardcoded `25.0`, same 20 s stuck bailout, same 8 s no-progress timeout). BF2 only adds a `distSqXZ < 1.0` floor and splits target selection into `CheckForRecipientAndInit`. **BF1 carries the same ArmedBuilding vulnerability**; it is not a BF2 regression.
- **`AttackHelper::UpdateTarget` hysteresis**: BF1 will not switch targets until a timer expires unless the new candidate is *strictly* better. BF2 has this too, but bias #5 bypasses it outright.

---

## How BF2 could be improved

### A. Fairness pass - directly addresses the complaint

These are gameplay tuning, not crash fixes, so INI/Lua toggles are appropriate (contrast `feedback_no_ini_for_crash_fixes`). Recommended as one `[AI]` knob group, all defaulting to stock behaviour.

| # | Site | Change | Knob |
|---|---|---|---|
| 1 | `MaxVisibleDist` | replace `mult = mult + mult` with `mult *= k` | `PlayerVisibilityMultiplier` (2.0 = stock, 1.0 = fair) |
| 2 | `GetVisualPriority` | patch `JL` @ `007a19f5` to `JMP` so players are doubled too | `PlayerPriorityWeight` (0.5 = stock, 1.0 = fair) |
| 3 | `AddThreat` | register bots at `TS_SEEN` as well | `EqualThreatRegistration` |
| 4 | `UpdateThreat`/`AddThreat` | skip `RequestControllerUpdateASAP` for players | `PlayerReactionPriority` |
| 5 | `DamagedEventPlayerCentricness` | gate `ForceSetAttackHelperTarget` behind a per-controller cooldown, or require the player to actually win on priority | `DamageAggroCooldown` (0 = stock) |

**Highest impact for least risk: #1 and #2.** Together they remove the "spotted from across the map, then laser-locked" effect while leaving the *good* behaviour (#6 - the AI prioritising you when you are actually aiming at it) fully intact. That is exactly the BF1 fairness the community praises, without giving up any of BF2's extra machinery.

#5 is what produces the "the whole squad turned on me at once" moment, because the 20 m re-broadcast plus an unconditional override fires for every AI in the bubble simultaneously. A short cooldown fixes the pile-on without making AI ignore being shot.

### B. Capability pass - make the AI genuinely better

1. ~~**Fix ArmedBuilding repair**~~ - **IMPLEMENTED AND REVERTED, 2026-08-13. Do not re-attempt in this form.**

   The root cause holds and is not in question: `RepairHelper::Update`'s fire-attempt branch is reachable for non-Controllable targets only via `distSqXZ < 25.0` against the target's raw pivot, because the collision-touch cache can never populate for them. Widening that constant to `max(25.0, (radius + 8)^2)` for non-Controllable targets does work, and armed buildings get repaired.

   It was still reverted, because the repair behaviour it unlocks is worse than the bug. Damaged armed buildings were effectively invisible to repair target selection (nearest needy ally within 75 m, 5 s recheck) only because the fire condition could never be satisfied. Once it can, buildings compete with every other repair target, and engineers break off mid-firefight and charge across open ground holding the fusion cutter. Confirmed by play test with the fix toggled on and off: at `0` the engineer stops running into battle with the repair tool out. The bug was load-bearing.

   Anything that revisits this needs to change *target selection*, not reach - for example excluding armed buildings from repair candidacy while under fire, or requiring the unit to already be near the building rather than letting it path to one. Widening reach alone is a dead end. Addresses for all three builds are kept in the table below in case a narrower attempt wants them; they are no longer in `game_addrs.hpp`.
2. **Make `SetTeamAggressiveness` live.** `Team::mAggressiveness` (Team+0x94) is written but read by nothing - `Team::GetAggressiveness` has exactly one xref, its own thunk.

   **The default is `1.0`**, set in `Team::Team` (Phantom `0x00774950`), and the Lua binding (`0x00652a60`) validates the argument to `[0.0, 1.0]` - so scripts can only ever turn aggressiveness *down* from a default of maximum. Any formula used must therefore be **identity at 1.0** or it will silently re-balance every existing map:

   ```
   offensive goal weight *= aggr           // 1.0 -> x1.0  (no-op)
   defensive goal weight *= (2.0 - aggr)   // 1.0 -> x1.0  (no-op)
   ```

   A map that never calls `SetTeamAggressiveness` is then bit-identical to today. Note the formula in earlier notes (`offensive *= 0.5 + aggr`, `defensive *= 1.5 - aggr`) is **wrong** - it yields x1.5 / x0.5 at the default and would change every stock map. Still TODO before coding: map the goal-type byte offset in the goal struct, set in `AddGoal` (`0x5cd130`).
3. **Expose threat decay.** The `(missionTime - lastSeen) * 10` term - doubled for `TS_FORCEDSEEN` - controls how long AI stay interested in a lost target. A multiplier here is a one-instruction change that gives modders "AI memory length" as a tunable.
4. **Reconsider a scout agent.** BF2 has all the prerequisites (`PatrolHint`, `SnipeHint`, the goal manager) but no agent that ranges ahead of the line.

### Implemented (2026-08-12)

Three of the above now ship. Per-build addresses, all derived and byte-verified against the live binaries (Phantom was reference only):

| Patch | modtools | Steam | GOG | Site |
|---|---|---|---|---|
| MaxVisibleDist player 2x | `0x005c9a27` `7C 0C`->`EB 0C` | `0x00670496` `7C 12`->`EB 12` | `0x00671536` `7C 12`->`EB 12` | `JL` skipping the player doubling |
| GetVisualPriority player exemption | `0x005c93a4` `74 13`->`90 90` | `0x006710eb` `7C 1D`->`EB 1D` | `0x0067218b` `7C 1D`->`EB 1D` | branch to the undoubled return |
| Threat::GetPriority player 2x on eye contact | `0x005a147a` `0F84 8E..`->`E9 8F.. 90` | `0x00669bad` `0F84 99..`->`E9 9A.. 90` | `0x0066ac4d` same as Steam | `PlayerControllerPtr==null` JZ into the `/2` path |
| ShouldRaytestUnit tunnel vision | `0x005a1bb6` `74 0B`->`EB 0B` | `0x0066a3c0` `74 0B`->`EB 0B` | `0x0066b460` `74 0B`->`EB 0B` | JZ skipping the `return false` |
| Repair reach: guard call site | `0x005bce5d` | `0x0062f15a` | `0x006301fa` | `CALL GetCurrentCollision` -> shim |
| Repair reach: 25.0 operand | `0x005bce6a` | `0x0062f184` | `0x00630224` | repointed to a DLL float |
| Repair reach: 25.0f constant | `0x00a36708` | `0x007b22e0` | `0x007b3258` | verified before repointing |

GOG was ported from Steam with `tools/port_gog.py` (uniform shift `+0x10a0`, score 1.00 on all three code sites; the constant carried 31 agreeing reference votes) and then checked against the GOG image directly. Its guard is byte-identical to Steam's apart from the relocated constant addresses.

Code: `PatcherDLL/src/ai/ai_fairness.cpp`. The repair-reach addresses are recorded here only - `repair_reach_fix.cpp` was removed (see B.1), so nothing in the tree resolves them.

Notes worth carrying forward:
- **`Controllable::mPlayerId` is at +0xd4 on modtools and Steam, +0xd0 on Phantom.** Do not port that offset across.
- modtools is x87 (`FLD` / `FCOMP` against a `.rdata` constant), Steam is SSE (`MOVSS` / `COMISS`). The 25.0f constant is `0x00a36708` (modtools) and `0x007b22e0` (Steam).
- Steam and GOG keep XMM2 (weaponRange) and XMM3 (distSqXZ) live across the `GetCurrentCollision` call, so any shim there must preserve them.

### A sixth bias, found later: ShouldRaytestUnit

`AI::UnitThreatManager::ShouldRaytestUnit` (Phantom `0x00794b60`) is a **hard gate**, not a ranking term, which is why it survived the first three patches:

```c
if (candidate is a human player)  -> normal per-LOD raytest timer
else {
    current = GetVisibleObject(this);
    if (current == NULL)           return true;
    if (current is a human player) return false;   // <-- never raytest a bot
    -> normal per-LOD raytest timer
}
```

`UpdatePotentiallyVisible` only issues `VisionTest` / `AddRayRequest` when this returns true. So **while an AI is tracking a human player it spends no line-of-sight rays on any other enemy**, cannot confirm one visible, and therefore cannot switch. This is the true "once they see me they lock on" mechanism, and it explains why heroes in particular look fixated: they engage you, acquire you, and then are structurally incapable of noticing anyone else.

The per-LOD retest interval table is `{4.0, 4.0, 2.0, 1.0, 1.0}` seconds indexed by `mLodState`. Removing the early-out means AI now spend rays on other candidates while engaging a player, where stock spent none; the interval table still bounds it.

Related dead end worth recording: **`AIUtil::IsHeroGuy` is a `return false` stub on both modtools and Phantom**, and `AIUtil::IsImportant` is camera-distance LOD culling, not targeting. There is no hero-specific AI handler, no hero agent, and no hero row in the `GetRelativePriority` type matrix. Hero fixation is these generic player biases plus melee units having to close distance.

### Caveats

- Every BF2 address here is **Phantom**. Port via `tools/port_confirm.py` and confirm against disassembly before patching - see `reference_pdb_dump_caveats` and `project_debug_vs_release_offsets`.
- `ThreatStatus`: `TS_UNKNOWN = 0` and `TS_LOCKED = 4` are verified. The ordering of `TS_UNSEEN` / `TS_SEEN` / `TS_FORCEDSEEN` in between is inferred from usage (`UpdatePotentiallyVisible` tests `== 3`) and is **not** confirmed - pin it down before relying on the `status * 18` term.
- `GetVisualPriority` decompiles incorrectly in Ghidra (lost register tracking, `unaff_EBX/ESI/EDI`). Always read that one as disassembly.
