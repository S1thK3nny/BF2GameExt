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
600/sec, which is why the budget never binds.

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
