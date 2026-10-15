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
  and once it is cleared lets it ebb at a thirtieth of that rate. A rippled wingman turned for the slot the
  leader's dodge had moved, then dodged as well, and its steering acted in a rolling frame.
- **Fix:** from the leader's trick until its own copy has ended plus 0.3 s, a wingman is steered at a point
  100 m straight off its nose at the leader's speed (its trims left alone), then back to its slot.
- **Flips (the user's idea):** a flip is a half loop, so the leader comes out heading back the way it came
  with its wingmen across from where they were. Each leader flip mirrors the formation, left for right, so
  a wingman falls back into a slot on its own side instead of crossing the leader; a new formation starts
  unmirrored.
- **In play (a3a1971, the user, 2026-10-13):** "Great stuff, works!"

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
over the INI). Played on Cato Neimoidia: below, "First play" and "Second play".

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

**Second play (9e6221f, the user, 2026-10-13):** "That seems to have fixed the circling."

**Still to watch in play:**
- **Climbs:** `RaiseHeight` still lifts the point over what vehicles hit within 1.5 s of the nose, and 25 m
  over the terrain ahead, so paths hugging a capital ship, running under bridges or strafing low are flown
  higher than drawn.
- **Corners:** cut inside, tight hairpins swung wide; watch for scraping near buildings and hangar mouths.
- **Collisions between flyers:** nothing keeps two apart (`CalculateDest` avoids barriers only), so flyers
  sharing a path can now collide. Avoidance (below) is the answer, as a prototype of its own.
- **Landing on arrival** a lead early; **no stock flybys** for these flyers (their squadrons are ours now,
  below); **danger regions** ignored, as placed flyers always ignored them.

**Built next (2026-10-14, the user's ask):** real pursuit, AI squadrons and flyer-to-flyer avoidance, in
the three sections below; Steam, GOG and online (host only) still to come.

---

## Real pursuit (prototype, 2026-10-14, modtools only)

The user's ask: let a flyer on real flight turn on its attacker for real. On with real flight
(`[AI] FlyerRealFlightPursuit=1`, the default). Not played yet.

**How the game turns a flyer on its attacker** (read in P, then M).
- **The trigger:** in the attacker's own `EntityFlyer::Update` (M `0x004FDC69`), when its target is a flyer
  whose class's path class is FlyerVsFlyer (class +0x6AC + 0x78) that flies paths, flying, alive, in no trick
  and not already turning (`mbQueuePoint` +0x49D, `mbGoMinSpeed` +0x49E), at most every 2 s (+0x1AAC) and after
  a reaction delay (+0x1AB0): the target's `TargetReact(attacker)` (M `0x004F0C30` through `0x0040550B`,
  thiscall, `RET 4`). Human and AI attackers alike. The target need not be placed on its path, so flyers on
  real flight are turned too.
- **`TargetReact`:** within 700 m, turn and attack (300 to 700 m, by `GetFlyerTurnAndAttackFrequency`) zeroes
  the free-flight time and sets the follower's `mAttacker` (follower +0xB4, the entity's +0x480) to the
  attacker's `Character` (its Controllable's +0xCC), and adds the attacker as a threat. Evading (inside 300 m,
  or by `GetFlyerEvasiveFrequency`) winds the weave (`mStrafeTimer`, follower +0x70).
- **The follower acts on it in its next step:** `EntityPathFollower::Update` calls `ShotAtByEnemy`
  (M `0x005EBDA0` through `0x004112A7`), which, for a FlyerVsFlyer class with `mAttacker` set and not already
  turning, resets the follower and builds a curve toward the attacker's flyer
  (`Character::GetControllable`: its remote +0x150, else its vehicle +0x14C, else its unit +0x148). At the
  curve's end `GetNextTowardsAttackerPoint` points it at the attacker for a 3.5 s staredown
  (`mStaredownTimer` +0xC4) and clears `mAttacker`; when that runs out it branches back.
  `EntityPathFollower::Reset` leaves `mAttacker` alone.

**Built (`ai/flyer_real_flight.*`).**
- **Taking the attacker first:** the carrot's step is where `ShotAtByEnemy` would act, so before each step the
  `UpdateIndirect` detour reads `mAttacker`. When it is a flyer on another side, it is cleared (so the
  follower never bends toward it) and the flyer chases it. Another attacker while chasing switches the
  chase to it; one that is not a flyer is left to the game.
- **The chase:** the carrot waits. The `CalculateDest` stand-in steers at the target led by its velocity for
  the time the chaser needs to cover the distance (at most 1.5 s), flat out to close or to turn onto it, and,
  with the target within ~45° of the nose, at the target's speed plus 0.5 m/s per metre past a 120 m gap
  (`pursuit_point`, `pursuit_speed`). The speed is held until the point is ~45° off the nose; past that the
  game slows it to turn. Shooting stays the game's (`ProcessFire`, the FlyerVsFlyer bursts; the attacker is
  already its threat).
- **The leash:** 15 s from the last provocation (`TargetReact` re-arms it every 2 s while the attacker keeps
  it targeted), 40 s at most; over sooner when the target dies, lands, changes sides or gets more than
  1000 m away. Then the carrot starts over from the flyer, so its follower branches back onto the paths, and
  for 4 s it turns on no one.
- **RaiseHeight's ray passes over flyers** for a chasing flyer (and for every flyer with avoidance on): a
  flyer ahead is a rigid object to it, to climb over at 20 m/s, which would stop any chase at gun range. The
  ray's CALL is already redirected (`raise_ray_mt`): a hit whose `CollisionObject` belongs to a flyer (its
  vptr +0x38, as the ray callback M `0x00429E00` asks it) is added to the ignore list and the ray cast again,
  at most four times.
- **Squadrons:** a squadron's wingman that is turned on hands its attacker to its leader, and the whole
  squadron goes with it (below); the player's wingmen in formation leave the fight to the player.
- **Checks:** `tests/flyer_real_flight_tests.cpp` adds the lead, the speed, the leash's clock and a chase of a
  target circling at 20°/s (the chaser settles 83 to 89 m behind it, never more than 28° off its nose);
  `tests/flyer_real_flight_abi_tests.py` the trigger, `TargetReact`, `ShotAtByEnemy` and the ray callback
  (67 modtools checks). The log names each chase and why it ended.

---

## AI squadrons on real flight (prototype, 2026-10-14, modtools only)

The game's squadrons ride splines, so flyers on real flight never form one. The user's ask: AI-led
formations through the wingmen code. On with real flight (`[AI] FlyerRealFlightSquadrons=1`, the
default). Not played yet.

**The game's rule** (P `EntityFlyer::UpdatePathFollower` `0x00535E00`, M `0x004F30C0`).
- **When:** each update a placed flyer not in a squadron counts `mSquadronFormationTime` (+0x1CE8) down by dt;
  `ResetSquadronTime` (M `0x004F08C0`), once it is not above 0, sets it to 7 ± 1.5 s and says to try.
- **Reach and angle:** the class's `mSquadronSquaredDist` (+0x1108), else its model's bounding radius (the
  class's `GetModel`, vptr +0x20, then +0x20, +0x94) times 30, squared; `mSquadronMaxAngle` (+0x1104), a cosine.
  Both default to 0 (M `0x004FA01E`), and no stock or BF3 ODF sets them: so within 30 bounding radii and
  heading within 90°.
- **Who:** `CreateSquadronForFlyby` first (offline, beside the player), else
  `CreateSquadronFromClosestEntities` (M `0x005E8F70`, off when networked): only a FlyerVsFlyer path class,
  only with `SquadronFormation` names, only on a path; candidates from the list every follower joins (below),
  same side, `CanJoinSquadron` (placed on a path, not in a squadron or a trick), within the reach and angle,
  scored 2 + cos − d²/r²; as many as the path's `MaxSquadSize` (path +0x84; unset, no limit) lets.
- **How many:** `CreateSquadron(n + 1)` picks one of the class's `SquadronFormation` shapes with exactly that
  many slots at random (`EntitySquadronClass::SelectFormation`, M `0x005EDC20`: names at the class's +0x6AC,
  their count at +0x6D4); with none it tries one fewer, down to three flyers (a pair only when one candidate
  was all there was).
- **Shapes:** `EntityFormation` paths (`type_EntityFormation <name>` in the `.pth`): their points, a `Root`
  node (index +0x28, else the first) and `rootisslot` (+0x2C bit 0, default 1). `EntityFormation::Read`
  makes each point relative to the root; it means to turn them by the root's inverse rotation too, but
  `PblQuaternion::RotateVector` returns by value and the result is dropped (P `0x00536510`, M `0x005E2CD4`),
  so the offsets stay in the path's axes, which the game reads as x right, y up, z ahead. The leader takes
  the root when it is a slot, else the first other point; members take the rest in order.
  `EntityFormation::Find(name)` (M `0x005E2EB0`, cdecl) walks the map's formations (list head M
  `0x00AD33C0`).
- **Which way they face (2026-10-14, the user: "do whatever math to make it work properly"):** every path
  node is read by `RedPath::ReadScope`'s node reader (M `0x00820470`), which negates the position's z (the
  editor's +z is the game's −z) and stores `Rotation`'s numbers as s, x, y, z (the `.pth` writes w x y z).
  So a formation drawn with its leader in front flies with its leader at the back, mirrored front to back:
  from outside it still looks like a formation (no one can tell which ship leads), which is why the stock
  squadrons look fine, but the leader's wingmen sit ahead of it. Drawn, the stock shapes are what their names
  say: spa2's eschleft-form an echelon left and behind, eschrgt-form right, spa1's Tri-form a vee behind its
  point, Line-Form a line astern. The BF3 maps' nine formations are the stock shapes turned 105° in the
  editor (every node's rotation says so; turned back, Cato's five-form is spa2's to 0.1 m), while the stock
  maps' node rotations mean nothing (spa1's Tri-form turned back by its −88° is no longer a vee).
- **Stock use:** the space maps author Tri-form, Line-Form, Box-Form, five-form, eschleft-form,
  eschrgt-form and wall-form, every path `maxsquadsize(3)`; the BF3 mod's Cato Neimoidia adds escort-form
  and bomber-Form and sets no `maxsquadsize`. Formations of 3 or 4.

**Built (`ai/flyer_wingmen.*`).** The wingmen code now runs up to 33 formations: the player's and up to 32
AI squadrons, each with its leader, slots, trims, trick copying and flip mirroring.
- **Forming:** a free AI flyer on real flight, on a path and not chasing counts its `mSquadronFormationTime`
  down as the game would (the game's `UpdatePathFollower` no longer runs for it), and tries by the game's rule
  above, with flyers on real flight as the candidates. The leader class's `WingmanClass` list, where set,
  limits who may join; else a flyer must keep up (top speed at least 90% of the leader's).
- **Shape:** one of the class's `SquadronFormation` shapes the map has, chosen as the game chooses, flown
  as drawn: the z flip taken back out and, on a map whose formations are placed turned, turned back by
  their own rotation. Which of the two a map needs is decided per map: turned back when more of its
  formations are then led from the front (no member more than 3 m ahead of its leader) than as drawn (the
  BF3 maps 9 of 9 against 6; the stock maps keep theirs). Scaled up until no two flyers are nearer than
  22 m (five-form puts its first member 12.6 m off the leader: fine for placed flyers, not for flown ones).
  A class with no `SquadronFormation` but its own `WingmanSlot` lines flies those; with neither, no
  squadron, as the game.
- **Flying:** each wingman is steered to its slot as the player's wingmen are (the leader's speeds
  stretched round its own, its tricks copied, flips mirrored).
- **Fighting:** a leader turned on its attacker chases it (above) with its squadron in formation behind it. A
  wingman turned on hands its attacker to its leader, unless the leader already chases, so the squadron
  turns as one.
- **Breaking up:** a wingman is let go as the player's are (dead, landed, flown by someone else, changed
  sides, more than 800 m off); the squadron breaks up when its leader dies, lands, leaves real flight or
  joins another formation, or when its last wingman goes. Its wingmen go back to real flight at once.
- **The player:** may call an AI squadron's wingmen away into their own formation, never its leaders.
  The player's flight flies the map's shapes too (the user's idea), worked out the same way: the class's
  `WingmanSlot` lines where it has them; else the first map shape its new `WingmanFormation` key names
  (`WingmanFormation = "Box-Form"`, several allowed, the first the map has wins); else the first of its
  `SquadronFormation` shapes that holds all its wingmen (else the largest); else the vee. A shape holds
  no more wingmen than its slots.
- **Checks:** `tests/flyer_wingmen_tests.cpp` adds the rule (timer, reach, candidates and their order, how
  many) and the shapes (Tri-form, five-form, Box-Form from the stock maps, the scaling; spa1, spa2 and Cato's
  shapes as drawn, Cato's turned back to spa2's, the per-map choice); `tests/flyer_wingmen_abi_tests.py` the
  rule's reads, `SelectFormation`, `Find`, `GetNumSlots`, `Read`, the node reader's z flip and rotation
  order, and the class defaults (83 modtools checks). The log reports each squadron formed (leader, shape, members and
  their distances), each break-up and why, and every 5 s how far off their slots the wingmen fly.

---

## Flyer-to-flyer avoidance (prototype, 2026-10-14, modtools only)

The user's ask: avoidance between flyers, wholesale. `[AI] FlyerAvoidance=1` (off by default). Not played yet.

**What the game has.** Nothing: `CalculateDest` keeps an AI flyer off barriers, danger regions, terrain and
water, and `RaiseHeight`'s obstacle ray (rigid and static objects 1.5 s along the nose) takes a flyer ahead as
an obstacle: it lifts the point over its bounding sphere and slows to 20 m/s. Placed flyers are never
steered at all.

**Every flyer, one list.** The `EntityPathFollower` constructor (M `0x005E89F0`) links every follower into one
list: head node M `0x00AD3524` (+4 the first node; a node's +4 the next, +0xC its follower; the head's +0xC
null; the count at +0x10), linked in at `0x005E8AE3`. Only `EntityFlyer`'s constructor builds a follower
(through `0x0040E42B`, once, at the entity's +0x3CC), so the list is every flyer, human or AI;
`CreateSquadronFromClosestEntities` walks it the same way. A flyer's collision sphere is read as `RaiseHeight`
reads its own (M `0x004F4259`): the TreeGridObject's sphere stack (+0x10, its index +0x14, the radius at stack +
index × 16 + 0x4C), else its own radius (+0x24). States (`EntityFlyer::Land`): 1 taking off, 2 flying, 3 landing.

**Built (`ai/flyer_avoidance.*`).**
- **Who:** every AI flyer `CalculateDest` steers (free-flying AI on any map, flyers on real flight, chasers,
  wingmen of either kind), against every other flyer taking off, flying or landing, the player's included.
  Placed flyers keep the stock behaviour.
- **When:** after `CalculateDest` (and the wingmen's or real flight's own steering), the stand-in calls
  `flyer_avoidance::adjust`. Once a frame the flyers are read from the list. For each other flyer within
  400 m, the closest approach within 2 s, both holding their velocities, is a threat when it falls inside the
  two radii plus 8 m.
- **How:** the point it steers at is bent away from where the threat will be, square to its own heading, the
  harder the nearer and the sooner (at most ~66° off), and a slower one ahead going its way eases its speed
  target by up to 40%. On its line of flight it breaks right with the other ahead (two meeting head on both go
  right) and left with it behind (one being overtaken makes way). A point the game lifted over terrain or an
  obstacle is bent to the side only, never lower.
- **The climb over flyers goes:** `RaiseHeight`'s ray passes over flyers (see "Real pursuit").
- **Checks:** `tests/flyer_avoidance_tests.cpp` meets two kinematic flyers (90°/s, 30 m/s²): head on they
  pass 19.2 m apart (1.0 m blind), 5 m offset 19.2 m (5.1), crossing at right angles 16.3 m (0), overtaking
  19.0 m (0.2). `tests/flyer_avoidance_abi_tests.py` audits the list, its one builder, the sphere reads
  and the states (11 modtools checks). The log reports every 5 s how often it steered and the nearest pass
  it foresaw.

---

## Wingmen orders (prototype, 2026-10-14, modtools only)

The user's answers to the questions: call up the nearest suitable flyers, from anywhere (with a
`WingmanClass` gate only those classes, however far); make up losses at any time; form up, engage, attack my
target and cover me as proposed, attack my target only with a locked target or an enemy under the reticule
as the order is given; try having the wingmen land with the player; reuse the trooper command keys; show
the orders as the trooper commands show theirs; formation cycling later. Built in `ai/flyer_wingmen.cpp`
and `ai/flyer_orders_core.hpp`. None of it played yet.

### What the game offers (read in P, then M)

- **The squad key in flight:** the flyer's Controllable trigger (+0x60), whose low two bits are both set on
  the update it is pressed (`EntityFlyer::Update` tests for 3). Which bit says held is not traced. Landed, the
  game uses it itself (`SquadCommands::VehicleCommand`, seats).
- **The trooper commands' keys:** on PC there is one squad key, read by `EntitySoldier::Update` (M from
  `0x00549851`): on a friendly under the reticule it is "follow", else hold or move out. The eight comm keys
  (`RawControllerInputs::TeamOrderInputProcess`, P `0x00454440`, M `0x00745D00`, keyboard rows 0x14 to 0x1B)
  send team chat with a code: `Spotted`, `NeedMedic`, `NeedRepair`, `NeedAmmo`, `NeedPickup`, `NeedBackup`,
  `AttackPosition` (6), `DefendPosition` (7). Each press ends in one CALL to `NetGame::AddChatMessageToQue`
  (M `0x00745DC3` to `0x0070C260`, cdecl, seven dwords, the code the fifth, the caller pops them). Only
  online: `RawControllerInputs::ReadLatest` (P `0x004539C0`, M gate `0x0074695D`) runs `ChatInputProcess`,
  and with it `TeamOrderInputProcess`, only when `netEnabled` (`netEnabledNext` in the shell); in single
  player `StandardInputProcess` alone (P `0x00454170`, M `0x007455F0`), so the comm keys send nothing there.
- **The comm keys as inputs:** `StandardInputProcess` still turns every binding into the player's
  `ProcessedInputs`, the comm keys among them (`ePROCESSEDINPUT_Comm_Chat_0` to `_7`, digital inputs 20 to 27,
  F5 to F12 by default; Attack 26, Defend 27): bit n of +0x10 (`ProcessedInputs::GetDigital`, M `0x00743A30`,
  n below 0x1E). `ReadLatest` clears them each frame (`ProcessedInputs::Clear` keeps the top two bits), then
  `ProcessedInputs::Set` (M `0x00743710`) sets the bit of input 8 or 13 to 28 only when now is pressed and
  last was not. A key held down is Set with last 0 and now 1 (M `0x007459A9`), so its bit is set every frame
  it is held; a pad's button, Set with its last frame, the frame it goes down only. In single player
  `NetGame::IsTurnReady` copies player 0's inputs whole into `hostJoysticks[0]`, which the player's
  `PlayerController` reads as `mJoystick` (+0x88, from `NetGame::GetJoystick` as it is built; M
  `PlayerController::Update` loads it at `0x0059B6A2`). The pad bindings' hook on `StandardInputProcess`
  runs the stock pass first and adds its own, so a key's bits stand.
- **The trooper commands' text:** `EntitySoldier::Update` looks up `game.squadcommand.follow` (hash
  `0x731FD493`, the localisation lookup M `0x007F3720`, cdecl(hash, bool nullIfMissing)), copies the colour
  at M `0x00AE2170` (`DF DF 20 FF`) and hands both to the player's HUD object (M `0x0067EBB0`), which calls
  the message line, M `0x006AECD0`, cdecl(player, text, color*): it sends `player%d.message.color` (type 7, M
  `0x00BA3D08`) and `player%d.message` (type 8, M `0x00BA3D04`), registered by `HUD::GameEvents::Open`. The
  four-way squad command menu (`StatusDisplay::SetSquadCommand`, `game.squadcommand.<context>.<up|down|right|
  left>`) is shown by `SquadCommands::ShoutOrder` (M `0x005C7450`; empty in P).
- **The player's target:** `Controllable::GetTargetLockedObj` (P `0x004EE4F0`, M `0x005E0C70`, thiscall(), EAX)
  refreshes the lock from the player's `LockOnManager` into +0x138 (handle +0x13C). The reticule's target is
  `mReticuleTarget` (M +0x164, handle +0x168, a second channel 8 bytes on), as the trooper command reads it
  (M `0x0054985F`); M's Controllable fields from `mCharacter` on are P's plus 0x3C.
- **Enemies:** `GameObject::GetAffiliationToMe` (M `0x0055F840`, thiscall(other), RET 4): below 0 an enemy
  (the trooper command will not recruit one, M `0x00549896`).
- **Making an AI attack something:** `UnitController::ForceSetAttackHelperTarget` (P `0x0078B170`, M
  `0x0059EC20`, thiscall(target), RET 4) calls `AttackHelper::SetVisibleTarget` (M `0x005B43D0`) on its agent's
  (+0x2C0) AttackHelper (+0x10): the target at +0, its handle at +4. The AI's own combat sets the same.
- **Landing:** `EntityFlyer::Update`'s state switch (M `0x004FE7B2`, on the Controllable's +0x364): the player's
  use key lands a flying flyer (3) and takes a landed one off (1); landing, a ray straight down (RayHit, types
  0x190, 1024 m) measures the height until it is down (0). `EntityFlyer::Land` (M `0x004F1380`) and
  `EntityFlyer::TakeOff` (M `0x004F8B70`), both thiscall() on the entity, set those states for anyone (Lua's
  `EntityFlyerLand` and `EntityFlyerTakeOff`); `CalculateDest` lands an AI flyer at a destination flagged
  for it. `UnitFlyAgent::TakeoffNow` only shortens its wait.

### What was built

- **Calling up:** the nearest friendly AI flyers that may follow, from anywhere on the map. One further than
  150 m flies in at its stretched speeds and has 20 s plus 1 s per 20 m it started off (3 minutes at most) to
  reach its slot; meanwhile it is not let go for its distance. Losses are made up every second once the player
  has called wingmen (or from takeoff with `WingmanAuto`), whatever the order.
- **Orders:** form up holds the slots. Engage: each wingman takes the cheapest enemy within 800 m of the leader:
  its distance, plus 600 m for one on the ground, plus 400 m for each wingman already on it (troops on foot
  are left out); chased until 1000 m from the leader. Cover me: whoever's AttackHelper target is the leader,
  within 1200 m; chased until 1500 m. Attack my target: the lock, else an enemy under the reticule, as the key
  is pressed; with neither, "no target" and nothing changes; all wingmen on it until it is down or 3000 m from
  the leader, then back to the order it interrupted. A chasing wingman 2500 m from its leader comes back.
- **Chasing:** a flyer in the air as real flight's chase (lead pursuit, flat out to close, then at its speed);
  anything else in passes: a dive at it (`SH_TAPER`), out short of it or once past it, on and 60 m up, back
  in. The distances are each wingman's own (the user: "primary weapon values should be taken into account vs
  a blanket 150m pull out", then "I'd prefer it utilises maxRange more", then "Strafing should run into
  optimal range, pulling out just before minrange"), from its primary weapon's ranges (channel 0's weapon:
  `MinRange`, `OptimalRange`, `MaxRange`, the ranges its AI fights at; twice `OptimalRange` without a
  `MaxRange`) and its cruising speed: it comes back in from `MaxRange`, runs in through its optimal range, and
  pulls out just before its `MinRange` (10% outside it), never nearer than 2.5 s of its speed (a flyer turning
  90 degrees a second carries on 1.4 to 2.1 s of its speed toward the target as it pulls out; with no
  `MinRange` that room decides); behind a flyer it sits 7.5% of `MaxRange` back (50 to 200 m). Stock
  fighters' lasers (0/800/1600 m) at 60 m/s: back from 1600 m, out at 150 m, 120 m behind (simulated: 4
  passes in 2 minutes, 54 to 1638 m from the target, passing over it). A gunship's recoilless cannon
  (0/300/600 m) at 40 m/s: back from 600 m, out at 100 m, 50 m behind (7 passes, 34 to 625 m). Torpedoes
  (MinRange 200) pull out at 220 m. Its AI's attack
  is aimed at its target, at most every 0.5 s and only while aimed elsewhere, so it fires at it. A chasing
  wingman flies at its own speeds, copies no tricks and may pull its own.
- **Line of sight and the open side** (the user, 2026-10-14: "They don't seem to need line of sight before
  trying to kill it, meaning they'll shoot a hardpoint on the underside of a ship from the top"). Forcing
  the AttackHelper skips the AI's own choice of what it can see, and its fire follows its aim, hull or no
  hull; and a pass treated everything off the ground as on open ground (in from wherever, out 60 m up).
  Now a wingman fires only while a ray along its line to 10 m short of its target (the landing ray's types:
  `COLL_STATIC`, terrain, water, so capital ships but no vehicles) meets nothing, looked along every
  0.25 s; without it the AttackHelper's target is let go with `AttackHelper::Reset` (P `0x0040635C`
  thunk, M `0x005B4230`: no target, not visible, last seen at -1000, as `SetVisibleTarget` starts;
  `ForceSetAttackHelperTarget(null)` would crash, `SetVisibleTarget` going on to
  `AIUtil::GetHisVisionTargetPos`, which calls through the target), and a pass inside where it would turn
  back breaks off. A target off the ground whose ray down from 500 m up meets something first is shut in
  from above; of 14 directions (the axes, the corners) the rays in onto it from 500 m out that are clear,
  the nearest to the wingman, up a little first, is its open side (the rays look in from outside, so they
  meet the outside of what shuts it in; begun nearer they could begin inside a big ship). Off that side
  the wingman flies out to 400 m along it first: straight, else by the nearest of the 14 points 400 m out
  it reaches in a straight line that sees there, else 400 m further out across that side at its own
  height, looked at again every 2 s. It pulls out along the open side. Under open sky, passes are as they
  were. `flyer_orders_core.hpp`: `pick_open`, `on_open_side`, `pick_via`, `escape_point`,
  `extend_point_along`; `update_run` breaks a blocked run off.
- **Forming up near a hangar** (the user: "how do wingmen attempting to form up work when I'm in a hangar
  for example and they're still in open space?"): each flies straight at its slot beside the leader; the
  game's CalculateDest lifts its point over what is in the way (it only climbs) and avoidance steers round
  flyers only, so nothing routes a wingman through the hangar's mouth. Offered: a trail of the leader's
  path for wingmen whose line to their slot is blocked, and the formation closing up under a roof.
- **Keys:** the squad key calls wingmen up when there are none, else switches between form up and engage (from
  an attack or cover, form up). The comm keys Attack and Defend (F11 and F12 by default) order attack my target
  and cover me as each goes down, read from the player's processed inputs (its controller's +0x88, bits 26 and
  27); online their team chat still goes out. In the air only. The first build caught the comm keys' chat
  call instead, which single player never makes, so they did nothing offline (the user, 2026-10-14: "I've been
  pressing F11 and F12 with a wingman at my side, nothing").
- **Picking and breaking off** (the user, 2026-10-14: "direct recruitment by aiming at a flyer whilst you're
  in one and pressing F4, only if your squadron can accomodate it else do nothing", "a break-off command,
  which disbands the AI wingmen altogether", "hold F4. Works identically to ground unit follow that way",
  "After break-off, the next F4 press should be form up"). The trooper's squad key (`EntitySoldier::Update`,
  M from `0x00549851`, EBX its Controllable) acts on the press (the trigger's low two bits both set), and
  while bit 0 stays set adds the update's time to a hold timer (+0x7B4); past 1.0 (M `0x00A2A290`) it lets
  every follower go (`EVT_SC_StopFollowChr`), once a hold. The flyer's squad key does the same: on the
  press, a friendly AI flyer in the air under the reticule (`mReticuleTarget`, either channel) joins in
  the first free slot that takes its class if the flight has room and it may fly with this leader as a
  call-up would take it (else nothing happens); held a second, the flight breaks off. Then none joins on
  its own until the squad key calls a flight up again, in form up whatever its start order, or picks one;
  a flight picked from empty is not made up to its size (`flyer_orders_core.hpp`: `squad_hold`, `tops_up`,
  `pick_verdict`, `call_up_order`).
- **Markers** (the user: "Some sort of HUD indicator for wingmen units"): the game's own target markers.
  `TargetManager::AddMarker` (P `0x00773CA0`, M `0x0048E3A0`; cdecl(object, region, icon hash, scale, team
  mask, colour hash, showHudTarget, flashAlpha, pulseSize, pulse start)) fills one of 32 `Marker` slots
  (`gMarkers`, 40 bytes: the actor's handle, region, team mask, icon, scale, colour, showHudTarget, pulse
  start, the two flags); `RemoveMarker` (P `0x00774620`, M `0x0048E650`; cdecl(object, region, team mask))
  takes the team's bits off every marker on that object. The map draws them (`HUD::ElementMap::
  UpdateMarkerIcons`, P `0x00602980`: the player's team's, at the object's smoothed position, flashing or
  pulsing by the two flags); with showHudTarget the object is also a "common" HUD target
  (`LockOnManager::AddTargetCommon`, P `0x0063A190`, 32 of them), which `HUD::ElementTarget` brackets on
  screen in the marker's colour. `LockOnManager::Update` (P `0x0063D0B0`) picks the player's locks from its
  own nearest targets and only keeps the common ones' visibility, so a common target is not a lock. Colours
  by name from the table at M `0x00ACA978` (black, red, green, blue, cyan, magenta, yellow, orange; filled at
  start-up). Each of the player's wingmen gets a steady cyan `hud_objective_icon_circle` (scale 3, the
  comm keys' requests' icon) with its bracket, from joining to leaving. A marker whose object's handle moves
  on before it is removed is drawn no more but keeps its slot until the mission ends. The flight's attack
  target gets the same circle in red while the attack lasts (the user: "Maybe an icon for their current
  target when ordered to attack my target as well").
- **HUD markers** (the user: "If the hud entry isn't found, resort to the current implementation"):
  `render/hud_wingmen.cpp` publishes `player1.wingmanN.*` (N 1..8, slot order: position, onScreen,
  offScreen, direction, distance, health, disable), `player1.wingmen.count` and `player1.wingmen.target.*`,
  placed as the command post markers are ([HUDSystem.md](HUDSystem.md), "Wingmen markers"). Each tick the
  wingmen's `TargetManager` markers go while a `.hud` binds any wingman event and come back while none does;
  the target's red one likewise with its own events.
- **The leader's trail** (the user, on wingmen in open space while the player flies into a hangar: "Possibly?
  Not got a clue how that would be done reliably however", then "Yep, get them done"; `ai/flyer_trail_core.hpp`,
  `[AI] FlyerWingmenTrail=1`): the player's flyer's path is kept as a point every 8 m, the last 256 (about
  2 km), started again past a 100 m jump and where the leader takes off after landing. A wingman whose ray to
  its slot (or, landing, to its place) meets a building, prop, the ground or water (the landing's types 0x190,
  four times a second) takes the trail from its nearest point within 300 m in a clear line, steers at the
  furthest of the points 16, 8, 4 and 2 ahead it has a clear line to (else the next), at `formation_speed` on
  how far it is behind its place in a line, a spacing behind the leader for each wingman in front of it, and
  goes back to its slot after three clear looks in a row. A wingman that gets no further for 6 s while its place
  is ahead is left to its own AI for 10 s. Under a roof over the leader (`roofed`, the landing's ray up 40 m,
  twice a second) the slots close in to half their spacing at half a second's worth a second. Under a roof the
  landing places the wingman cannot see are still given when it can reach the leader along the trail.
- **Why the trail turns the lift off.** `EntityFlyer::CalculateDest` (P `0x00523CB0`, M `0x004F4630`) calls
  `EntityFlyer::RaiseHeight` (P `0x00528910`, M `0x004F40A0` through the thunk `0x00409827`) for safe heights
  `SH_FULL` (0) and `SH_TAPER` (1) whatever happens, and for any other only when the low level has a path and
  its flag 2. RaiseHeight samples the heights round the flyer and casts a ray ahead from under it, 1.5 times its
  set speed long, at types 0x18 (COLL_RIGID and COLL_STATIC: vehicles and buildings); on a hit it takes the top
  of that object's collision sphere, and then lifts the point toward 25 m over the highest (`fadd [0xa36708]`),
  and the speed to 20 m/s. For a hangar that sphere is the whole capital ship's, so a wingman flying the
  leader's path inside would be sent up into the roof. With `SH_TAPER_FACTOR` (3) CalculateDest hands it
  `mTaperFactor` (+0x2C) and the taper flag (M `0x004F486A`), and RaiseHeight makes the point's y
  `(lift - y) * factor + y` (M `0x004F446A`, `0x004F44E8`): a factor of 0 lifts nothing. The SafeHeight enum
  (P): `SH_FULL` 0, `SH_TAPER` 1, `SH_NONE` 2, `SH_TAPER_FACTOR` 3. On the trail a wingman's `mDest` asks for
  3 with a factor of 0; it still slows for what is ahead, and `AIUtil::AvoidBarriers` still runs.
- **The first hangar landings (2026-10-15).** The user: "I just don't see any late arriving wingmen ... they
  seem to panic in tight spaces". The log: the trail never gave up on one (no "no way along your trail"); of
  the eight times a wingman took it, five left it with their way clear, one crashed on it and two were still
  on it when the player got out. Three died in the hangars. The rest were let go: as the player got out (the
  flight went with it), and in each landing one with no clear place under the roof. `BF2GameExt_crash.log`
  had two faults in `matrix_of` from `start_landing`, reading through `0xDDDDDDDD` (modtools' fill for freed
  memory): a wingman shot down within the last second, its flyer already freed. The update's guard swallowed
  them and left those landings half set up (no summary line, a wingman given nothing). Built:
  - **The lift off under a roof too** (`flyer_trail_core.hpp`, `lift_off`): a wingman under a roof itself
    (the roof ray from it as from the leader, twice a second), or with its leader under one and its line clear
    as last looked (within 1 s), flies at its slot, its place or its target with `SH_TAPER_FACTOR` and no
    factor, as on the trail. With a wall in its way and off the trail it keeps the stock lift.
  - **Landing without the player** (`landing_without_you`; `flyer_orders_core.hpp`, `arrival_over`): the
    player out of its landed flyer, the flight goes on landing for 60 s with the empty flyer as its leader (the
    side checked against is the one it came down on, not the empty flyer's); back aboard, the player leads it
    again; then, or with the flyer gone, flown off or another taken, the rest are let go.
  - **The shot down go first** (`drop_gone`), each tick before the landing asks any wingman where it is; and
    every guard in `flyer_wingmen.cpp` logs what it caught (`caught`: the 1st, 10th, 100th ... time for each).
  - **On ahead, then circling till a place is free** (the user: "Instead of a circle, they should move
    forward instead. If forward is not clear (wall) then circle until a free spot appears"): past its five
    places beside the leader a wingman tries 16 on ahead, the way the leader faces (`forward_tries`: 12 m to
    its side, every 12 m from 15 m ahead), none past a wall on that line; every place is also checked for a
    flyer or vehicle on it (`occupied`: five rays down at vehicles alone, COLL_RIGID, which the ground's rays
    pass through). With none, it circles and looks again every 2 s (`look_again`): overhead in the open, low
    and slow under a roof (`roof_circle`: at its hover height, 15 to 30 m round the point with most room, eight
    rays at static, terrain, water and vehicles, of the one over the leader and those on ahead before a wall;
    no room, let go). Under a roof one interrupted past its two retries is let go, not sent round again.
  - **The way out** (the user: "can it fly back out the way it flew in, probably by [following] my path
    backwards? Lots of hangars are single entry/exit, so if they disband it's highly likely they'll crash
    unless they do this"): one let go under a roof while flying (no place and no room to circle, its
    landings failing, or the 60 s with the player out over) flies the trail backwards first (`head_out`,
    `follow_trail_out`; `flyer_trail_core.hpp`: `nearest_back` takes the oldest of points as near, the way
    in, and never goes forward again; `back_candidates` cuts back 16, 8, 4 or 2 points where it has a clear
    line), at its slowest and not lifted, and is let go once its own roof ray has found nothing over it two
    looks in a row (`out_clear`); with no trail point it can take, at once; not on the trail within 2 s,
    stuck 6 s or still going at 45 s, where it is.
  - Left for the user to decide: avoidance between flight mates closed up under a roof (it keeps their two
    radii plus 8 m between them, side by side too, so it pushes them off their slots toward the walls). It
    steers a flyer only in normal flight (`EntityFlyer::UpdateIndirect` M `0x004F4C10` calls `CalculateDest`
    only in state 2, flying; not taking off, state 1, nor in the game's own landing, 3), and counts as
    obstacles only flyers taking off, flying or landing (not those down, state 0).
- **Feedback:** "Wingmen: form up", "engage", "attack my target", "cover me", "no target", "no one to call up",
  "target down", "target lost" on the message line in the trooper commands' colour; a map or mod may word them
  under `game.wingmen.formup` ... `.targetlost`.
- **Landing with the player** (a try; the leader class's `WingmanLand`, else the INI's `FlyerWingmenLand`, on by
  default; with 0 the wingmen break off as the player lands): a player can land only inside a landing region
  (Lua `AddLandingRegion`, `TriggerRegionManager` TRIGGER_LANDING; the flyer's land key and speeds check
  `mInLandingRegionFactor`, M +0x5FC), but `EntityFlyer::Land` needs none. When the player's flyer lands, each
  wingman, in slot order, is given a place on the ground beside it: of its slot's place (1.5 times out and at
  least 15 m from the leader), the same way at the slot's own distance and at 0.6 of it (never nearer than
  10 m), the other side, and behind the leader (15 m, 12 m more for each later wingman), the first 12 m from
  the places already taken where a ray down finds ground within 8 m of the leader's and flat (its up at least
  0.8). The ray starts 40 m up, or 2 m under a roof: a ray up from the leader's ground that meets something
  within 40 m finds one (a hangar's, which a ray from 40 m up would meet before the ground). Both rays take
  the flyer's own landing ray's types, 0x190: `COLL_STATIC` (0x10), `COLL_TERRAIN` (0x80) and `COLL_WATER`
  (0x100) of `CollisionObjectType` (P), each object's own type its `TreeGridObject::mData` (from the
  `GameObject` constructor: `EntityBuilding` and `EntityProp` pass `COLL_STATIC`, `EntityFlyer` `COLL_RIGID`
  8, `EntitySoldier` `COLL_SOFT` 2; P `0x0050262D`, `0x0055FB9D`, `0x00520F8E`, `0x00565FB0`), so buildings
  and props count and the wingmen overhead do not; `RayHit` with no types takes 0xBE. Under a roof a
  place must also be in a clear line from the leader 3 m over its ground, and the wingman's way to it clear.
  It comes down toward 15 m over its place (under a low roof 4 m under it, never under 4 m), at its slowest
  from 300 m in (its stretch lets it fly at 10 m/s), and lands once within 25 m across and 40 m up. Once down
  its pilot gets out and it is let go (the user: "Make getting out on land the default"), unless its own class
  says `WingmanExitOnLand = 0`: then it waits aboard, and when the player takes off it takes off too and forms
  up again. One with no place, or up again first, circles 200 m round a point 150 m over the leader; with the
  leader under a roof, where that would take it into the roof (or the ship or the mountain round the hangar),
  it is let go instead (`GetWingmanLostReason` "noroom").
- **Landing right away, and again elsewhere when interrupted** (the user, 2026-10-14: "When I land, AI should
  land right away. If their land is interrupted by landing on top of something, that's when they re-check and
  try to find a clear spot"). As the leader comes down, each wingman within 150 m of it lands where it is
  (`EntityFlyer::Land`) where a ray down finds ground within 200 m, level with the leader's and flat, and a
  second ray down (`COLL_RIGID` 0x8, the flyers and vehicles, which the landing ray's 0x190 goes through)
  meets no flyer or vehicle above that ground; the rest are given places as before. Every second while it is
  told to land: one coming down onto a flyer or vehicle, still not down 8 s after `Land`, or down on top of
  one (a flyer or vehicle nearer under it than the ground) takes off again (`EntityFlyer::TakeOff`) and, up,
  is given a clear place beside its leader away from the others' places and from where they are, twice at
  most; then it circles (under a roof, it is let go). `flyer_orders_core.hpp`: `land_here`,
  `landing_interrupted`.
- **The formation key** (the user: "We'll utilise an unused Fnum key whilst in a flyer, so anything from F5 to
  F10. F5 is best I think"): the comm key Spotted, `ePROCESSEDINPUT_Comm_Chat_0` (input 20, F5 by default in
  every control mode; flyer mode leaves only "previous primary" and "previous secondary weapon" unbound,
  `RawControllerInputs::sDefaultKeyboardBindings`, P initialiser `0x0043B1E0`), read from the player's processed
  inputs as Attack and Defend are. Each press: the flight's next formation of the vee, its leader class's own
  WingmanSlot layout, and each of the map's formation shapes that holds it (as many slots as it has wingmen
  or calls up), round, in the map's order; wingmen whose slot the new one lacks move to free ones. "Wingmen
  formation: <name>" on the message line (`game.wingmen.formation`).
- **Getting out, for sure** (the user, 2026-10-14: "AI don't always leave their ship when they land it as
  wingmen"). In the user's log one wingman touched down and the player got out a second later, before the
  once-a-second landing update had seen it landed: the flight was let go ("the player is not flying") with
  its pilot still aboard. And `SetExitVehicle` was asked once only. Now a landed wingman still aboard is
  asked again every 2 s, four times in all, and one landing or landed when the leader's pilot gets out (or
  the leader changes) keeps being asked for 20 s, its fly agent kept from taking it up meanwhile. One still
  coming down 10 s after `EntityFlyer::Land` is logged with its distance from its spot (one landed 25 m from
  it, as far as the landing radius lets it, on ground nobody checked).
- **Installing alongside other fixes:** `EntityFlyer::Land` may be detoured first by the LandOnArrival fix and
  `EntityFlyer::TakeOff` by the carrier fixes (`flyer_carrier_fixes.cpp`, from `lua_hooks_install`, before
  `flyer_wingmen_install`), each only adding to it, so a JMP at either is taken as theirs. The first build
  took the carrier fixes' JMP at `TakeOff` for a different exe and left landing off (the log: "landing with you
  NOT installed: EntityFlyer's Land or TakeOff differ"): the user's wingmen followed them into a hangar and
  were let go as they landed ("3 let go: the player is not flying"), and the game's own AI flew them round it
  and into a wall ("They circled then crashed into a wall, even though the ground was clear beneath them").
  `tests/flyer_wingmen_abi_tests.py` now finds every function another feature's `DetourAttach` takes and
  fails if a guard on its first bytes would not take the JMP (`Land`, `TakeOff`, `DoTrick`, the camera
  shake's).
- **Getting out:** stock, an AI pilot would get out only through its fly agent's own landing:
  `UnitFlyAgent`'s FLY state (4) lands at an AI "land" hint node within 80 m (`IsNearLandingPt`, P
  `0x00790AC0`, M `0x005AED00`, every 3 s; `HintManager::RequestLandHints`, P `0x005EC510`: hint type 0x20,
  each tied to a command post, one its team cannot spawn at while the agent's `mGotoNonOwnedCP` is set, as it
  starts, else one it can; not the players' landing regions), its LAND state (5) waits for the flyer to be
  down, then DROPOFF (6) calls `SetExitVehicle` with that post. None of it ever runs: a hint's command post
  is stubbed out (`BaseHint::GetCommandPost` always returns none), so no land hint qualifies
  (docs/RE/HintNodes.md) (`AILowLevel::SetExitVehicle`, M `0x005A2170`: the
  exit flag 8 at +0x74, the post at +0x78). `UnitController::UpdateHighLevel` also gets AI out at the end of
  a game; `UnitFlyAgent` swallows damage, so no pilot bails out. For a wingman the agent's one CALL to
  `IsNearLandingPt` (M `0x005AFDD3`) answers no (it lands only with its leader), and its two CALLs to
  `TakeOff` (TAKEOFF and FLY states, M `0x005AF154`, `0x005AF323`) do nothing while it is down beside its
  leader. Landed there, its pilot is got out with `SetExitVehicle` and no post, as the end-of-game exit does
  (M `0x005A079E`). The squad command `EVT_SC_GetOutOfVehicle` (20) would do the same through the unit's
  command queue, and keep it out of vehicles for 20 s (`UnitAgent::HandleSquadCommandEvent`, P
  `0x007844D0`).
- **Checks:** `tests/flyer_wingmen_tests.cpp` (orders, the passes' shapes and the passes flown, landing and
  the places tried, the heights under a roof and the circle overhead, simulated);
  `tests/flyer_wingmen_abi_tests.py` (159 modtools checks, every guard's bytes, the guards on functions other
  features detour); the mutation suite's `orders` (all caught).

### What to watch in play

The log names each step: `[Wingmen] slot N: #a called up X m off, Y s to reach its slot`, `in its slot after
N s`, `order: engage, 3 wingmen`, `comm key: attack my target, #t (locked, in the air), X m off`, `slot N (#a)
takes on #t`, `back to its slot after N s: why`, `the attack's target is down: back to ...`, and every 5 s
`[Wingmen] engage: 3 wingmen, 1 in their slots, 0 flying in, 2 chasing`; each chase names the weapon ranges
it was shaped from (`its weapon's ranges 0/800/1600 m: pulls out at 200 m, back in from 800 m, 120 m behind a
flyer`); the squad key: `squad key on #t: it joins your flight (n)`, `... your flight has no room for it`,
`squad key held: break off, n let go`, `markers installed` at start-up; landing: `landing with you installed` at start-up, then `the leader comes down [under a roof]: N land
beside it, M circle overhead, K let go (nowhere to land under the roof)`, `slot N (#a) lands beside you X m off
(place k of 5)`, `slot N (#a) lands X m from its spot`, `landed: its pilot gets out`, `the leader takes off: N
of its wingmen take off with it`; the trail: `slot N (#a): a wall between it and its slot: it flies your trail,
from X m off`, `its way clear again, off your trail`, `no way along your trail for 6 s: its own AI flies it for
10 s`, `you fly under a roof: your wingmen's slots close up`; getting out: `you got out: your N wingmen land
beside your flyer all the same, 60 s at most`, `you are back aboard`, `your flight is down and out, with you out
of your flyer`, `N let go: 60 s after you got out`; and any fault a guard caught: `a fault in <where> was caught
(n so far): that step was skipped`. Unknowns: whether a forced attack target makes a wingman fire (the AI's own
combat may take it back between settings), how the passes fare against terrain, how an AI flyer lands when
told to at speed, and whether a hangar's roof stops the ray from 40 m up (a roof is looked for whatever it is
made of; the ray may pass through the back of a face).

## Wingmen from a mission script, and more ODF keys (prototype, 2026-10-14, modtools only)

Asked for in the brainstorm the user agreed to start on ("Sounds good to me"): the keys `Wingman = 0`,
`WingmanRole`, `WingmanStartOrder` and `WingmanEngageRange`, and Lua to call up, name, order and lock the
flight, with events. The user-facing tables are in docs/user/ODF_PROPERTIES.md and docs/user/LUA_API.md
("Flyer Wingmen"); the code is `ai/flyer_wingmen.cpp` (the flight) and `lua/lua_wingmen.cpp` (the Lua side).

- **The keys.** `Wingman = 0` (on the wingman's class) keeps a class out of every call-up, the player's and
  GameExt's AI squadrons' (not the game's own spline squadrons, which GameExt does not touch); a script
  naming it overrides it. `WingmanRole = bomber` turns the foe cost round: kGroundCost goes on flyers in the
  air instead of targets on the ground. `WingmanStartOrder` (formup, engage, cover; never attack, which
  needs a target) is the order a flight starts with at takeoff and on the squad key's call-up.
  `WingmanEngageRange` replaces the 800 m (100 to 5000), the chase keeping the same quarter on top (1000 m
  for 800). None of the four hashes appears anywhere in the modtools or Steam exe, so no stock key is
  shadowed. Words are matched with case, quotes, spaces, `_` and `-` ignored (`flyer_orders::word_is`).
- **Names.** A world object's name goes through the game's own table: `_GetEntity<GameObject>` (P
  `0x006568E0`, M `0x00470FF0`, an LTCG-style register call on modtools) takes a light userdata as the
  object itself, else `PblHashTableCode::_Find(EntityEx::mIdMap, 0x800, PblHash(name))` (M `0x007E1A40`,
  table M `0x00B7AD3C`; instance_props.cpp already uses both), then checks `IsRtti(GameObject)`. A light
  userdata from `GetCharacterVehicle` is the vehicle's `Trackable` (its Controllable +0x18, vptr +0x20), the
  same object `object_of` gives, so pointers from either side match. A vehicle spawner's flyer carries the
  spawner's name, so a name keeps naming the flyer that spawner sends out next: that is the roster's point.
- **The roster.** Up to 8 entries, by name (resolved each tick) or by flyer and handle (dropped once the handle
  no longer matches). Entries join before any call-up on their own, into the first free slot that takes their
  class (else any), from anywhere, whatever the class keys say; one leading an AI squadron dissolves it. A
  removed flyer's handle is kept out of later call-ups (32 kept). The flight's capacity is its map shape's
  slots (now all of them, not just as many as are called up), its WingmanSlot lines, or 8.
- **What a script sets** holds for the mission: `CallWingmen` turns call-ups at takeoff on (over
  WingmanAuto) and can set how many come on their own; `DismissWingmen` turns them off and empties the
  roster; `SetWingmenLocked` stops call-ups on their own (the squad key's too) and every let-go for distance or
  lateness; `SetWingmenOrder` sets the standing order (over WingmanStartOrder) or attacks a target (anything
  not friendly by `GetAffiliationToMe`); `SetWingmenFormation` takes a map shape or the vee over everything
  else and re-slots the flight; `SetWingmenLand` overrides WingmanLand and the INI. A script's orders put no
  line on the HUD (`Formation::quiet`, also for its attack's end).
- **Events** are four rows in lua_events.cpp's table, of the `Character, GameObject` family, so the engine
  makes `On...`, `On...Name/Team/Class` and `Release...` and stubs them on network clients. The pilot is the
  character whose `mVehicle` is the flyer's own Controllable (+0x240), found when it joins and kept, so a lost
  wingman still names its pilot. They are queued and fired after the original `EntityFlyer::UpdateIndirect`
  returns, or at the end of the script's call, never halfway through changing a flight; a callback calling
  back in only queues (the flush loop takes it). `GetWingmanLostReason` reads the reason of the event being
  fired.
- **Mission end.** `EventManager::Cleanup` (the GameLoop's, per mission) now also calls
  `flyer_wingmen_mission_end`: the flights, the roster and everything a script set are dropped there, and
  nothing more fires into the Lua state on its way out. Before, formations kept pointers into the old level
  until the next tick found them gone.
- **Checks:** `tests/flyer_wingmen_tests.cpp` (`mission_keys`: words, orders by name, the start order, the
  engage range and its clamp, the roles' foe choice, Wingman = 0, the script's flight size);
  `tests/flyer_wingmen_abi_tests.py` (the name lookup's push sequence and `_Find`'s guard, the keys' hashes
  absent from the exe, the event rows); the mutation suites `orders` and `wingmen`. Nothing of it played.
