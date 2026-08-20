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
