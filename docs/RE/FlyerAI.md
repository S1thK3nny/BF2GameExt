# AI flyers circling

Why AI-piloted flyers orbit instead of arriving, and why being shot at does not
snap them out of it. Addresses are modtools unless stated; Phantom twins are
given where its PDB names the function.

---

## The orbit lock

Three flyer-specific facts combine into a trap at the **final** waypoint of any
path. None of them is a bug on its own.

### 1. Flyers are exempt from stuck detection

`PathFollower::Update` — modtools `0x005db240`, phantom `0x00710220`:

```c
GetEntity();
IsRtti(EntityFlyer::rttiHashEntityFlyer);
if (mDone == 0 && !isFlyer) CheckStuck(0x005da480);
```

Nothing will ever notice that a flyer has stopped making progress.

### 2. Arrival is a PLANAR test against a distance with no flyer case

`PathFollower::CloseToDest` — modtools `0x005d9770`, phantom `0x0070f500` —
computes `dx*dx + dz*dz`. The Y delta is computed and then never used in the
comparison, so altitude does not count toward arrival.

For the **last** node it compares against `AIUtil::StopDist`
(modtools `0x0058ebb0`, phantom `0x00489c40`):

```asm
0058ebb0  TEST EAX,EAX / JL  0x0058ebf7
          CMP EAX,1 / JLE    -> 0.5
          CMP EAX,6 / JNZ    0x0058ebf7
0058ebf7  FLD dword [0x00a2a0bc]     ; = 4.0
```

There is **no case for a flyer** — it falls through to a flat **4.0 units**.

Intermediate nodes are fine: `AIUtil::StopDistIntermediate`
(modtools `0x0058eb30`) *does* special-case flyers, giving
`classParam[+0x894] * 0.25` (`[0x00a2a0b8] = 0.25`). The trap is specifically the
terminal waypoint.

### 3. The overshoot test has a flyer-only gate at 10 units

`PathFollower::GonePastDest` — modtools `0x005d9290`, phantom `0x0070f920`:

```asm
005d9338  CMP EAX,0x5              ; ENTITY_FLYER
005d933b  JNZ +0x1f
005d934f  FCOMP dword [0x00a2a418] ; = 100.0  (10.0 squared, planar)
005d935a  JZ   0x005d938e          ; -> return false
```

A flyer more than 10 units from the goto point cannot register as having gone
past it either.

### The consequence

A flyer must pass within **4 units planar** of its final waypoint to register
arrival, or within **10** to register overshoot. If its turn radius keeps it
outside both — which for most flyer ODFs it will — neither test ever fires,
`mDone` is never set, `AILowLevel::IsAtDest` never returns true, the agent never
re-enters `EnterState`, and nothing flags it stuck. It orbits that point forever.

This is agent-independent, and it applies **during combat too**:
`AttackPatterns::GeneratePath` (modtools `0x005c2070`) pushes its strafe
waypoints through the same `PathFollower`.

> **What this is NOT.** An earlier reading blamed the `UnitFlyAgent` FLY -> FLY
> self-loop. That is refuted: the self-loop only runs when `IsAtDest` is TRUE, and
> each firing picks a NEW random command post. A *working* self-loop produces a
> flyer shuttling across the map between posts — not an orbit. The lock is one
> layer down, in `PathFollower`.

---

## Why being shot at does not help

`UnitFlyAgent` overrides `UnitAgent::EventHandler` (vtable slot 20, offset 0x50)
— modtools `0x005aeea0`, phantom `0x00790420`. It **swallows**:

| Event | Result |
|---|---|
| `EVT_Damaged` | calls `PlayDamageEventVO`, then returns 0 |
| `EVT_Audible_CanHearEnemy` | returns 0 |
| `EVT_Grenade` | returns 0 |
| `EVT_EmptyVehicle` | returns 0 |
| `EVT_Vision_CanSeeEnemy` | forwarded, but only when `mCurState != LAND` and `!= EXITHANGAR` and `!IsTransport(this)` |

So an AI flyer under fire never even asks whether to fight back, and a transport
never engages at all. Vision is the only route into combat.

Also worth recording: the state dispatch table at `0x005afe68` handles
`mCurState` 1..5 only (`CMP EAX,4 / JA` at `0x005afd64`), so state 6 `DROPOFF`
has no update handler at all.

---

## Tunables

| What | Site | Note |
|---|---|---|
| Flyer overshoot gate, 10.0 (stored 100.0) | float at `0x00a2a418`, read at `0x005d934f` | flyer-only path — the most targeted lever |
| Terminal `StopDist`, 4.0 | float at `0x00a2a0bc`, read at `0x0058ebf7` | **shared by every unit type** — changing it moves infantry arrival too |
| Intermediate flyer scale, 0.25 | float at `0x00a2a0b8` | already flyer-aware, times `classParam[+0x894]` |
| Flyer CheckStuck exemption | `0x005db240` | re-enabling it would give a fallback escape |

The cleanest fix is probably to give `AIUtil::StopDist` a flyer case mirroring
`StopDistIntermediate`, so the terminal waypoint uses a radius scaled to the
vehicle rather than a flat 4.0. Raising the `0x00a2a418` gate is the smaller
change but only rescues the overshoot path.

---

## RESOLVED: the observed circling was missing map data

The map being tested had **no flyer paths authored**, so there was nothing for the
AI to follow. Reported by the map's author, 2026-08-20. Everything below this line
about `StopDist` and `GonePastDest` is still true of the binary, but it was NOT the
cause of the behaviour that prompted the investigation, and patching `AIUtil::StopDist`
would not have changed it.

Keep the threshold analysis: it is verified, and it would matter for a flyer that DOES
have a path and still fails to arrive. But it is no longer a candidate fix for
"my flyers circle", which should first be answered with "does the map have flyer paths".

**What the engine does with no path is not established.** Worth knowing, because it
would decide whether a fallback is worth building:

- `UnitFlyAgent`'s FLY state has no failed-path branch of its own. The two
  `IsNavigatorFailedPath` consumers found are `UnitTrooperAgent::UpdateStatePostVision`
  (phantom `0x00795D86`, infantry) and nothing on the flyer path.
- The engine already has a path-free movement mode: `AILowLevel::SetNavigator_GotoDirect`
  (phantom `0x00481050`), used by `UnitFlyAgent::DoFlyDirect` (`0x00791210`) for
  `AttackPatterns` mode 17 FLYDIRECT, and by `UnitRandomAgent::PickRandomDest`
  (`0x00793CC0`). So flying to a point without a path network is something the engine
  can already do - it is simply not what the FLY state chooses.
- A plausible feature, NOT built and NOT costed: have the FLY state fall back to
  `GotoDirect` when the path request fails, so flyers work on maps with no flyer path
  network. That would make flyers usable on maps that never authored one.

---

## Open: is the orbit a symptom, or the goal?

The thresholds above are verified from bytes. What is NOT established is that
they are the REASON a given flyer circles. A competing reading: the flyer may be
running a defend or attack goal on an object and circling on purpose, in which
case `StopDist` is what makes the behaviour look like an orbit rather than the
reason it never leaves. Patching `StopDist` would then change nothing worth
having.

The two cases are distinguishable, and it is cheap to do:

| | Trap | Goal-driven |
|---|---|---|
| Current goto point | a distant path node it never reaches | at or near the thing it is circling |
| `mDone` | never set | set, then a new destination issued |
| Re-entry to `EnterState` | never | every cycle |

So the measurement is: for ONE circling flyer, log its current goal and command,
its goto point, and whether `mDone` ever latches. A flyer that never sets `mDone`
on a TRAVEL path to a distant node is the trap. A flyer whose goal keeps
re-issuing a destination it is already effectively at is doing what it was told,
and the interesting question moves up a layer to goal selection.

`UnitFlyAgent`'s state dispatch (`0x005afe68`) and `AILowLevel::IsAtDest` are the
two places to instrument; the LOD interval work in `AISystem.md` shows the shape
a diagnostic like this takes.

---

**Status:** analysis only. Nothing here is patched, and the step from "these
thresholds" to "therefore it orbits" depends on the vehicle's turn radius
exceeding 10 units, which is ODF data and was not measured, AND on the orbit not
being goal-driven in the first place. The thresholds and the `CheckStuck`
exemption are verified from bytes; the causal step is inferred.

---

## Fighter splines: how the AI uses them (researched 2026-10-13)

Read in Phantom for the user's question on the pros and cons of fighter splines, and checked against the
stock map and side sources under `C:\BF2_ModTools\assets`. P = Phantom, M = modtools, given where already
known. Nothing is patched.

**Opting in.**
- **The ship:** its ODF's `PathFollowerClass` names the paths it may fly (stock: `rep`, `cis`, `imp`,
  `all`, `Fighter`, `transport`), and `PathFollowerClass_CTF` gives it other routes while it carries a
  flag. `PathFollowerFlyerVsFlyer` switches the fighting reactions below. It is on unless the ODF says 0
  (`EntityPathFollowerClass` constructor, P `0x005571F0`); stock says 0 only for `all_fly_snowspeeder`
  and `rep_fly_gunship`. `PathFollowerBranchPaths`, `PathFollowerBranchRange`,
  `PathFollowerBranchMaxAngle` and `PathFollowerMinBranchDist` (default 300) shape the branching
  (`EntityPathFollowerClass::SetProperty`, P `0x0055C200`).
- **The path:** a `.pth` path named `type_EntityPath <name>`, read by `EntityPath::ReadPropertyScope`
  (P `0x00556860`): `Class` and `Class1` to `Class5` (a path class, optionally followed by a number, not
  traced), `Direction` (-1, 0, 1), `VarianceX/Y/Z`, `Frequency`, `Speed`, `EnableObject` and
  `SingleDirection`. Stock also writes `MaxSquadSize` and `PathFollowerSpeed`. Four ids are unresolved:
  `0x95EA8376`, `0xF22B0522`, `0x3CBCEB39`, `0x60940EA9`. Nodes can carry `BranchProbability`,
  `SpeedFactor` and `LandOnArrival`, and override the branching (count, angle, and `Range` or
  `BranchRegion`; see EntityPathBranchRegions.md).
- **Live paths:** a path is used while it is enabled (Lua `EnableFlyerPath`) and, if it names an
  `EnableObject`, while that object exists and is alive (`EntityPath::IsEnabled`, P `0x00556400`).
- **Stock use:** 22 path files on the space maps and one on Hoth. On every other map AI flyers fly by
  physics.

**Riding.** While `GetFollowingPath` holds, `EntityFlyer::UpdatePathFollower` (P `0x00535E00`,
M `0x004F30C0`) places the flyer on the curve, as "Flying a path" below describes.
`EntityFlyer::GetBranchParams` (P `0x005261C0`) hands the follower the flyer's own matrix and a speed
from `AIDifficulty::GetFlyerSpeedRange` with a slow wobble; a flag carrier flies in the faster half. The
follower flies 0.8x while turning on an attacker and 1.5x while its boost timer runs.

**Choosing the next path.** `EntityPathFollower::EndOfSegment` (P `0x00559B20`, M `0x005EA9C0`) branches
at a path's end, when a node's `BranchProbability` comes up, or when the flyer has no path. `Branch`
(P `0x00557520`) asks `FindPath` (P `0x00559E20`):
- **Candidates:** only the first and last node of each live path open to the class, in a direction the
  path allows.
- **Score:** `EntityPath::CalculateSuitability` (P `0x00555A60`) multiplies range² / distance², how far
  ahead of the nose the node lies and how well the path's direction there matches the flyer's heading.
  `FindPath` multiplies that by `Frequency`, and an end nearer than `MinBranchDist` counts 0.4x.
- **Pick:** at random among the best `BranchPaths`, each tried with `RayTestSpline` (the joining curve
  against collision), at most four tries, else the closest. `MoveTo` joins with a generated curve.
- **No path:** `Update` fails and `UpdatePathFollower` gives 2 s of physics flight (`mFreeFlyTime`, with
  a trick if one is due), then it tries again.
- **Online:** `Branch` returns at once on a client; the host decides.

**Fighting.** The AI still picks targets, but nothing steers a placed flyer at one.
- **Shooting:** `UpdateIndirect` (P `0x00535530`, M `0x004F4C10`) runs `AILowLevel::ProcessFire`, so it
  hits what crosses its nose. A FlyerVsFlyer class also fires a 0.6 s burst every 1.5 to 6 s
  (`mFireTimer`, 10 s at first). This runs for every AI flyer, on a path or not.
- **Being targeted:** in `EntityFlyer::Update` (P `0x0052EB00`), a flyer whose target is a FlyerVsFlyer AI
  flyer on a path calls the target's `TargetReact` (P `0x0052E1E0`), after a reaction delay from the
  difficulty and at most every 2 s (the timers are not fully traced). Within 700 m the target rolls:
  - **300 to 700 m:** turn and attack (`GetFlyerTurnAndAttackFrequency`), else evade
    (`GetFlyerEvasiveFrequency`), else nothing.
  - **Under 300 m:** evade or nothing.
  - **Turn and attack:** sets `mAttacker` and adds the attacker as a threat. `ShotAtByEnemy`
    (P `0x0055C5D0`) drops the path and builds a curve round toward the attacker, flown at 0.8x. At its
    end `GetNextTowardsAttackerPoint` (P `0x0055A970`) adds a point toward the attacker and starts a 3.5 s
    staredown, the head-on pass. When that runs out it branches back into the network.
  - **Evade:** `mStrafeTimer` = 4π, a sideways weave of up to about 8 m for about 3 s.
- **Damage:** `EntityFlyer::ApplyDamage` (P `0x00523760`) gives an AI FlyerVsFlyer flyer a
  `GetFlyerFreeFlyOnDamage` chance of 2 s off the path in physics flight, with a trick timed by
  `GetFlyerTrickOnDamage` (at most one per 10 s).
- **Side roll:** on maps without `SetGroundFlyerMap(1)`, `UpdateIndirect` side-rolls any AI flyer whose
  target is within 200 m and either within 40 m or more than about 15° off the nose.

**Single-player extras** (skipped when networked):
- **Player-centric:** at a branch, `EndOfSegment` sends about half the FlyerVsFlyer AI toward the local
  player's vehicle (`HeadTowards`, P `0x0055AD20`) when it is far enough away. Each AI's random seed
  fixes which half (`IsFlyerPlayerCentric`, P `0x0055AFC0`; never in 1- or 2-flag CTF).
- **Squadrons and flybys:** see "Wingmen for a player's flyer" below.

**Other uses.** Two-seat flyers circle an AT-AT (`UnitFlyAgent::IsNearATAT` calls `CirclePoint`,
P `0x00557AD0`), path flyers can pick up a CTF flag (`PickupCTFFlag`, P `0x0055B2E0`), and nodes can land
a flyer (`LandOnArrival`, mended by `land_on_arrival_fix`).

**The AI brain barely reaches it.** Of the follower's steering calls (`HeadTowards`, `MoveTo`,
`SetPoint`, `Branch`, `FindPath`, `DelayPath`, `SmoothPath`, `CirclePoint`), only `CirclePoint` is called
from outside it, for the AT-AT. Goals and command posts do not move a spline flyer; the network,
`EnableObject` and Lua do.

**Pros and cons**, as given to the user:
- **For:** cheap and safe (no steering or avoidance, so never into terrain or a capital ship); the map's
  author decides where the fight happens; it looks cinematic; single player stays busy round the player.
- **Against:** a network per map and class, without which flyers circle; short or sharp legs give
  hairpins and jitter; no real dogfighting (nothing chases, and turn-and-attack aims where the attacker
  was); rejoining only at path ends, often far from the fight; the ship's own handling is skipped and
  nothing keeps two ships apart; online loses the extras; a player cannot lead a spline squadron.

---

## Flying a path: where the jitter comes from (researched 2026-10-11)

Read in Ghidra by a research pass and checked against a Python model of the code run on the user's
Coruscant and Dathomir fighter splines; nothing is patched. P = Phantom, M = modtools, S = Steam,
G = GOG; S and G addresses move with the exe.

**A flyer on a path is placed, not steered.** Flyer paths are `EntityPath` objects (Lua
`EnableFlyerPath` reaches `EntityPath::EnablePathByName`, P `0x00556180`, which sets bit 3 of
path+0x88). Each `EntityFlyer` embeds an `EntityPathFollower`: a four-point `PblCatmullRom` window and
a parameter `mT`. Every frame `EntityPathFollower::Update` (P `0x0055d380`, M `0x005ec8f0`, S and G
`0x004d49a0`; on retail ECX = this, XMM2 = dt, six stack arguments, RET 0x18), called from
`EntityFlyer::UpdatePathFollower` (P `0x00535e00`, M `0x004f30c0`, S `0x004ac150`), puts the flyer on
the curve and points its nose along the tangent through `EntityPath::BuildMatrix` (P `0x00555370`,
M `0x005e41a0`, S `0x004d23e0`: heading from atan2(x, z), right vector kept level). The control-driven
flight model does not run, so there are no steering inputs and no steering overshoot.

- The curve is uniform Catmull-Rom (`CalculatePoint` / `CalculateFirstDerivative`: P `0x0086b940` /
  `0x0086b640`, M `0x008495f0` / `0x00849680`, S `0x0072af70` / `0x0072acb0`). The follower never reads
  the path's `SplineType`; the only reader found is `AI::PatrolPath::GetAIPath` (P `0x00713660`), for
  infantry patrol paths, so the editor's setting likely does nothing for flyers (inferred).
- `mT` advances by `min(speed x mCurSpeed / |dP/dt|, 1.05) x dt`, so speed along the curve stays
  nearly constant: Catmull-Rom's uneven parameter speed is not the problem (under 1% frame to frame at
  a steady 60 fps in the model). The 1.05 cap is at M `[0x00a4b834]`, S `[0x007b211c]`,
  G `[0x007b3094]`.
- At each node the window shifts (`SavePoints`, P `0x0055bdc0`) and the new node gets a random offset
  of up to half the path's `Variance` (`RandomVector`, P `0x00556470`), drawn once per flyer per pass.
  Joining or branching restarts the window at the flyer (`CalculateZeroPoint`, P `0x005579e0`).
- Speed is the difficulty speed range with a slow sine wobble (`GetBranchParams`, P `0x005261c0`);
  `mCurSpeed` eases toward the path's and node's `Speed` and drops by `TurnSpeed` in turns. Velocity
  is (new position - old) / dt.

**Roll.** `Rotate` (P `0x0055bc30`, M `0x005e6840`, S `0x004d6cc0`) then `Roll` (P `0x0055b910`,
M `0x005e64f0`, S `0x004d6a10`). The turn input is (previous heading - heading of a look-ahead
tangent) / dt, with the look-ahead at `mT` plus `RollLookAhead` (default 1) clamped to the current
segment and no wrap at ±180°. The bank target is clamped to ±90°, gets a wobble (amplitude 1 with
`SetGroundFlyerMap(1)`, 2 otherwise), is low-passed, and is chased by a bang-bang accelerator with a
speed-scaled dead band. Its ODF keys, all undocumented, are `PathFollowerSpeed`, `TurnSpeed`,
`Acceleration`, `RollAcceleration`, `RollLookAhead`, `RollTolerance`, `RollSpeed`,
`RollAccelerationLag`, `RollTurnFactor`, `RollSpeedFactor` and `RollDampening` (property table
P `0x009f3580`).

**Likely causes of the jitter, most likely first** (effects from the model):

1. **Roll kicks at nodes.** For the last second before each node the look-ahead is stuck at the node's
   tangent, so the flyer takes the turn wings level; at the node it jumps, and the bank target spikes
   for a frame. Crossing due south adds spikes of a full turn divided by dt. On `ground_dogfight1`
   the look-ahead is clamped 40% of the time, with about 0.7 sharp roll reversals a second, nearly all
   at nodes.
2. **Path shape.** The dogfight and strafing paths turn 100-113° at nodes 50-140 m apart, which
   uniform Catmull-Rom makes into hairpins of about 10 m radius (some 300°/s at 60 m/s). `Variance`
   makes short legs worse, sometimes into cusps. Climbs or dives steeper than about 60° spin the
   flyer about its nose, since the heading comes from atan2.
3. **Timing.** The position is evaluated before `mT` advances, so the drawn position lags by a frame
   while the velocity divides by the current dt. With uneven frame times the flyer judders by about
   speed times the frame-time difference (probably what `flyer_sound_fix` smooths; inferred).
4. **Squadrons.** Wingmen run `Rotate` and `Roll` twice a frame, the second pass aiming at about zero
   (M `0x005ecf67`), and `UpdateSquadron` (M `0x005e5e50`) flips a wingman's speed target between 2.0
   and 0.3 times the leader's.
5. **By design:** the wobble, jinks, breakaways when shot at, and free flight with random controls.

A latent oddity: `EndOfSegment` (M `0x005ea9c0`) rolls a node's `BranchProbability` every frame, not
once per node.

**Fixes.** The roll, timing and squadron causes need two hooks per build: a replacement `Rotate` (wrap
the heading difference, take the turn from the real tangent's heading rate plus a look-ahead that
crosses into the next segment using the raw next node, carry the previous up vector above about 60°
of pitch, integrate `Roll` once a frame), and a wrapper on `Update` that places the flyer at the
advanced `mT` and recomputes the velocity. On retail `Rotate` takes dt in XMM3, returns in XMM0 and
ends RET 0x14. The path shape needs either centripetal Catmull-Rom in the two evaluators, as an
opt-in because it changes every flown path, or map edits. `land_on_arrival_fix` NOPs bytes inside
`Update` (a wrapper at the entry is compatible); branch regions and carriers rebuild or write the same
state and need testing.

**To tell the causes apart in play**, log one flyer each frame (dt, `mIndex`, `mT`, the rate, whether
the look-ahead is clamped, `Rotate`'s result, `mCurRoll`, `mRollVelocity`, pitch, speed). Roll flicks
in step with `mIndex` are the clamp; spikes of a full turn over dt are the missing wrap; heading
spikes above 60° of pitch are the vertical spin; judder that goes away at a locked frame rate is the
timing; a wingman's speed flipping is the squadron logic. Quick checks: `Variance` 0 on one path,
`PathFollowerRollSpeedFactor=0` (leaves only the wobble), and the `DrawEntityPaths` console command.

---

## Wingmen for a player's flyer (researched 2026-10-11, not built)

The idea: friendly AI fighters fly with a player's flyer in formation, as in Rogue Squadron, and break
off to fight near the player. Read in Ghidra by a research pass; nothing is patched.

**Two ways an AI flyer flies** (`EntityFlyer::SetupController`, P `0x0052DC10`, M `0x004F1430`):

- **On splines** when the map's paths list the ODF's `PathFollowerClass`: bit 3 of the flyer's flags
  byte (+0x5F0 on P, +0x5F4 on M) is set, and while `GetFollowingPath` (P `0x00526750`, M `0x004D7960`)
  is true (flying, alive, `mFreeFlyTime` not above 0, no trick) the flyer is placed on the path as
  described above. Stock content flies this way in space, and probably on Hoth (inferred).
- **By physics** otherwise. `EntityFlyer::UpdateIndirect` (P `0x00535530`, M `0x004F4C10`; S
  `0x004B37E0`, inferred) flies with ordinary controls: `CalculateDest` (P `0x00523CB0`, M `0x004F4630`,
  S `0x004B3390`) takes `AILowLevel::mDest` (AILowLevel+0x1C, its speed fraction at +0x30), steers
  round barriers and terrain, caps the height, and slows to 30% when the point is off the nose or
  nearer than `MaxSpeed` metres; `AIUtil::UpdatePhysics` then writes move, strafe, turn and pitch.

**A moving point is only ever a fixed point issued again.** `AttackPatterns::DoFlyDirect`
(P `0x00790F70`) calls `SetNavigator_GotoDirect` (P `0x00481050`) on every agent update (0.25 to 4 s
by LOD tier) at the target's last seen position plus a lead, allocating a new `AIPath` each time; the
`0x00791210` given earlier in this file is that call site, not the function. The FLY state
(`UnitFlyAgent::UpdateStatePostVision`, P `0x00790D20`, M `0x005AFCF0`) heads for a random command post.
The one moving target is `UnitFlyAgent::IsNearATAT` (P `0x00790560`): two-seat flyers circle an AT-AT
within 200 m, rewriting the circle's point each update and forcing the walker as the crew's target
(`AddThreat` and `ForceSetAttackHelperTarget`).

**Formations exist, but only for AI, and only offline.** `CreateSquadronFromClosestEntities`
(P `0x005590C0`, M `0x005E8F70`) puts same-side spline flyers on the leader's spline, in slots from
`EntityFormation` paths (space maps author Tri-form, Box-Form and others; the ODF picks one with
`SquadronFormation`), 25-45 m from the leader, slid in by `EntitySquadron::UpdatePosition`
(P `0x0058DE00`). `CanJoinSquadron` (P `0x004DC0C0`) requires the spline mode, so a player can never
lead one. `CreateSquadronForFlyby` (P `0x00558130`, M `0x005EB110`) teleports AI squadrons beside the
local player's flyer to streak past. Both squadron functions return at once when networked.

**The squad key does nothing for a flyer in the air.** In a flyer it is read only while landed
(`EntityFlyer::Update`, P `0x005305DF`, M `0x004FE4A1`), for `SquadCommands::VehicleCommand`
(P `0x00770BF0`, M `0x005C81A0`), which fills or empties seats. `CheckForAutoFollow` recruits only for a
player on foot, `AIShoutOrder` (P `0x00770840`) turns a follow order from someone in a vehicle into
board-vehicle, and `UnitFollowAgent` queues boarding and quits when its leader is in a vehicle. A Lua
`Follow` goal (`AddAIGoal`, P `0x00650D80`, M `0x00473160`) sends command 13, which
`UnitFlyController::ApplyNextState` (P `0x00791A30`, M `0x005A0BC0`, S `0x00667DA0`, G `0x00668E40`)
does not handle, so a flying AI ignores it (inferred). One behaviour worth reusing: a follower with
no target takes its leader's enemy reticule target.

**A design.** Wingmen are same-team AI flying a fighter (transports excluded: `IsTransport` checks
`VehicleType` 5), recruited with the squad key in flight (the nearest few within range), optionally at
takeoff, or from Lua, and released when the player exits, dies or dismisses them.

- Slots: built-in V, echelon, line abreast and trail, or a map's `EntityFormation` by name through
  `CalculatePoint` (P `0x005363A0`), spaced from each flyer's bounding radius, in the leader's frame
  with roll removed and led by about half a second of the leader's velocity (inferred to look steadier).
- Steering: one per-frame hook on `CalculateDest` for wingmen writes the slot into `mDest` and runs the
  original, which keeps the barrier, terrain and height handling, then sets the speed fraction from
  the leader's speed plus a gain on the along-track error (the stock slow-down near the destination
  would otherwise make wingmen fall back). In space, `mFreeFlyTime` (flyer+0x1CCC on P, +0x1CDC on M)
  stays above 0 so wingmen stay off the splines; `EntityPathFollower::Reset` on release.
- Behaviour: a hook on `UnitFlyAgent::UpdateStatePostVision` skips the FLY and LAND states but keeps
  `AttackHelper` running, and `UnitFlyAgent::EventHandler` (P `0x00790420`, M `0x005AEEA0`) drops
  sightings far from the leader. Stock combat still picks targets, aims and fires.
- Orders, after Rogue Leader's command cross (Wikipedia, chromagi) and Ace Combat's
  Cover / Attack / Disperse (acecombat.wiki.gg): form up, engage (fight near the leader on a leash),
  attack my target (forced as `IsNearATAT` does), cover me (whoever damaged or locked onto the leader),
  and loiter while the leader is landed. A tap of the squad key toggles form up and engage, a hold sends
  attack my target, each confirmed by a message and voice line.

Wingmen would be host-side AI, seen by clients as ordinary AI flyers (inferred). `CalculateDest`
avoids barriers but not other flyers, so tight slots may collide (inferred). It shares the
`UnitFlyAgent` hook with the roadmap's fallback for maps without flyer paths, and `AIUtil::UpdatePhysics`
with smoother AI flight.

**Open:** what calls `UpdateIndirect` and how often; whether the stock controller can hold a slot
within a few metres at 70-95 m/s (a prototype answers that); whether the host sees a remote client's
squad key; how a wingman boosts to keep up with a boosting leader (no AI boost path was found outside
the spline mode); how often `AIGoalManager` reassigns a wingman's pilot.

### The prototype (2026-10-13, modtools only)

**Why the squadron code cannot follow a player.** `EntityPathFollower::UpdateSquadron` (P `0x0055E2D0`,
called from `EntityPathFollower::Update`, P `0x0055D380`) adds a member's formation offset
(`EntitySquadron::UpdatePosition`, P `0x0058DE00`, `EntityFormation::CalculatePoint` in the frame it is
handed) to the member's **own** point on the path, then matches its speed along that path to the
leader's. `CreateSquadronFromClosestEntities` (M `0x005E8F70`) returns at once unless the leader has a
path (+0xA8), puts every member on the leader's path, and is off when networked. So a squadron is AI
fighters sharing one path. The formation shapes would carry over; the movement would not.

**How a free-flying AI flyer is steered** (read in P, then M). `EntityFlyer::UpdateIndirect` (M
`0x004F4C10`, `this` the Controllable at entity +0x240) runs `AILowLevel::UpdateIndirect` (the
navigator), then, while flying and not placed on a path (`GetFollowingPath`, M `0x004D7960`: flags
+0x5F4 bit 8, state +0x5A4 = 2, alive, `mFreeFlyTime` +0x1CDC not above 0, no trick), calls
`CalculateDest` (M `0x004F4630` via the thunk `0x00401726`, call at `0x004F4D4D`; thiscall on the entity,
`RET 0x14`). That copies `AILowLevel::mDest` (UnitController +0x2C4 +0x1C, a 0x34-byte
`ControllableDestination`: `mHasDest` +0, `mPos` +4, `mOriginalPos` +0x10, `mDirection` +0x1C,
`mSafeHeightEnforce` +0x28 (SH_FULL 0), `mTaperFactor` +0x2C, `mMaxSpeed` +0x30), avoids barriers every
0.1 s (sliding: flags bit 0x10), raises the point over terrain, and shapes the speed:
- **Off its nose:** a point more than about 26° off the nose (`cos` 0.9) drops the speed factor to 0.
- **Near and misaligned:** a point within one top speed's distance scales it toward 0.3 unless the flyer
  already faces `mDirection`.
- **Floor:** never below 20 m/s.

`UpdateIndirect` then turns it into thrust. The speed is a share of the top speed:
`AIUtil::GetTargetSpeedForRealSpeed` (P `0x00488330`) is speed / max, kept within 0 to 1.
`GetFlyerMaxSpeed` / `GetFlyerMinSpeed` (M `0x004F0950` / `0x004F0A10`) read the class at +0x66C.

**Built (`ai/flyer_wingmen.*`, `[AI] FlyerWingmen=1`, unregistered).**
- **Picking wingmen:** a detour of `UpdateIndirect` does this once a second: the nearest friendly AI
  flyers within 300 m that the leader's class lets follow. It also holds a path flyer's `mFreeFlyTime`
  at 0.5 s so it flies free.
- **Steering:** the call to `CalculateDest` swaps a wingman's `mDest` for a point near its vee slot,
  facing the leader's way, at the leader's speed plus 0.5 m/s per metre the slot is ahead. It puts the
  AI's own destination back afterwards, and keeps 85% of that speed until the point is 60° off the nose.
- **Logging:** BF2GameExt.log reports each slot's error every 5 s.
- **Checks:** `tests/flyer_wingmen_abi_tests.py` audits the reads.

**First test (2026-10-13, the user, a ground map).**
- **Level flight:** wingmen held 10-25 m from their slots.
- **Steady errors:** they sat ~20 m ahead of their slots, unable to fly as slowly as the player's 70 m/s,
  and ~6 m under them.
- **Turns:** hard turns threw them 100-600 m out for 10-20 s.
- **Breaking formation:** they rolled, U-turned and flew into the player.
- **Steering at fault:** the point was the slot carried on 0.5 s, so a wingman past its slot turned round
  for it.
- **Tricks at fault:** `DoTrick` (M `0x004F3D10`, thiscall(Trick), `RET 4`) does the side roll (6) and
  the flips, which are U-turns. `UpdateIndirect` asks for a side roll near a target (M `0x004F5023`).

**Second round (the user's answers).**
- **Steering:** the point now lies across and up to the slot but always ahead of the wingman: as far
  ahead as the slot, plus 0.4 s of the leader's flight (at least 25 m). It is pushed away within 20 m of
  the leader, and a slow trim, from errors under 40 m only, takes out steady height and speed errors.
- **Speeds:** `GetFlyerMaxSpeed` / `GetFlyerMinSpeed` are detoured. The flyer's own `Update` and
  `RecalculateSpeed` (P `0x00528E70`) take its speeds from them. A wingman in formation may fly up to
  30 m/s faster than its leader and down to 20 m/s slower (not under 10), whatever its class allows, so
  it matches a fast, slow or boosting leader.
- **Tricks:** `DoTrick` is detoured to do nothing for a wingman in formation. Camera shake detours it
  too; the two chain.
- **Orders:** the squad key works in flight. The flyer's Controllable holds its trigger at +0x60, 3 when
  pressed, and `EntityFlyer::Update` acts on it only landed (M `0x004FE4AD`). With no wingmen it
  recruits; otherwise it switches between form up and break off (wingmen fly and fight as the AI will
  until called back).
- **ODF keys on the leader's flyer class:**
  - `WingmanClass` names the classes that may follow it, matched by the PblHash of the ODF name, which
    `EntityClass::mId` (+0x18) holds; `EntityClass::Read` looks a ClassParent up by it (M `0x004D08F0`).
  - `WingmanCount` and `WingmanAuto` (join at takeoff, else on the squad key only).
  - A class that sets any of them has wingmen even with the INI off; with none, any class that can keep
    up follows.
- **Not yet:** played, ported to Steam/GOG, an on-screen word for the orders.

**Round two in play (2026-10-13).**
- **A crash at once:** EIP `0x004F09ED` in `GetFlyerMidSpeed`, reading a null class. `EntityFlyer::Update`
  keeps the flyer in EDX across `GetFlyerMaxSpeed`, which touches only EAX, ECX and the FPU stack, and
  loads ECX from it for the next getter (M `0x004FED55`). The C++ stand-in lost EDX. The getters' and
  `DoTrick`'s stand-ins are now naked and keep every register.
- **Then:** wingmen broke their limits, but drifted ahead when the player braked. They flew ~71 m/s
  where ~45 was asked.

**How a throttle becomes a speed** (M `0x004FED19`, read in `EntityFlyer::Update`).
- **The flight code:** the move control below 0 runs from the mid speed (at 0) down to the min (at -1),
  and above 0 from mid up to max (at +1), all three from the getters. The result is kept within min and
  max, stored as the speed the flyer heads for (Controllable +0x358, the entity's +0x598), and flown
  toward.
- **The AI's throttle:** `AIUtil::GetThrustForTargetSpeed` gives 2 x target - 1 for a flyer whose min is
  above 0. So a target taken as speed / max, `GetTargetSpeedForRealSpeed`'s unit, is right only at the
  top: 45 m/s on a 60/75/105 flyer came out at ~73.
- **Round three:** the target is the exact inverse (`target_for_speed`).
- **Feed-forward:** it takes the player's +0x598, the speed the player's throttle asks for, rather than
  how fast the player flies, so a wingman brakes or boosts with the player rather than after.
- **Speed limits:** a wingman's speeds stretch round both.
- **Turning:** in formation it flies that speed exactly until its point is 60° off its nose.

**Round three in play:** the user: "Much better, they slow down with me now."

**`WingmanSlot` (the user's idea).** A leader class can lay out its own formation, one slot per line,
written as a command vehicle's `SpawnPointLocation` lines are:
- **Format:** `WingmanSlot = "x y z"`, in the leader's frame with its roll left out (x right, y up,
  z ahead). A fourth number, the spawn points' angle, is ignored.
- **Order and count:** wingmen take the slots in order. The lines set how many wingmen the class takes,
  unless `WingmanCount` asks for fewer.
- **Inheritance:** a child class's own lines replace the slots it inherits.
- **Without any:** the vee.
- **Limit:** up to 8 wingmen (was 4).
- **Slot classes (the user's follow-up):** `WingmanSlot = "x y z [class ...]"`.
  - Classes named after the offsets keep the slot for themselves. A name not on the leader's
    `WingmanClass` list is ignored, and a slot left with none is open to any class that may follow.
  - The free slots fill in order, each with the nearest flyer that may take it.

**Copying the leader's tricks (the user's ask, 2026-10-13: rippled, free of energy).** `DoTrick` (M
`0x004F3D10`) does nothing for a class with no single-tap cost (+0xD8C), makes any trick a side roll for one
with no double-tap cost (+0xD90), refuses while a trick runs (flags +0x5F4 & 3: `RollAdd` sets bit 0,
`FlipAdd` bit 1) or when `EnergyBar::SpendEnergy` (M `0x004D0050`) on the bar at +0x618 refuses (only while
its +8 bit 0, spent out, is set; else it takes the cost from +4, the entity's +0x61C), and records the trick
it did at +0x5E4. The tricks (`EntityFlyer::Trick`): FLIP_UP 0, FLIP_DOWN 1, FLIP_UP_RIGHT 2,
FLIP_DOWN_RIGHT 3, FLIP_UP_LEFT 4, FLIP_DOWN_LEFT 5 (half loops, the diagonal ones with a roll added),
SIDE_ROLL 6 (a full roll, its side the sign of `mSideRoll.x`, +0x5D8; the player's double-tapped strafe
sets it to -1 or 1 first, the AI's own roll to a world direction).
- **Built:** the `DoTrick` stand-in notes the leader's call (the trick asked, the side); when the leader is in
  a trick by the next update, each wingman in formation copies the trick it did, in slot order, 0.15 s
  apart, the first 0.15 s after the leader. A copy calls the original past the stand-in (camera shake's
  `DoTrick` detour acts only for the viewed flyer), with the energy and the spent-out bit put back round it.
- **First look (the user):** after a side roll wingmen flew off for a moment. A side roll is a dodge:
  while `mSideRoll.x` is set, `EntityFlyer::Update` (M `0x004FEE51`) eases the flyer's sideways speed
  (Controllable +0x35C) toward `mSideRoll.x` times a class speed (class +0xDA4) at a class rate (+0x884),
  and once it is cleared lets it ebb at a thirtieth of that rate. A rippled wingman turned for the slot the leader's dodge had moved, then dodged as well, and its
  steering acted in a rolling frame.
- **Fix:** from the leader's trick until its own copy has ended plus 0.3 s, a wingman is steered at a point
  100 m straight off its nose at the leader's speed (its trims left alone), then back to its slot.
- **Flips (the user's idea):** a flip is a half loop, so the leader comes out heading back the way it came
  with its wingmen across from where they were. Each leader flip mirrors the formation, left for right, so
  a wingman falls back into a slot on its own side instead of crossing the leader; a new formation starts
  unmirrored. Not yet played.

---

## Danger regions (researched 2026-10-13)

A danger region is a no-fly zone for AI flyers that steer by physics. Read in P; nothing is patched.

- **Authoring:** a box region whose class string starts with `danger` (`Region("danger", 0)` in the
  `.rgn`). The `danger` factory belongs to `FlyRegionManager`, built in `GameState::PreStateInit`
  (modtools factory vtable `0x00A47014`, see EntityPathBranchRegions.md). `FlyRegionManager::CreateRegion`
  (P `0x005C64C0`) rejects any other shape ("Danger region %s must be a box") and adds 1000 to the box's
  height (`PblRegion::mSize.y`, descriptor +0x44), so in practice it is a column taller than any map.
- **Steering:** `CalculateDest` (P `0x00523CB0`, M `0x004F4630`) calls `AIUtil::AvoidBarriers`
  (P `0x00484500`) every 0.1 s, unless the navigator's path sets a flag (not traced). It looks one second
  ahead along the flat heading at the class's top speed. If planning barriers block that point, or
  `FlyRegionManager::RayTest` (P `0x005C66A0`) finds the line crossing a danger box widened by the
  flyer's collision radius (at least 15 m), the flyer slides: it heads for a point square to its heading,
  always on the same side, two seconds of top speed away (at least 30 m), and re-checks every 0.1 s. It
  turns sideways only, never over or under.
- **Reactions:** `UnitAgent::HandleDamagedEvent` and `UnitAgent::Handle_Audible_CanHearEnemy`
  (P `0x00783F70`, `0x007847B0`) ignore the event for a flyer inside a danger box
  (`FlyRegionManager::IsInside`, P `0x005C65E0`). `UnitFlyAgent` swallows both events before them (see
  "Why being shot at does not help"), so for an ordinary AI pilot this likely never runs (inferred).
- **Not affected:** spline flyers while placed (`CalculateDest` does not run) or on real flight (the check is
  held off, "Real flight on paths" below), human pilots, ground units.
- **Stock use:** none; no stock region file names one.
- **Wingmen:** they fly through `CalculateDest`, so they slide away from danger regions too.

---

## Real flight on paths (prototype, 2026-10-13, modtools only)

The user's ask after the spline write-up: let spline flyers fly for real. Built on the wingmen's hooks
(`ai/flyer_real_flight.*`; `[AI] FlyerRealFlight=1`, or `PathFollowerRealFlight = 1` on a flyer class,
over the INI; unregistered). Not yet played.

**The carrot.** `EntityPathFollower::Update` (M `0x005EC8F0`, thiscall, 7 arguments, `RET 0x1C`) moves the
matrix its `BranchParams` points at (+4), and `GetBranchParams` (the follower's vptr +0x1C, M `0x004F52D0`,
`RET 4`, the same slot in all three flyer types' follower vtables) hands it the flyer's own. A flyer on real
flight has a matrix of ours, the carrot, and the `UpdateIndirect` detour steps the follower on that
instead. The flyer is held in free flight, and the `CalculateDest` call steers it at the carrot, along the
carrot's heading, at the carrot's speed, as the wingmen are steered at their slots.
- **Pace:** the carrot runs a lead ahead (a second of the flyer's speed, 40 to 150 m). It is stepped by `dt`
  times 2 - distance / lead (0 to 2), so it waits with the flyer two leads behind and runs on as the flyer
  catches it.
- **Speed:** the flyer is asked for the carrot's speed, the path's, plus 0.5 m/s per metre past the lead.
- **Starting over:** four leads behind (at least 300 m), the follower is reset and the carrot starts again
  from the flyer, so the next step branches onto a path from there.

**Free flight resets the follower every update.** `EntityFlyer::Update` (M, EBX the Controllable) calls
`UpdatePathFollower` while the flyer is on its path (`0x004FDD93`, through the thunk `0x00411CA2`), makes
the same test again with the free-flight time read afresh, and off the path calls
`EntityPathFollower::Reset` (`0x004FDDED`, through `0x00404133`), every update. So a flyer back from free
flight always branches onto a path from where it is. Both CALLs are redirected:
- **`free_reset_mt`** keeps the follower of a flyer on real flight.
- **`update_path_follower_mt`** holds a flyer on real flight, or a wingman in formation, whose free flight
  something cut short (`TargetReact`, P `0x0052E1E0`, zeroes it), instead of letting the follower place it
  for an update; the second test then flies it.

**As the stock code takes it.** A follower that finds no path leaves the flyer to the AI for 2 s, the free
flight `UpdatePathFollower` gives. `mbLandNow` lands it (`Land`, M `0x004F1380`, which the LandOnArrival fix
detours). `TargetReact`'s trigger does not test the free-flight time, so turn-and-attack and the weave move
the carrot.

**What else changes for these flyers:**
- **Danger regions and barriers** do not apply: their check is held off (below, "First play").
- **`EntityFlyer::RaiseHeight`** (P `0x00528910`, M `0x004F40A0`) lifts the point 25 m over terrain and,
  when a collision ray from the flyer along its nose, 1.5 s long, hits something, over that object's
  bounding sphere, asking 20 m/s. The path's speed is not held then, and the ray counts only the bodies
  vehicles hit (below).
- **Corners:** in the tests' kinematic model (90°/s, 30 m/s²) the flyer cuts about 15 m inside a 150 m
  turn, and a 10 m hairpin swings it wide and back onto the far leg.
- **No stock squadrons or flybys:** both form only in `UpdatePathFollower`, and `CanJoinSquadron`
  (P `0x004DC0C0`) takes only flyers whose free-flight time is not above 0.
- **Landing on arrival** comes about a lead early, when the carrot reaches the node.

**Checks.** `tests/flyer_real_flight_tests.cpp` (the core, and a chase in the kinematic model) and
`tests/flyer_real_flight_abi_tests.py` (53 modtools instruction checks, the vtables, the guards).
BF2GameExt.log reports every 5 s: flyers on real flight, the carrot's distance against the lead, the speeds,
restarts and paths not found.

**First play (2026-10-13, the user, Cato Neimoidia in the BF3 mod).** Flyers fell into endless circling,
and those launching from the capital ships stayed in the hangars.
- **The log:** no path ever missing and few restarts, but carrots 1.6 leads ahead on average (up to 570 m)
  and flyers at 60 to 70 m/s against the path's 80 to 94: flyers going round rather than after their
  carrots.
- **Danger regions:** Cato has seven (`cato.rgn` five, among them a 700 x 100 box; one round each capital
  ship, 304 x 514 and 211 x 411). Of its 46 fighter paths, 2 to 17 cross each box (Size read as full or half
  extent), with up to 71 nodes inside one, the capital ships' hangar and takeoff paths among them.
  `AvoidBarriers` then slides the flyer off to one side every 0.1 s while the line ahead stays in the box:
  round and round its carrot, and never out of a hangar inside one.
- **Fix:** CalculateDest checks once the flyer's +0x650 is due (M `0x004F47D9`; only `CalculateDest` and the
  constructor touch it, P `+0x64C`) and calls a slide off only a second after a check (`0x004F4682`). For a
  flyer on real flight it is set to FLT_MAX and any slide flag (0x10) cleared before each `CalculateDest`,
  and set back to 0 when the flyer leaves real flight or loses its path. Placed flyers never met either.
- **Hangar shields (the user's find):** a shield that lets vehicles through still stopped them, and the
  user suggested the vehicle collision mask. `RaiseHeight`'s ray, `CollisionManager::RayHit` (M
  `0x0042E230`, types 0x18 rigid and static, ordnance only false), tests every body of each model:
  `CollisionModel::RayHit` (P `0x004B93B0`, M `0x004359C0`) skips bodies only with ordnance only set, and
  then by the ordnance mask. A collision model keeps a body mask per collider (P `CollisionModel_data`:
  soft +0x108, rigid +0x110, static +0x118, terrain +0x120, ordnance +0x128, tow cable +0x130, targetable
  +0x138, flying ordnance +0x140; M +4 for the vtable), filled from the ODF's `SoldierCollision`,
  `VehicleCollision`, `BuildingCollision`, `TerrainCollision`, `OrdnanceCollision` and
  `TargetableCollision`; `CollisionObject::CheckForCollision` (P `0x004B9C80`) picks which mask of each
  side two objects collide with, a flyer by `mIsFlyerFlag` and `mUseVehicleCollisionAgainstFlyers`.
- **Fix:** `RaiseHeight`'s CALL (M `0x004F429E`, the flyer its one ignored object) goes to `raise_ray_mt`,
  which for a flyer on real flight or a wingman in formation sends the ray out with ordnance only set,
  and `CollisionModel::RayHit` is detoured to read the model's rigid mask (+0x114, `GetCollisionMask`'s
  case for 8) as its ordnance mask (+0x12C) during that ray only.
