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
