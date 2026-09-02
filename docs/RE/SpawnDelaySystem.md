# Spawn cycle and the multiplayer respawn delay

Why the respawn wait is 15 seconds in every online match, what actually drives
the countdown, and which machine owns it. Addresses are Phantom unless the table
says otherwise; the modtools/Steam/GOG sites are listed where they differ.

---

## SpawnManager

One global, `SpawnManager::sInstance`, 0x100 bytes. Everything about spawn timing
lives in per-team arrays of 8:

| Offset | Field | Meaning |
|--------|-------|---------|
| `+0x18` | `int mMaxUnitCount[8]` | team size cap |
| `+0x38` | `int mCurrentUnitCount[8]` | live count |
| `+0x58` | `int mAllyUnitCount` | |
| `+0x5c` | `float mCycleDelay[8]` | **seconds between spawn waves** |
| `+0x7c` | `float mSlotDelay[8]` | the "variance" argument |
| `+0x9c` | `float mCycleTimer[8]` | countdown to the next wave |
| `+0xbc` | `int mCycleIndex[8]` | wave counter |
| `+0xdc` | `float mSlotTimer[8]` | |
| `+0xfc` | `bool mHistorical` | |

BF2 does not give each corpse its own timer. It runs a per-team wave clock, and a
dead player is assigned the next wave:

```c
// Character::Respawn, tail
this->mSpawnCycle = SpawnManager::sInstance->mCycleIndex[this->mTeamNumber] + 1;
```

```c
// SpawnManager::Update (0x0076EE30), per team 0..7
mCycleTimer[team] -= dt;
if (mCycleTimer[team] <= 0.0f) {
    mCycleTimer[team] = NetGame::IsPreGame() ? (mCycleTimer[team] + 0.5f)
                                             : (mCycleTimer[team] + mCycleDelay[team]);
    mCycleIndex[team]++;
    mSlotTimer[team] = mSlotDelay[team];
}
mSlotTimer[team] -= dt;
```

> **`mCycleDelay` is re-read at every rollover**, not latched at init. Writing it
> mid-match takes effect on the next wave, with no reload and no re-init call.

The constructor seeds it: teams 1 and 2 get `mCycleDelay = 15.0f`,
`mSlotDelay = 0.5f`; every other team gets 0/0. So 15 seconds is the engine's
baked-in default, and a map that never calls `SetSpawnDelay` keeps it everywhere,
online or off.

---

## The multiplayer override

`Lua_Callbacks::SetSpawnDelay` reads both arguments and then discards the first
one whenever networking is on:

```c
delay    = luaL_checknumber(mState, 1);
variance = luaL_checknumber(mState, 2);
if (netInShell ? netEnabledNext : netEnabled)
    delay = 15.0f;                                   // <== the script's value dies
for (team = 1; team < 3; ++team)
    SpawnManager::SetSpawnDelay(sInstance, delay, variance, team);
```

`SetSpawnDelayTeam` carries the same clamp but applies it only when the team
argument is 1 or 2, so **teams 3 and up already pass their scripted value through
untouched**. The **variance is never clamped** in either function.

That is the whole reason a host cannot change online respawn timing: the value is
overwritten between the script and the field, so nothing a map author writes ever
reaches `mCycleDelay` in a network game.

| Symbol | modtools | Steam | GOG |
|--------|----------|-------|-----|
| `Lua_Callbacks::SetSpawnDelay` | `0x0046C370` | `0x0058C610` | `0x0058D5C0` |
| `Lua_Callbacks::SetSpawnDelayTeam` | `0x0046C410` | `0x0058C690` | `0x0058D640` |
| `SpawnManager::SetSpawnDelay` | `0x0065F680` | `0x0064E290` | `0x0064F330` |
| `SpawnManager::sInstance` | `0x00B9A3DC` | `0x01EAFB34` | `0x01EB0FE8` |

modtools carries the `15.0f` as an immediate (`MOV [ESP],0x41700000`); the retail
builds load it from a shared `.rdata` literal with about 50 xrefs, so the constant
itself cannot be edited in place on retail.

---

## Who owns the number in a network game

Three separate facts, and together they say the host is the only machine that
matters:

1. **Only the host spawns.** `Character::Update` skips `Spawn()` entirely under
   `netOnClient` (see [[mp-scripting-constraints]]), so the wave clock that
   actually produces a body is the host's.
2. **The client's countdown comes off the wire.** `SpawnDisplay::Read` is two
   fields and nothing else:

   ```c
   NetPktGroup::ReadBits(pkt, &this->mBaseTurn,      0x20);
   NetPktGroup::ReadBits(pkt, &this->mBaseCycleTime, 0x20);
   ```

   and `SpawnDisplay::UpdateTimer` splits cleanly on `netOnClient`: the host
   computes the number from `mCycleDelay`/`mCycleTimer`/`mCycleIndex` and publishes
   it, the client extrapolates the received `mBaseCycleTime` by elapsed turns
   (`* netSecondsPerTurn`) and never touches `mCycleDelay` at all.
3. **A client's own copy is a local mirror, not a transfer.**
   `SpawnManager::InitialUpdate` copies `mMaxUnitCount`/`mCycleDelay`/`mSlotDelay`
   from `sHostInstance` under `netOnClient && !netIsOneWorld`, but `sHostInstance`
   is the locally-created host-simulation manager (`SpawnManager::CreateClient`),
   seeded by the same constructor and the same mission script. It is not fed from
   the network, and nothing downstream of it drives the client's own display.

So a change made on the host propagates to every client with no client-side patch.
The clients' local `mCycleDelay` stays at 15 and is simply never consulted.

---

## Applied in BF2GameExt

`util/mp_spawn_delay.cpp`, `[Features] MPSpawnDelay` (seconds, clamped to
[0.1, 300], defaulting to the engine's own 15), all three builds. It substitutes the configured value for the `15.0f` that
the two Lua callbacks force in, so the host's number lands in `mCycleDelay[1]` and
`mCycleDelay[2]` the moment the mission script calls `SetSpawnDelay`. Only the
branch the engine already gates on `netEnabled` is touched, so singleplayer keeps
whatever the script asked for.

How the constant reaches the delay slot differs by build, so the write does too:

| Build | Instruction | Site | Patched |
|-------|-------------|------|---------|
| modtools | `C7 04 24 <imm32>` `MOV [ESP],15.0f` | `0x0046C3B0` / `0x0046C470` | imm32 rewritten in place |
| Steam | `F3 0F 10 0D <disp32>` `MOVSS XMM1,[0x007B22B0]` | `0x0058C655` / `0x0058C6F4` | disp32 repointed at a DLL float |
| GOG | same, literal at `0x007B3228` | `0x0058D605` / `0x0058D6A4` | disp32 repointed at a DLL float |

modtools can be edited in place because it carries the constant as an immediate.
The retail builds load it from a shared `.rdata` literal with about 50 unrelated
xrefs, so editing the literal would move every one of them; the instruction's
operand is repointed at a float this DLL owns instead. Both sites are verified
(opcode bytes plus the operand) before either is written, and one mismatch
disables the feature - half of it applied would leave `SetSpawnDelay` and
`SetSpawnDelayTeam` disagreeing about the same team.

A request of exactly 15 writes nothing at all: it is what the engine already
does, so a default install leaves these bytes untouched. Values below 0.1 would
give `mCycleTimer` a step it can never expire on, and are clamped rather than
refused. Fractions survive the whole way to `mCycleDelay`, which is a float.

**Limits, both by design.** A map that never calls `SetSpawnDelay` at all keeps
the constructor's 15, because the constructor is shared with singleplayer and is
deliberately left alone. And the value is read when the script calls the callback,
so it cannot be changed part-way through a match.

### What a mid-match change would take

`mCycleDelay[team]` is re-read at every wave rollover (see `SpawnManager::Update`
above), so the field itself supports being written live - the missing piece is
only something to write it. Two ways in:

- **Write the field directly.** `sInstance + 0x5c + team*4`, from anything that
  ticks. This is the real hook point; the call-site patch above is a convenience
  on top of it, not a prerequisite.
- **Read it from somewhere external.** SWBF2Admin caves the function to pull the
  value out of an unused environment variable, which is the right shape for a tool
  that lives in another process and has no injected code of its own. From inside
  the process the env round-trip buys nothing over writing the float directly.

Either way, a cave placed on `SetSpawnDelay` alone still only re-evaluates when
the script calls it; genuinely mid-match means reaching the rollover in
`SpawnManager::Update`.

> **Interop.** SWBF2Admin patches this same area. Two patchers writing the same
> site is the one way this breaks, so if both are ever expected on one server the
> exact bytes each one owns need comparing first. `util/shader_patch_detect.hpp`
> is the existing precedent for standing down when another patcher is present.

Related: [[mp-scripting-constraints]], [[unit-count-limits]], [[ia-bot-count-system]].
