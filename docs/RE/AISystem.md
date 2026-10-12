# BF2 AI System - Reverse Engineering Notes

## Architecture Overview

The AI is a **hierarchical command/agent system** with four main layers:

```
ControllerManager (singleton, ticks everything)
  └─ UnitController (one per AI-controlled unit)
       ├─ AILowLevel (locomotion, navigation, firing)
       ├─ UnitAgent (behavior/decision - swappable)
       │    └─ AttackHelper (target tracking, aiming, weapon selection)
       └─ UnitThreatManager (threat awareness)
```

---

## ControllerManager - The Brain Clock

The singleton that drives all AI every frame. `ControllerManager::Update` (1612 bytes) is the main loop:
- Maintains two linked lists: **PlayerControllers** and **UnitControllers** (`PblOrderedList<UnitController, float>` sorted by next-update-time)
- Calls `sUpdateFriendlyFire` - iterates all controllers, does `TeamManager::GetObjectsInRange` with radius 50 to detect friendly fire incidents
- `EnterClient`/`LeaveClient` - swaps global state for client/server context switching (netplay)
- `BroadcastEvent` - sends `AI::EventInfo` to controllers within a radius, filtered by team affiliation (`AIAffil` enum)
- `GetPlayersInRange`/`GetAIInRange`/`GetUnitsInRange` - spatial queries for nearby units
- `DoPeriodicUpdate` - lower-frequency updates
- `UpdateCrowd` - crowd/group behavior updates
- `OutputDebugInfo` - guarded by `AIUtil::gAIDebugOutput`, renders obstacles/connectivity graph/hint nodes

---

## UnitController - Per-Unit AI State Machine

One per AI character. Inherits from `Controller`. Has a **command FSM** (finite state machine) with states like idle, combat, vehicle, etc.

### Update flow:
1. `UpdateHighLevel` (1268 bytes) - the big decision loop. Computes delta time, checks if alive/active, runs high-level state transitions
2. `EnterState` (900 bytes) - massive switch on state enum, creates the appropriate `UnitAgent` subclass for the new state
3. `UpdateLowLevel` (312 bytes) - decrements a LOD-based countdown; when it fires, calls `AILowLevel::Update` with accumulated delta time. Higher LOD = less frequent updates
4. `UpdateLodState` (857 bytes) - calculates distance to camera, picks LOD tier from `{4.0, 3.0, 2.0, 1.0, 0.25}` update rate multipliers

### LOD system
AI units far from camera update less frequently. `GetUpdateRate` multiplies the base agent rate by a LOD tier factor. This is how BF2 handles 32+ AI units without melting the CPU.

### Combat entry
`EnterCombat` allocates a `UnitAgent` from a memory pool (`UnitAgent::sMemoryPool`, 92 bytes per agent), constructs it, stores at `this+0x2C0`.

### Friendly fire tracking
Maintains a count at `+0x3E4` and an array at `+0x3E8`. `AddFriendlyFire` records who shot friendlies, `GetFriendlyFireCount` reads it.

### Invisibility/buffs
`UpdateInvisibility` checks if the soldier has an active invisibility weapon; `UseBuffs` handles buff items.

### Squad commands
`GetLastSquadCommand`/`SetLastSquadCommand` at `+0x3B8` - stores the last squad order received.

### Subclasses:
- `UnitFlyController` - overrides `EnterState` to check state==6 (fly); creates `UnitFlyAgent` instead of generic agent
- `UnitMobileController` - for ground vehicles
- `UnitStationaryController` - for turrets/stationary units

---

## UnitAgent - Behavior Layer

The **swappable behavior module** that decides what the unit actually does. Base class provides:

### Event handling
`EventHandler` (1024 bytes) is a big dispatch on event type:
- `Handle_Vision_CanSeeEnemy` (336 bytes) - visibility event, checks if enemy is in sight
- `Handle_Audible_CanHearEnemy` (677 bytes) - audio event, uses collision radius for detection range
- `HandleDamagedEvent` (877 bytes) - damage response, calls `DamagedEventPlayerCentricness` to weight player-centric reactions
- `HandleSquadCommandEvent` (543 bytes) - processes squad orders with subtypes

### Target facing
Three `FaceTarget` overloads - no-arg (default mode 5), with height enum, with height + position vector. All dispatch through `FaceTargetCommon` (225 bytes).

### Decision helpers:
- `ShouldAgentGetInNearbyVehicle` (301 bytes) - evaluates whether to board a nearby vehicle
- `RunForVehicle` (169 bytes) - pathfind to vehicle
- `AreNearbyUnitsInCombat` (313 bytes) - checks if friendlies nearby are fighting
- `PlayDamageEventVO` (146 bytes) - triggers voice-over on damage

### 14 specialized subclasses
Each overrides `EnterState`, `UpdateState`, `ExitState`:

| Agent | Role |
|-------|------|
| `UnitTrooperAgent` | Default infantry - capture CPs, patrol, engage |
| `UnitCombatAgent` | Active combat engagement |
| `UnitFlyAgent` | Flying vehicle AI |
| `UnitBoardAgent` | Getting into a vehicle |
| `UnitUnBoardAgent` | Getting out of a vehicle |
| `UnitFollowAgent` | Following another unit |
| `UnitDefendAgent` | Defending a position/goal |
| `UnitDestroyAgent` | Destroying a target/goal |
| `UnitCaptureCPAgent` | Capturing a command post |
| `UnitHoldAgent` | Holding position |
| `UnitRandomAgent` | Random wandering |
| `UnitWaitAgent` | Waiting |
| `UnitWaitSecondaryAgent` | Secondary wait (with timeout + flag) |
| `UnitDeathmatchAgent` | Deathmatch-mode behavior |
| `UnitCTFOffenseAgent` | CTF flag offense |

---

## AILowLevel - Locomotion & Firing

The **low-level controller** that translates high-level commands into actual movement and shooting inputs. **EMBEDDED at `UnitController+0x2C4`, 232 bytes - it is not a pointer.** Reach it with `LEA`/`ADD`, never a dereference. Its `mNavigator` is at `AILowLevel+0x18`, i.e. `ctrl+0x2DC`, and THAT one is a pointer and is nullable.

Verified layout of `UnitController_data` from Phantom's PDB. It sits at `+0x1CC` inside `UnitController`, which four known offsets confirm:

| Field | In `_data` | Absolute | Cross-check |
|---|---|---|---|
| `mNextUpdateTime` | 20 | `0x1E0` | the scheduler key |
| `mThreatManager` | 40 (204 B) | `0x1F4` | `Threat[6]` |
| `mAgent` | 244 | `0x2C0` | nullable, see EnterState |
| `mLowLevel` | 248 (232 B) | `0x2C4` | **embedded** |
| `mLodState` | 480 | `0x3AC` | the LOD tier |
| `mLodHumanPlayer` | 484 | `0x3B0` | confirms LOD keys off a human, not the camera |

### Navigation
Uses a `Navigator` abstraction with multiple implementations:
- `NavigatorGraphFollower` - follows the connectivity graph (pathfinding)
- `NavigatorPathFollower` - follows a pre-computed `AIPath`
- `NavigatorFollowTarget` - follows a moving target
- `NavigatorSlide` - slides along/around obstacles

Key navigation methods: `SetNavigator_Goto` (620 bytes, allocates navigator + requests path), `SetNavigator_FollowPath`, `SetNavigator_FollowTarget`, `SetNavigator_Slide` (2 overloads), `SetNavigator_GotoDirect`, `SetNavigator_Stationary`, `DeleteNavigator`, `IsNavigatorInProgress`, `IsNavigatorFailedPath`, `Navigator_Wait`.

### Firing
`ProcessFire` (462 bytes) - reads fire request bits, does weapon-type dispatch (cannon, melee, etc.), calls `Trigger::Update`. Sub-handlers: `ProcessSniperWeapon`, `ProcessChargeWpn` (303 bytes - charge weapons), `ProcessBarrageWpn` (570 bytes - barrage fire), `ProcessTriggerSingle` (255 bytes - single-shot timing).

### Head look
`CalcHeadLookMatrix` (903 bytes) - computes the head-look inverse matrix using the `Aimer`. `HeadLook_Object` (319 bytes) - look at a specific game object. `HeadLook_Glance` (76 bytes) - quick glance.

### Movement
`SetDest` - sets destination with position, ahead direction, safe height mode, speed params. `SetDestCurrent` - sets dest to current position. `SetHeading` - set facing direction or face a game object. `SetTarget` (246 bytes) - sets the combat target, writes entity ID, checks matchup capability bits. `SetBoardVehicle` - tells the unit to board. `SetJump`/`SetJetJump`/`StopJetJump` - jump control. `IsSafeToRoll` (246 bytes) - checks if there's room to dodge-roll.

### State
`Update` (208 bytes, profiled as "Navigators") - main tick, calls navigator update. `UpdateSkip` - lightweight skip when LOD says to skip this frame. `UpdateIndirect` (546 bytes) - indirect fire/mortar logic.

---

## AttackHelper - Target Tracking & Weapon Selection

Embedded in each `UnitAgent`, handles the combat targeting loop:

- `UpdateTarget` (242 bytes) - main target update, checks if target is still alive/visible
- `SetTarget`/`SetTargetInfo` - sets target with optional position info
- `GetTarget`/`GetVisibleTarget`/`HasTarget` - target queries via `AIUtil::GetAlivePtr`
- `GetLastSeenTargetPos`/`GetLastTimeSeen` - last known position and timestamp
- `Aim` (161 bytes) - aiming logic
- `TargetIsHiding` (147 bytes) - checks if target is behind cover
- `SelectBestWeapon` (84 bytes) - picks best weapon for current target
- `SetWaitDelay`/`ResetWaitDelay` - wait between engagement actions
- `AllowEnemyUnseen` - whether to continue tracking an unseen enemy
- `ResetWaitOverTime` - resets the wait-over timer

---

## CombatUtil - The Weapon Selection Matrix

A static utility class that implements the **combat response table** - given attacker type vs target type, which weapon/tactic to use:

### Infantry matchups
`SoldierVsSoldier`, `SoldierVsDroideka`, `SoldierVsDroid` all route through `InfantryVsFoot`. `InfantryVsVehicle` handles buildings/walkers/hovers with special suppress/ram logic. `InfantryVsFlyer`/`SoldierVsFlyer` for air targets.

### Vehicle matchups
`WalkerVsInfantry`, `WalkerVsVehicle`, `WalkerVsFlyer`. `HoverVsInfantry`, `HoverVsVehicle`, `HoverVsFlyer`. `FlyerVsFlyer`, `FlyerVsGroundTarget`, `FlyerVsIgnore`.

### Special
`DroidekaVsInfantry`, `TauntaunVsAll`, `TurretVsAll`.

### Weapon helpers
`VehicleWeapon`, `FlyerWeapon`, `HoverWeapon` - pick the appropriate weapon slot. `GetWeaponLabel` - maps weapon to a type enum (gun/explosive/melee). `HasNoWeapons` - check if unit has any weapons at all.

### Damage calculation
`sCalcExplosionDamage`, `sGetMaxAmmo`, `sCauseDamage` (1024 bytes), `sCalcDamagePerSecond`, `sSelectBestWeaponType` (1047 bytes), `sIsTargetLinedUpPos` (581 bytes), `sCalcImpactTime`/`sCalcImpactTime_Missile`.

### Top-level
`SelectCombatResponse` (850 bytes) - the main dispatcher that picks the right matchup function. `EngageInCombat` - initiates combat. `SendDamageEvent` - broadcasts damage to the AI event system.

---

## AIDifficulty - Difficulty Scaling

A global system that scales AI parameters based on difficulty setting. Uses `mProfileDifficulty` (int) with per-player and per-enemy script modifiers (`mScriptPlayerModifier[3]`, `mScriptEnemyModifier[3]`).

### Skill computation
`GetIntSkill`/`GetFloatSkill` compute a skill value from difficulty + modifiers. `GetFloatSkill_RangedLinear` and `GetFloatSkill_RangedCubed` interpolate across the skill range (linear vs cubic response curves).

### What it scales:
- **Aiming:** `GetAimerMaxAngAcc`, `GetAimerDamping`, `GetAimerSpread`, `GetAimerYawSpread` - all per-controller
- **Firing behavior:** `GetBarrageCountMultiplier`, `GetBarrageWaitMultiplier`, `GetTriggerSingleWaitDelayMultiplier`, `GetChargeWeaponBot`/`Top`
- **Grenades:** `GetGrenadeReactionTime`, `GetCrowdGrenadeThrowPercentage`
- **Target tracking:** `GetTargetBubbleShrinkRate`, `GetMinBubbleSize`/`GetMaxBubbleSize`/`GetMaxBubbleSize_Flyer`, `GetPlayerThreatAngle`
- **Flyer AI:** `GetFlyerTurnAndAttackFrequency`, `GetFlyerEvasiveFrequency`, `GetFlyerTrickOnDamage`, `GetFlyerFreeFlyOnDamage`, `GetFlyerReactionTime`, `GetFlyerSpeedRange`
- **Speed:** `GetRangedSpeed` - piecewise interpolator for movement speed

### Auto-balance
`EnableAutoBalance`, `GetAutoBalanceMode`, `AutoBalanceActive`, `GetAutoBalanceScores`, `UpdateAutoBalance` - dynamic difficulty adjustment during gameplay.

### Space Assault cheats
`SpaceAssault_CheatLikeABastard`, `SpaceAssault_RandomFlyerKill`, `SpaceAssault_RandomCritSysDamage` - the AI literally cheats in space assault mode by randomly killing player fighters and damaging critical systems on a timer.

---

---

## Why AI stand around at high unit counts

Measured, not theorised. `[Diagnostic] AIUpdateDiag` instruments
`ControllerManager::Update` and `UnitController::UpdateHighLevel`; the numbers
below are from a real match on modtools.

### The scheduler, and what it gates

`ControllerManager::Update` (`0x005997a0`, `__cdecl(float dt)`) drains a
priority queue ordered by next-update-time, servicing at most **ten** controllers
per simulation turn. The bound is branchless:

```asm
005999c2  CALL 0x0040298c     ; AIUtil::IsUberMode()
005999c7  NEG  AL             ; CF = 1 iff uber
005999c9  SBB  EAX,EAX        ; 0 or 0xFFFFFFFF
005999cb  AND  EAX,0x5a       ; 0 or 90
005999ce  ADD  EAX,0x0a       ; -> 10, or 100 in uber mode
00599a69  CMP  EBP,EBX / JL   ; the loop's only exit
```

That budget gates `UpdateHighLevel` (`0x005a0370`, `__thiscall(this)`, no stack
args) — LOD, vision, the threat manager and the command FSM, i.e. everything that
issues an ORDER. `UpdateLowLevel` (`0x0059e880`) runs **uncapped** for every
controller every turn, ticking the navigator and the weapon trigger.

So a unit that misses its slot keeps walking wherever it was already going and
keeps shooting at whatever it already had, but never makes a NEW decision. It
finishes its order and waits. That is what standing around is.

### The budget is NOT the constraint

The obvious conclusion — raise the ten — is wrong, and the measurement says so:

```
turns=7200  highLevelUpdates=10730  per turn=1.49  budget=10
peak controllers=263  by LOD tier [0]=40 [1]=93 [2]=123 [3]=5 [4]=2
```

**1.49 updates per turn against a budget of 10.** The scheduler is running at
about 15% of capacity with 263 AI alive. Nothing is queueing. Raising
`[AI] AIUpdateBudget` would change nothing, because demand never reaches the cap.

### The LOD tier assignment is the constraint

Demand is set by each unit's LOD tier, and the tier is chosen in
`UpdateLodState` (`0x0059f480`) by distance to the nearest **human player
character** — it walks `Character::sCharacters` filtering `mPlayerId >= 0`, with a
camera-visibility test only as a secondary bump. Radii are 25 and 100.

| Tier | Interval | Condition | Units observed |
|------|----------|-----------|----------------|
| 4 HIGH | 0.25 s | within 25 of a player | **2** |
| 3 NORMAL | 1.0 s | within 100 | **5** |
| 2 LOW | 2.0 s | beyond | 123 |
| 1 LOWER | 3.0 s | | 93 |
| 0 WICKED_LOW | 4.0 s | | 40 |

**256 of 263 units sit in tiers 0-2**, deciding once every two to four seconds.
Two units in the whole match were at full rate. Nothing about that depends on
combat — a firefight on the far side of the map is graded purely on how far it is
from the one human, so AI fighting each other think at 0.25 Hz.

Summing the tiers gives roughly 115 decisions/sec of demand against a supply of
600/sec, which is why the budget never binds. That supply assumes single player
at 60 fps: the budget is per AI tick, and in single player a tick is one rendered
frame. A multiplayer host ticks at 20 Hz and supplies 200/sec. See "Why more AI
makes every AI worse" below.

### Tunables, all verified

The demand side is where the leverage is, and the interval multipliers are the
safest of them because they are `imm32` floats — any value fits with no
re-encoding:

| What | Site | Encoding |
|------|------|----------|
| Interval multipliers `{4.0, 3.0, 2.0, 1.0, 0.25}` | `0x0059e7d9`, `+8`, `+8`, `+8`, `+8` | imm32 float, any value — **shipped as `[AI] AIDecisionRate`**, see below |
| LOD HIGH radius (25) | `0x0059e63c`, imm8 at `0x0059e63e` | max 0x7f in place; 16 bytes of `CC` padding follow for an imm32 rewrite |
| LOD NORMAL radius (100) | `0x0059e65c`, imm8 at `0x0059e65e` | same, same padding |
| Update budget (10) | `0x005999ce`, imm8 at `0x005999d0` | max 0x7f; sign-extended, so 0x80+ would run ZERO updates |
| Low-level skip, all tiers | `0x0059e8a1` bytes `78 0f` | `78` -> `EB` makes it unconditional |

There is headroom to spend: at 1.49/10 the budget could absorb roughly 6x more
decisions before it binds. Halving the three slow multipliers would take demand to
about 230/sec, still comfortably under the cap.

### The multipliers, shipped as `[AI] AIDecisionRate`

The interval table is `ai/ai_decision_rate.cpp`, and it is the one AI dial that
works on all three builds.

`UnitController::GetUpdateRate` — modtools `0x0059e7b0`, steam `0x006634e0`,
gog `0x00664580`, phantom `0x0078b3b0` — reads the agent's base rate (virtual
`+0x48` on the agent at `UnitController+0x2c0`, or `1.0` when there is no agent)
and multiplies it by `table[UnitController+0x3ac]`. With a stock agent the table
IS the interval in seconds.

**It has exactly one caller**, the re-queue at the tail of `UpdateHighLevel`
(modtools `0x005a084e`):

```c
controller[0x1e0] = GameLoop::GetMissionTime() + GetUpdateRate(this);
```

That is what makes scaling it safe — the numbers feed the scheduler key and
nothing else, so there is no second consumer to surprise.

The builds encode the same five floats differently, which is why the address
registry names the address of each FLOAT rather than of the instruction:

| Build | tiers 0-3 | tier 4 |
|---|---|---|
| modtools | five `C7 44 24 nn <imm32>` in `.text`, `0x0059e7d9` and every `+8` | `0x0059e7f9` |
| steam | one 16-byte `.rdata` constant at `0x007b28b0`, `MOVAPS` at `0x00663510` | imm32 at `0x0066351e` |
| gog | same shape, constant at `0x007b3820`, `MOVAPS` at `0x006645b0` | imm32 at `0x006645be` |

Rewriting the retail constant in place is safe: it carries exactly one xref, and
its `+4`, `+8` and `+0xc` have none of their own, so it is not a literal pooled
with unrelated code. Checked on both retail images.

The patch verifies all five sites against the stock table before writing any of
them, and clamps every result to a floor of `0.25` — the engine's own
closest-to-player interval — so the dial only ever closes the gap between distant
and nearby AI. It never invents a faster-than-stock rate.

One second-order effect worth knowing: `UpdateLodState` runs INSIDE
`UpdateHighLevel`, so a tier-0 unit only re-checks its own distance every four
seconds. Shortening the intervals shortens that re-check too, so units promote to
a faster tier sooner when a player closes on them.

> **Correction to an earlier note.** `AISystem.md` previously described the LOD
> tier as picked by "distance to camera". It is distance to the nearest human
> player character; the camera only contributes a secondary visibility bump.

> **Uber mode is not a shortcut.** `SetUberMode(1)` raises the budget 10 -> 100
> but ALSO shrinks the HIGH radius 25 -> 5 and NORMAL 100 -> 20, and cuts the
> per-unit vision allowance. It would demote nearly everything to longer
> intervals, plausibly making the standing-around worse at medium range.

### Eliminated, so nobody re-treads them

| Hypothesis | Why it failed |
|---|---|
| ~~`UnitAgent::sMemoryPool` exhaustion leaves units with no agent~~ | **RETRACTED - this elimination was wrong.** `MemoryPool::Allocate` returns the block in EAX and `XOR EAX,EAX / RET 4` at modtools `0x0080244e` on a failed `RedAllocFromHeap`, so exhaustion CAN hand back null. See the note below. |
| The 750-entry `PathRequest` cap | `RequestPath` frees the requester's previous request first, so there is at most one live request per controller — 750 is unreachable at any real unit count |
| The 201-slot vision ray queue (`0xc9`, not 200 - the two `PblHeap`s it feeds are `mMaxCount = 200`) | Its only producer is `UpdatePotentiallyVisible`, itself inside the already-capped high-level update, so arrival rate plateaus at ~50/pass regardless of unit count |
| Anything O(n^2) per frame | `sUpdateFriendlyFire` and the spatial queries are linear or better; nothing all-pairs runs per frame |

### Inert units: a null agent, not a slow one

Distinct from the LOD story above, and matching a reported symptom - with `aimode`
on, the occasional unit prints **nothing at all** and stands completely still.

A unit that is merely LOD-demoted still prints something and still acts, just
rarely. A unit that prints nothing has no agent state to print. The candidate
mechanism is `UnitController+0x2C0` being null:

- `MemoryPool::Allocate` (modtools `0x00802300`) returns the allocated block in
  EAX. Its failure path is `XOR EAX,EAX / POP EBX / RET 4` at `0x0080244e`, taken
  when `RedAllocFromHeap` cannot satisfy `mSize * mGrow`. **Allocation failure
  yields null**, and whether the caller notices is the caller's business.
- `UnitAgent::sMemoryPool` starts at 600 x 0x358 (`AIUtil::Init`). It grows, so 600
  is not the ceiling - but every growth is a fresh `RedAllocFromHeap` that can fail.
- `GetUpdateRate` already tolerates a null agent (`TEST EAX,EAX / JZ` at
  `0x0059e7c2`, falling back to a base rate of 1.0), which shows a null agent is a
  state the engine expects to survive rather than assert on.

Two other candidates for the same symptom, neither ruled out:

- **`AI::AIGoal::sMemoryPool` is only 20 entries** (`0x14`, from `AIUtil::Init`).
  That is tiny next to 263 controllers. If goals are per-unit rather than shared,
  a unit that cannot get one has nothing to do. Whether they are per-unit was NOT
  established.
- **A dropped `ListPool` entry.** `ListPool` does not grow: on overflow it warns at
  `ListPool.h:0x5c` and discards the item. A live report showed capacity 60 against
  2129 attempted adds, i.e. ~2069 silently dropped.

All three are cheap to separate with a read-only poll of the controller list -
print `agent`, `goal` and `command` per controller and look for the units where
they are null.

---

## Why more AI makes every AI worse

The community claim is that the more AI a map has, the dumber they get. It is
true, but not because a unit thinks less often: the decision interval comes from
distance to the player and nothing else (see above). What gets worse is that every
AI on the map draws from the same few allowances, and those allowances are handed
out per AI tick. Addresses are modtools; retail is not ported yet.

### The clock: per frame in single player

`ControllerManager::Update` (`0x005997A0`) runs once per simulation turn from the
host arm of `GameLoop::Update`. What a turn is depends on the mode:

- **Single player.** With `netEnabled` false, `NetGame::SubmitMove` (`0x006E71A0`)
  adds exactly one turn per call, and it is called once per rendered frame.
  `NetGame::GetTurnStep` returns the frame delta. **AI ticks once per rendered
  frame.**
- **Multiplayer host.** Ticks at `netTurnsPerSecond`, 20 by default.

Demand is measured in mission-time seconds (the LOD intervals), so frame rate does
not change it. Frame rate only changes supply. This is the loop behind the claim:
more AI costs frame rate, lower frame rate means fewer AI ticks per second, and the
same per-second demand is now split across fewer ticks.

| Budget per tick | Value | Uber mode | SP 60 fps | SP 30 fps | MP host 20 Hz |
|---|---|---|---|---|---|
| High-level decisions | 10 | 100 | 600/s | 300/s | 200/s |
| Vision rays | 10, or 20 when no path search is pending | 50 | 600-1200/s | 300-600/s | 200-400/s |
| Path search | 10 cost units, a second slice when no ray was cast | 100 | | | |

### Vision: one queue for the map, wiped every decision

**Allowance per decision.** `VisionManager::UpdatePotentiallyVisible`
(`0x005CA3E0`) gathers enemies in combat range (up to 1200), ranks them in a
150-entry heap, and queues ray requests for the best ones. How many it may queue
depends on the unit's LOD tier, built as stack immediates at `0x005CA9A0`:

| Tier | WICKED_LOW | LOWER | LOW | NORMAL | HIGH |
|---|---|---|---|---|---|
| Rays per decision | 1 | 1 | 2 | 4 | 5 |
| Uber mode | 0 | 0 | 1 | 2 | 3 |

Uber mode gives the two farthest tiers no rays at all. Requests only queue when
`AiQueueVisionRayTests` (`0x00ACFF44`) is set, which is the default; with it clear
every ray is cast immediately with no budget.

**The queue.** One global heap of 200. Each request's priority is its rank within
the requesting unit's own list (150, 149, ...), so every unit's best candidate has
the same priority and distance plays no part. When the heap is full, the
lowest-priority entry is evicted, or the new request is dropped if it is not
better.

**Service.** The ray pass (`0x005CAC50`) pops requests until it has cast
`min(budget, average of the last 8 passes + 1)` rays. The budget is
`IDEAL_MAX_RAYCASTS_PER_UPDATE` (`0x00AD031C`, value 10, one reader) times 2 when the path manager is idle, or 50 in uber mode. The counter
is **rays cast, not targets seen**: the vision test (`0x005CA000`) returns true
whenever both the unit and the target are still valid. Stale requests are free.

**The wipe.** `UnitController::UpdateHighLevel` (`0x005A0370`) calls
`RemoveRequests` (`0x005CA2D0`) and then `UpdatePotentiallyVisible` on every
decision, with no condition. Whatever that unit had queued and not yet served is
thrown away, and the replacements land behind every equal-priority request already
in the heap. A full heap takes about 20 ticks to drain. A HIGH-tier unit
re-decides every 0.25 s, about 15 frames at 60 fps, so under a backlog the AI
closest to the player are the ones most likely to have their requests reset
before they reach the front.

**How close to the limit.** With the 263-unit tier spread measured above, the
upper bound on ray demand is about 224/s (2 x 20 + 5 x 4 + 123 x 1 + 93 x 1/3 + 40
x 1/4). That is comfortable at 60 fps and saturated on a multiplayer host or at
20 fps. It is an upper bound: a unit only queues rays for candidates that pass
`ShouldRaytestUnit` and its own per-tier retest timer.

### Pathing: one search at a time for the whole map

**One queue, one search.** `PathManager::Update` (`0x005DD1B0`) serves one global
request list and runs a single A* search at a time.
`PathManager::GetNextPathRequest` (`0x005DD100`) takes the first request of the
lowest priority, scanning priorities 0-4.

**The budget.** The tick's float argument (1.0, or 10.0 in uber mode, pushed at
`0x00599BCD`/`0x00599BD7`) is multiplied by 10.0 into `mCostConstraint`, and
`IsWithinTimeConstraint` compares it to `PathFinder`'s cost counter. Cost is only
added inside `Obstacle::IntersectionNewer` after the broad-phase check passes, so
the unit is one detailed test against an obstacle near the route. Open terrain
costs nearly nothing; maps dense with props and buildings pay for every search.
The 10.0 at `0x00A33220` is a pooled literal with 103 readers and must not be
patched. The QueryPerformanceCounter timer around the loop only accumulates, and
`sWait`/`sWaitWorst` are zeroed in `Initialize` and never written: there is no
engine-side measure of path latency.

**Priority.** Short moves (under 50 m) request at priority 0; legs of a long route
on the connectivity graph request at 1. To either, `RequestPath` (`0x005DD550`)
adds the number of flagged nodes still ahead on the unit's current path, so a unit
with plenty of path left waits; once that sum reaches 5 the request is not served
at all until `PathFollower::GotoNextNode` lowers it as the unit walks.

**Re-requesting loses your place.** `RequestPath` first calls
`RemovePathRequest` (`0x005DD460`), which cancels the unit's own search if it is
the one running and frees its queued request, then appends the new one at the
tail. A new move order therefore throws away any partial work and starts again at
the back. Move orders come mostly from agent `EnterState` handlers, so a unit
that changes state often keeps resetting its own path.

**Waiting means standing.** A short-move navigator does nothing while it waits for
its result. A graph navigator stops at the end of the current leg until the next
leg's path arrives.

### Smaller limits

- **Six threats per unit.** `UnitThreatManager` holds six slots. A new threat
  replaces the lowest-priority one, and a replaced threat that comes back starts
  unseen and needs another ray.
- **Reservation pool.** 60 entries, raised to 127 by `ReservationPoolSize`
  (`docs/RE/EngineLimits.md`).

### Checked and not limited by unit count

| System | Why it does not degrade |
|---|---|
| `ControllerManager::BroadcastEvent` (damage alerts) | Scans every controller, no cap |
| `AudibleManager::UpdateAudience` (hearing) | 30 m radius, 1200-entry gather |
| `ControllerManager::UpdateCrowd` | Counts soldiers within 9 m, linear, no cap |
| Vision candidate gather | 1200 objects, 150-entry heap, far above real counts |

---

## AIUtil - The Kitchen Sink

~60+ static utility functions used everywhere:

### Steering
`CalculateFlyerSteerPoint` (1588 bytes), `CalculateFlyerCircleSteerPoint` (1617 bytes), `CalculateSoldierSteerPoint` (1071 bytes), `sCalculateTurnControls` (2036 bytes), `CalculateHeadingControls` (1167 bytes), `UpdatePhysics` (1725 bytes), `CalculateBubbleOffset` (838 bytes).

### Vision
`CanPotentiallySee`, `CanSee`, `CanSeePosition` (2 overloads), `IsInFOVRange`, `GetMyVisionPos`, `GetHisVisionTargetPos`.

### Spatial
`GetLeadPosition`, `GetTangentPointToCircle`, `Ovalize`, `GetRandomPtInRegion`, `GetRandomAccessiblePoint`, `DirectLineToTarget`, `GetCollisionRadius`, `StopDist`/`StopDistIntermediate`, `AvoidBarriers` (742 bytes).

### Classification
`GetEntityType`, `IsVehicle`, `IsImportant`, `IsSniperGuy`, `IsRocketGuy`, `IsHeroGuy`, `IsATAT`, `IsPlayerTeam`, `GetAIType`, `ShouldSoldierDodgeThisType`, `ShouldStayInVehicle`.

### Command posts
`IsNearPost`, `IsNearNonAllyCommandPost`, `HasCPMoved`, `GetCommandPostPos`, `GetCommandPostOffset`, `DeployCargo`.

### Spy system
`AddSpied`, `ResetSpied`, `IsSpied` - the disguise weapon's detection system.

### Vehicle queries
`CountUnitsInVehicle`, `CountHumansInVehicle`, `FindFirstPlayerInVehicle`, `FindFirstAIInVehicle`, `TellAIToExit`.

### Combat support
`DumbDown` (1270 bytes) - deliberately worsens AI aim (the "bubble" system), `GrenadeAlert` (410 bytes), `ResolveCollision`, `Slide`, `IsValidEnemy`, `GetThreatSwitchTime`, `AreEnemiesInRange`.

---

## BaseHint stances - the hint node prone bit the engine drops

`BaseHint` is a 64-byte pooled object whose PDB declares one ushort bitfield at +0x3C:

| Bits | Field |
|---|---|
| 0-7 | `mType` bitmask: Snipe=1, Patrol=2, Cover=4, JetJump=8, Mine=0x10, Land=0x20, Fortification=0x40, VehicleCover=0x80 |
| 8-9 | `mPrimaryStance` mask |
| 10-11 | `mSecondaryStance` mask |
| 12-13 | `mSidestep` mask |
| 14-15 | unused |

`BaseHint::GetRandomStance` (Phantom 0x5EC0D0, modtools 0x5C44D0, Steam 0x5435B0, GOG 0x544300)
rolls `AIUtil::GetRandomInt(0, 2)` until `mask & (1 << roll)` is set, so the mask is indexed by
`ControllableHeight`: bit0 = Stand, bit1 = Crouch, **bit2 = Prone**.

### Where hint stances come from

`LoadUtil::ProcessHintInfo` handles only the `INFO` chunk (`NAME`, `TYPE`, `XFRM`) and constructs
the hint. The stances arrive separately: `LoadUtil::ProcessHint` (Phantom 0x638310, modtools
0x451AE0) walks the sibling `PROP` chunks and calls `BaseHint::SetProperty(hash, valueString)`
(Phantom 0x5EC750, modtools 0x5C45B0, Steam 0x543670, GOG 0x5443C0) for each. The property hashes:

| PblHash | Property | Parsed as |
|---|---|---|
| 0xBF0F1CD1 | `PrimaryStance` | 3-bit stance mask |
| 0x92C05A59 | `SecondaryStance` | 3-bit stance mask, plus a 2-bit sidestep mask at bits 3-4 |
| 0x844CCDB4 | command post name | validated against `CommandPostManager::FindPost` |

Both are `sscanf("%d")`'d and range-checked against `0x20` - five bits, i.e. three stance bits plus
two sidestep bits. `.hnt` source files carry them verbatim, e.g.

```
Hint("HintNode42", "0")
{
	Position(228.444077, 1.181329, 47.504223);
	Rotation(0.674962, 0.000000, -0.737853, 0.000000);
	PrimaryStance(4);
	SecondaryStance(2);
	Mode(0);
}
```

### The prone bit is parsed, then masked off

`SetProperty` extracts the stance with `& 7` (secondary) but stores it with `& 3`, two bits wide,
because that is all the bitfield holds:

- `PrimaryStance(1|2|3)` stores normally.
- `PrimaryStance(4)` (prone only) masks to 0, and 0 is the "no value given" case that `SetProperty`
  skips entirely, so the hint keeps the constructor default of Stand.
- `PrimaryStance(6)` (crouch or prone) stores as 2, plain Crouch.

Pandemic's own levels author the bit: `assets/worlds/END/world1/end1.hnt` uses `PrimaryStance(4)`,
`assets/worlds/TAT/world2/tat2.hnt` uses `PrimaryStance(6)`, and `SecondaryStance(6)` appears too.
The prone stance was authored into the shipped levels and then cut on the reading side, matching the
rest of the stubbed-out prone system. The engine's hint debug draw
(`HintManager::_NFINAL_DrawHintNodes`, `AiShowHintNodes`) is 2-bit for the same reason: its label
table is only `""`, `"St"`, `"Cr"`, `"St Cr"`.

Consumers of the stored stance: `CoverHelper::EnterState` and `SnipeHelper::EnterState` state 3 take
the primary, `CoverHelper` states 4 and 5 the secondary; the result is held as the helper's stance
and pushed into `AILowLevel::mHeight`.

### How BF2GameExt restores it

`entity/soldier_prone.cpp` stores the dropped bit in the two unused bitfield bits (14 = primary,
15 = secondary):

1. The constructor's stance-init mask (`AND 0xC5FF`, modtools 0x5C5CAF, Steam 0x5433C3, GOG
   0x544113) is patched to `0x05FF` so those bits start cleared rather than holding pool garbage.
2. `BaseHint::SetProperty` is hooked to re-parse the value and set the prone bit, clearing the
   vanilla 2-bit field when the hint is prone-only.
3. `GetRandomPrimaryStance` (modtools 0x5C4500, Steam 0x5435E0, GOG 0x544330) and
   `GetRandomSecondaryStance` (modtools 0x5C4520, Steam 0x5435F0, GOG 0x544340) are hooked to
   rebuild the 3-bit mask and pass it to `GetRandomStance`.

**Trap:** widening `GetRandomPrimaryStance`'s own `& 3` to `& 7` does not do this. Bit 2 of the byte
at +0x3D is the *secondary* stance's Stand bit, which is set on practically every hint, so that
turns every cover and fortification hint into a 50/50 stand-or-prone roll. BF2GameExt shipped that
mistake before this was worked out.

---

## AI::AIGoalManager - Strategic Goal Assignment

Singleton that manages high-level AI goals (capture CP, defend, destroy target, CTF):

- `AddGoal`/`DeleteGoal`/`ClearAllGoals` - goal pool management (max 20 goals)
- `AssignUnit` (836 bytes) - assigns a character to the best available goal
- `RemoveUnit` - removes a character from its goal
- `ReassignIfNeeded` - checks if a unit should switch goals
- `FindGoal` - finds the goal a character is assigned to
- `MakeSureUnitHasGoal` - fallback assignment
- `Update` (314 bytes, virtual) - ticks the goal system
- `GetSpawnLocation` - picks a command post for spawning based on goals
- `AddCP`/`RemoveCP`/`ChangeCP` - command post lifecycle callbacks
- `GiveDifferentOrder` - forces a unit to a different goal
- `PrintInfoConsole`/`PrintInfoInstConsole` - debug output

---

## Key Insight: The "Bubble" System

The AI deliberately misses shots via `DumbDown` (1270 bytes) and `CalculateBubbleOffset` (838 bytes). The AI aims at an offset from the target - the "bubble". Difficulty controls the bubble size (`GetMinBubbleSize`/`GetMaxBubbleSize`) and how fast it shrinks toward the target (`GetTargetBubbleShrinkRate`). On easy difficulty, the bubble is large (AI misses a lot); on hard, it shrinks quickly (AI hits more). This is why AI feels "dumber" on easy - it's literally aiming wrong on purpose.

## Key Insight: Space Assault AI Cheats

`SpaceAssault_CheatLikeABastard` is a real function name from the PDB. It runs on a timer and calls `SpaceAssault_RandomFlyerKill` (picks a random enemy flyer and kills it) and `SpaceAssault_RandomCritSysDamage` (damages a random critical system). The AI doesn't actually outfly you in space - it just rolls dice and destroys things.

---

## Squad formations (researched 2026-10-11, built 2026-10-12)

Read in Ghidra by a research pass, then re-read on every build for `[AI] SquadFormation` and
`[AI] SquadAutoFormUp` (`ai/squad_formation.cpp`) and for AI-led squads (`ai/ai_squads.cpp`), both
built below. P = Phantom, M = modtools, S = Steam, G = GOG. S and G addresses are as Ghidra
shows them and move with the exe.

### Two kinds of follower

`UnitFollowAgent` runs in one of two modes, set by what recruited the follower:

- **Formation.** The player's "follow me" squad command (`EntitySoldier::Update` pushes
  `EVT_SC_FollowChr`) and Lua `Follow` goals (`AI::FollowGoal::Assign`, P `0x005C8650`).
  `UnitFollowAgent::UpdateStatePostVision` (P `0x007928D0`, S `0x00668500`, G `0x006695A0`; on M the
  code is there but Ghidra has no function, call site `0x005B0AB1`) reserves a slot with
  `ReserveManager::ReserveOffset(leader, follower, type)`: P `0x0073AA80`, M `0x005C6840` (cdecl),
  S `0x0062FA70` and G `0x00630B10` (ECX = out, EDX = leader, two stack arguments the caller pops).
- **Loose.** Only `PlayerController::CheckForAutoFollow` (P `0x0071BE90`, M `0x0059AD80`, S `0x0061C150`),
  which recruits friendlies the player runs past while the player has fewer than two formation
  followers. `UnitFollowAgent::EnterState` (P `0x00791E40`) sends each one to a random point 10 m
  behind the leader, give or take 4 m, and it stands still while within 20 m of the leader. There are no
  slots in this mode: the loose clump is the design.

### The slots

A follower's slot is its place among the leader's formation reservations. There are seven layouts:
random, single file (slot 0 lands on the leader), double file, jittered double file, circle, convoy
and flying V (a wedge). A 27-byte helper picks one per follower (P `0x007926E0`, M `0x005AFEE0`,
S `0x00667FC0`, G `0x00669060`): the wedge when the follower's `Controller::mOpenRadius` is above 10,
otherwise double file. On M it takes the controller in EAX and returns in EAX; on S and G it takes it
in ECX, and the caller keeps EDX live across the call.

The offset is scaled by 2.0 m, by 2.2 m when the leader moves slower than 4 m/s, and by 1.7 m while
the follower jet-hovers. On S the 2.2 and 1.7 constants are shared with other code, so a change must
move the operands, not the constants. `NavigatorFollowTarget::UpdateTarget` (P `0x0067AFF0`,
M `0x005A6920`) turns the slot by the leader's matrix, adds a sway of up to 0.3 m and drops a crumb
every 2 m; the follower walks that trail of crumbs.

Why a commanded squad still clumps (inferred):

- each follower picks its own layout, so one can be in the wedge while another is in double file;
- the slots turn with the leader's facing, which for a player is the camera's yaw;
- the first two slots are only 1.5 m to either side of the leader and 0.5 m behind;
- a reservation the pool refuses leaves the follower on slot 0 (stock pool 60;
  `[AI] ReservationPoolSize` raises it).

### What a change could do

- Auto-recruits could follow in formation: the recruit pushes mode `0x11`, and `0x10` is formation
  (M `0x0059AFAC`, S `0x0061C3CC`, G `0x0061D43C`). Auto-recruiting would then stop at two
  followers, since formation followers are what it counts.
- A replacement for the layout helper and the spacing operands could give the whole squad one layout
  and one spacing, perhaps from the units' collision radii as convoys already do (the sum of the two
  radii times 2.5).
- `ReserveOffset` could gain a line-abreast layout and a column that starts one slot back.
- `UpdateTarget` could turn the slots with the leader's direction of travel instead of the camera.

### Built (2026-10-12)

- The follow agent's one call to `ReserveOffset` (M `0x005B0AB1`, S `0x006687BC`, G `0x0066985C`)
  goes to a stand-in. It calls `ReserveOffset` for single file, which keeps the game's reservation
  and gives the slot number as -z (M `0x005C69FB`, S `0x0062FBF1`, G `0x00630C91`), then lays the
  slot out in formation space by the leader's layout and spacing. `auto` takes the layout helper's
  answer: a wedge when it says there is room (6), a column otherwise. The slot goes back divided by
  the agent's 2.0 with 1e-20 in y; every stock slot has 0 there.
- The turn cannot go into that offset. `AILowLevel::SetNavigator_FollowTarget` (P `0x00480CC0`)
  keeps the follower's `NavigatorFollowTarget` only while `IsEqual` (P `0x0067A860`) finds the same
  leader and a bit-identical `mFollowOffset`; otherwise it deletes the navigator and builds a new one,
  whose crumbs start again from the leader. An offset that changed as the leader turned would rebuild
  it nearly every update. The formation-space slot stays the same while the slot number does.
- `UpdateTarget`'s `D3DXVec3TransformCoord(out, &mFollowOffset, leader matrix)` goes to a stand-in
  too (M `0x005A6A49`, a CALL to the thunk `0x007CB7B5`, `JMP [0x00ADFC58]`: D3DX is linked into
  modtools and dispatches through that pointer; S `0x005ACC31` and G `0x005ADBE1`, `CALL [IAT]` to
  d3dx9_43). A tagged offset turns to the squad's heading round the leader's position, every update;
  any other goes on to D3DX. The navigator is `mFollowOffset` - 0x4C, its leader at +0x44 with the
  handle id at +0x48. The two stand-ins go in together or not at all.
- `ReserveOffset` has two other callers, which keep the stock slots: CTF escorts
  (`UnitCTFOffenseAgent`, two abreast for the Empire and the Republic and jittered for the other
  sides, P `0x0078D980`) and convoys (`UnitTrooperAgent`, `FT_CONVOY` scaled by the two collision
  radii).
- The heading is the leader's direction of travel (`GetVelocity`, GameObject vptr +0x44) once the
  leader moves at 1.5 m/s, easing round over about a third of a second, and the last one while the
  leader stands. The agent's 2.2 applies under 4 m/s, compared squared (16) on every build.
- Tag-alongs: two calls in `CheckForAutoFollow` are redirected, together or not at all. Its count of
  the player's formation reservations (M `0x0059ADE9`, S `0x0061C1B2`, G `0x0061D222`; on S and G
  ECX = object, EDX = team and the type pushed) comes back as 2 once the squad is full and 0 before,
  so its own `CMP EAX,2` stops at the squad size. Its recruit, `UnitController::SendEvent` (M
  `0x0059AFC3`, S `0x0061C3E1`, G `0x0061D451`; thiscall, RET 4) on an `EventInfo` built with type
  0x11, goes out as 0x10, or not at all for a class that never joins. The constructor only stores
  the type (+0x34) and the object (+0x38, +0x3C), so changing the type afterwards is the same as
  building it with 0x10. `UnitAgent::HandleSquadCommandEvent` builds the same follow command for
  both types, tight when the type is 0x10 (M `0x005AA059`, S `0x0065EA25`, G `0x0065FAC5`).
- ODF keys (`SquadFormation`, `SquadSpacing`, `SquadSize`, `SquadFollower`, and `SquadLeader` for
  AI-led squads) are kept per soldier class, read through `Controllable + g_soldier->classPtr`, and
  win over the INI key by key. A hero class is one with bit 1 of the class flags byte (+0x36C on M,
  +0x328 on S and G) set by `Team::AddSpecialClass`, Lua's `SetHeroClass` (M `0x00662EE0`,
  S `0x00653F00`, G `0x00654FA0`); `Character::IsHero` reads it (P `0x0049E1C0`).

### AI-led squads (built 2026-10-12)

Stock BF2 has none: an AI follows a leader only when the player commands it, tags along behind the
player, or a Lua `Follow` goal sends it. All three end in `UnitController::SendEvent` with an
`EventInfo` of `EVT_SC_FollowChr` about the leader, and nothing on that path needs the leader to be
a player, so `[AI] AISquads` sends the same event to AI about an AI leader.

- A tick runs once a second before `ControllerManager::Update` (M `0x005997A0`, cdecl(float); S and
  G `0x00486370`, the delta in XMM0 and no stack argument, behind a naked shim that keeps every
  register). It walks the list of every controller that `Update` sends through vptr +0x14 (the global
  M `0x00B8EE6C`, S `0x01E30930`, G `0x01E31DDC`; +4 the first node, a node's +4 the next and +0xC
  the controller, a null controller the end).
- Each `UnitController` whose owner (+4, the GameObject through the owner's +0x18 interface, vptr
  +0x20) is a soldier, alive and with health, is a candidate. Alive is bit 3 of GameObject +0x1FC,
  the bit `UnitController::UpdateHighLevel` tests before a unit thinks at all (M `0x005A03BA`
  through the Damageable at +0x140, S `0x006639B2`, G `0x00664A52`). Health is +0x144, as Lua's
  `GetObjectHealth` reads it (M `0x004720E1`, S `0x0058E59D`, G `0x0058F53D`), and the team the low
  4 bits of +0x234, signed. It already follows someone when its agent (+0x2C0, as `SendEvent` reads
  it) answers `IsRtti` (slot 0) for `UnitFollowAgent` (M `0x005B00C0`, S `0x00667F00`,
  G `0x00668FA0`), and fights while its state (+0x7C) is 2.
- Recruits are built with the game's `EventInfo` constructor (M `0x0052ADE0`, S `0x0043EDF0`,
  G `0x0043EDE0`; thiscall, RET 8, 0x58 bytes, a destructor that does nothing). Squads the tick made
  are checked each tick with `EVQ_SC_IsFollowingChr`, which `UnitFollowAgent::EventHandler` answers 1
  only about the leader it follows (P `0x00792300`).
- `UnitCombatAgent::EventHandler` hands a squad command on to `UnitAgent`'s (P `0x0078A300`), so a
  fighting unit would leave its fight to follow: the tick takes nobody in combat, and starts no
  squad round a leader in combat.
- `HandleSquadCommandEvent` plays `VOT_SC_FollowResponse` for each recruit, through
  `VOHelper::Play` with no listener, which plays it at the local player (P `0x0041BAA9`).
- A squad ends with its leader: a follower whose leader's handle goes stale leaves its follow state,
  and one whose leader boards a vehicle boards it too (`UnitBoardInfo`), as for the player.
- Points as a tie-break were left out: `PlayerStats` is `sPlayerStats[(character - sCharacters) /
  0x1B0]` (P `Character::GetPlayerStats`, `0x0049DE60`), more reads per build than a tie-break is
  worth. The tick notes when it first sees each soldier instead.

## AI sprinting (researched 2026-10-11, built 2026-10-12)

AI never touch `mControlSprint`, but they do sprint. `AILowLevel::mHeight` set to `HEIGHT_SPRINT` (3)
makes `EntitySoldier::UpdateIndirect` (P `0x00588E10`, M `0x0053B920`, S and G `0x004F0110`) call the
virtual `Sprint()`, standing back up if that fails; on S it is a thiscall taking a float, RET 4. Two
places set it:

- `NavigatorFollowTarget::UpdateFollowCrumbs` (P `0x0067AB50`, M `0x005A6C60`, S `0x005ACE90`,
  G `0x005ADE40`), whenever a follower is more than 1 m from its crumb: formation squads, CTF escorts,
  units running to board a vehicle, convoys;
- the `CombatHelper` flee state (P `0x004C7860`), for hovers and walkers only, where it means boost.

So AI sprint to catch up and never while simply crossing the map, which is what is seen in play.

What a sprint does, read in P and M:

- **Speed:** the ODF sprint factor times `MaxSpeed` (stock 1.5, so 10.5 m/s), reached over
  `SprintAccelerateTime` (0.35 s). Strafing while sprinting is halved.
- **Turning:** the sprint turn factor (0.35) applies to players only, so sprinting AI turn at full rate.
- **Energy:** `EnergyDrainSprint` (stock 20 per second, from 120) drains any soldier in the sprint
  state, but neither `Sprint()` nor the AI path checks energy first, so AI should keep sprinting on an
  empty bar (inferred).
- **Firing** ends a sprint unless the weapon allows firing while sprinting, so a fighting AI would
  flicker in and out of it (inferred).
- **Being shot at:** a sprinter's target bubble grows and its stance modifier doubles, so enemy AI hit
  it less (inferred).

**Built as `[AI] AISprint`** (`ai/ai_sprint.cpp`). A detour of `EntitySoldier::UpdateIndirect` raises
`mHeight` from stand to sprint for one update and puts it back afterwards, unless the AI set a height
of its own meanwhile, so the AI's own state never keeps it. The height jump table is left alone
(`soldier_prone` patches entries in it). It sprints when:

- the AI asked to stand, and the soldier is standing or already sprinting (`Sprint()`'s own test);
- its controller is not in combat (`UnitController` state 2, read at +0x7C) and `AILowLevel::mTarget`
  holds nothing;
- it travels a path of its own: a `NavigatorGraphFollower` (to `mGlobalDest`, +0x24) or a
  `NavigatorPathFollower` (to its path follower's `mFinalPoint`), not following a leader. A
  follower's `NavigatorFollowTarget` derives from `NavigatorGraphFollower` and its `IsRtti` says yes
  to both (P `0x0067A8F0`, M `0x005A63E0`, S `0x005AC510`, G `0x005AD4C0`), so it is ruled out
  first;
- that destination is at least `AISprintDistance` away on the ground (30 m), or half that once
  sprinting;
- its energy bar is not overheated, and to start a sprint it is at least half full.

`EntitySoldier::Update` drains any soldier in the sprint state (P `0x00583112`, the bar at Controllable
+0x7E0 on P and M, +0x7D0 on retail), so a sprint ends at the bar's overheat latch and resumes once it
clears, as a player's does. Every read and call is listed per build in `ai/ai_sprint.hpp` and checked by
`tests/ai_sprint_abi_tests.py`. Not yet tried in play.

## Path following (researched 2026-10-11)

`PathFollower::Update` (P `0x00710220`, M `0x005DB240`, S `0x006132C0` with dt in XMM1, G `0x00614360`)
moves to the next node once `CloseToDest` (P `0x0070F500`, M `0x005D9770`, S `0x00612E00`: within 1.0 m
for soldiers, 0.25 m walking, 0.5 m at the last node) or `GonePastDest` (P `0x0070F920`, M `0x005D9290`,
S `0x00612F80`: past the line through the node) is true.

`UpdateControllableMoving` (P `0x00710800`, M `0x005D9970`, S `0x006137C0`, G `0x00614860`) steers at
the point `AIUtil::CalculateSoldierSteerPoint` returns (P `0x00486160`, M `0x0058E460`, S `0x00440DB0`,
G `0x00440DA0`; on S and G ECX = previous node, EDX = target node, XMM3 = look-ahead, two stack
arguments the caller pops): (speed + 0.2) m ahead along the current leg, never past the node, so the
heading flips at every node. A soldier that bumps into something has its steer point moved 5 m
sideways (inferred to look jerky). Hovers and walkers share the function. `sCalculateTurnControls`
(M `0x00590760`) then turns as fast as allowed, up to `MaxTurnSpeed` (stock 5 rad/s, doubled with fast
turn).
