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

The **low-level controller** that translates high-level commands into actual movement and shooting inputs. One per UnitController at `this+0x2C4`.

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
