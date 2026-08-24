# Roadmap

Planned and in-progress work. This is the backlog, not a feature list: nothing here
is shipped. For what the DLL actually does today, see
[docs/user/FEATURES.md](docs/user/FEATURES.md).

## Bugs

**Upper-body animation lookup has NO upper bound and only a NULL guard on the result** -
found 2026-08-21 from a user crash while rolling. Access violation at
`SoldierAnimator::UpdateUpperBodyAnimation` +0xD (modtools `0x0057897D`, `MOV EAX,[EBX]`)
with `param_2` = `0xF4F2D887`, i.e. a garbage non-NULL pointer.

The producer is `SoldierAnimatorClass::GetUpperBodyAnimation` (modtools `0x0057DD40`), which is
a raw table read with no rejection path at all:

    EDX = map * 0x12E
    EAX = idx * 2                       ; idx is a uchar, so 0..255
    EDX += EAX
    if (EAX < 0x10C) return [ECX + EDX*4 + 0x24];   ; idx <  134
    else             return [ECX + EDX*4 - 0x6C];   ; idx >= 134

Both branches return something. Nothing compares `idx` against a populated count, so an index
that was never filled in yields whatever dword happens to sit at that offset.

The consumer, `SoldierAnimator::UpdateActionAnimation` (`0x0057AFD0`), guards only against NULL:

    0057B127  MOV EAX,[ESI+0x2014]      ; anim kind
    0057B12D  CMP EAX,0x7
    0057B130  JNZ 0057B154
    0057B134  MOV AL,[ESI+0x2018]       ; anim index byte
    0057B13A  CMP AL,0xA4               ; the "no animation" sentinel
    0057B13C  JZ  0057B1DF              ; ...skip
    0057B14D  CALL GetUpperBodyAnimation
    0057B16F  MOV EDI,EAX
    0057B171  TEST EDI,EDI
    0057B173  JZ  0057B1DF              ; NULL-ONLY guard
    0057B18E  PUSH EDI
    0057B19B  CALL UpdateUpperBodyAnimation   ; derefs immediately

So a non-NULL garbage slot reaches the deref unchecked. **Our `Combo Anims Increase` patch set
moves that sentinel from `0xA4` (164) to `0xFE` (254) at three sites inside this very function**
(file offsets `0x17b02c+1`, `0x17b13a+1`, `0x17b1ca+6`), so index 164 stops being rejected here
and becomes a live table index. That is the leading suspect and the A/B test is one INI line:
`[LimitIncreases] ComboAnimIncrease=0`.

Second, weaker suspect on the same path: `soldier_prone.cpp`'s `hooked_SetAction` forces
`mAction` to `CROUCH_TO_PRONE`/`PRONE_TO_STAND`/`PRONE_TO_CROUCH` (27/28/29), values vanilla
`SetAction` never writes. A class with no prone animations authored could resolve those to an
unpopulated index. Gated on `g_proneEnabled`, so it is separately testable.

NOT the cause: a zero roll energy cost. It removes the roll lockout entirely (see
`docs/RE/EnergyBar.md`) so the path runs far more often, but it is an exposure multiplier, not
a memory-safety mechanism.

**Tentacle fields are unclamped and overrun fixed arrays** - found 2026-08-21 while scoping the
9-tentacle feature; independent of it and worth fixing on its own. Both tentacle properties on
`EntitySoldierClass` are bitfields at `+0x8BC` that accept more than the arrays can hold, and
NEITHER is clamped on any build:

- `NumTentacles`, 3 bits (mask `0x380`, extracted `SHR 7 / AND 7`) accepts **0-7**, but `tPos`
  and `oldPos` are dimensioned for **4**. Values 5-7 pass straight through and `DoTentacles`
  writes past the arrays.
- `BonesPerTentacle`, 4 bits (mask `0x3C00`) accepts **0-15**, but the per-frame bone array is
  a fixed 4x5 on the STACK. Values >= 6 overrun it; on Steam `0x006558F0` it first destroys the
  spill locals holding the live row pointer and loop counters, so the corruption compounds, then
  reaches saved `EBP` and the return address. On modtools `0x0056F4E0` the array is at
  `[ESP+0x20..0x70)` and `[ESP+0x6C]` is the saved return address.

modtools at least WARNS (`"Too many tentacles!"` `0x00A40FE8` via `0x00541CD3 CMP EAX,4 / JBE`,
`"Too many bones per tentacle!"` `0x00A412E4` via `0x0053FBF5 CMP EAX,5`) - but the `JBE` only
skips the warning, it never clamps. **Retail compiled both checks out entirely**, strings and
all, so a mod hits this with zero diagnostic.

No stock ODF exceeds either limit, so it is latent - but it is a stack smash reachable from a
plain ODF typo. A clamp at the store site is cheap and length-neutral (edit the `AND` mask) and
should be done regardless of whether the 9-tentacle feature ever happens.

**Unbounded hash probe - the best candidate for the GUI freeze** - `PblHashTableCode::_Find` is
an open-addressing probe that walks backwards with wraparound and has NO iteration cap. Its only
two exits are "key matches" and "slot is zero", so a table that is 100% full plus a lookup for an
absent key spins forever on the main thread - GUI frozen, audio still playing, which is the
reported signature. Verified disassembly, modtools `0x007E1A40` (loop `0x007E1A62`-`0x007E1A75`,
`JNZ` back with no counter); Steam `0x00726E00` (loop `0x00726E28`), GOG `0x00727ED0` (loop
`0x00727EF8`), Phantom `0x00864330`, `_Store` same shape at `0x008644D0`.

Reachable from `AttachedEffectsClass::SetProperty`, which calls
`_Find(FLEffect::s_EffectClasses._uiTable, 0x200, effectNameHash)` - 256 key slots. `FLEffect::Read`
registers through `_Store` and tracks `_iNumEntries` but NEVER compares it to capacity; there is no
"too many effect classes" guard anywhere in the image. So a mod with 256+ distinct effect classes
fills the table, and the next lookup of a name that is not in it - exactly what a missing or
typo'd `AttachEffect` line produces - hangs during level load. Callers: modtools `0x004C2942` /
`0x004C2AFD`, Steam `0x004477DF` / `0x004478E7`, GOG `0x004477BF` / `0x004478C7`. Tables: modtools
`0x00CF55B4`, Steam `0x01EBD144`, GOG `0x01EBE5F4`.

**NOT YET LINKED TO THE OBSERVED FREEZE.** The loop is byte-confirmed to exist and to be
uncapped, but nothing yet proves it is what the user hit - its trigger is a full effect-class
table, which is a separate condition from the 64-entry attachment overflow fixed in
`[Fixes] AttachedEffectsOverflowFix`. Next step is to instrument `_iNumEntries` against 256 at
level load rather than to patch anything. A cap would need a probe counter, which means a hook
rather than a byte patch, and refusing a lookup changes behaviour for every hash table in the
game - `_Find` is generic.

**LODs break under freecam** - Models pop to the wrong detail level, or drop out entirely,
while the camera is detached. The LOD selection almost certainly scores against the player
entity or the game camera rather than the active render camera, so once freecam moves away
from the player everything is graded at the wrong distance. Needs the actual LOD distance
source traced before a fix is designed.

## Vehicles

**9-pose vehicle aiming for AI** - The 9 aim poses are driven directly by the player's
mouse / joystick deflection, so an AI driven vehicle never advances past the initial frame
and sits locked in the neutral pose. Needs the pose selection to be fed from the vehicle's
actual aim delta (turret / aimer angle) rather than raw player input, so AI and players
drive the same path.

**Walker stomp attack cleanup** - The stomp / attack system is fully wired at runtime but
rough around the edges:

- Add dedicated `AttackEffect` and `AttackSound` ODF properties instead of the current
  hardcoded FX and sound.
- Revisit `AttackControls`. Values 1 and 2 are documented as primary / secondary fire but
  do not actually restrict to those, so the real semantics need pinning down.
- Write proper user documentation for enabling it. It is currently gated on
  `HealthType = "animal"` plus an `AttackAnimation` ODF entry.

**Flyer strafe mode ODF property** - The engine has an abandoned strafing path for flyers.
Goal is an opt-in `EntityFlyer` ODF property that turns the turn axis into lateral movement,
which means giving up rolling on any class that enables it, since the same input drives both.
A prototype exists on an older commit and was
player-only for exactly this reason: AI keeps rolling so navigation is not broken. The open
question before this can ship is what AI actually does when a class it flies is in strafe
mode, and whether the AI flight path can be fed strafing at all rather than simply left on
the old behaviour. Needs a play test either way, since the visual lean scaling and sign were
never confirmed in game.

**ControlsUnit passenger weapons** - Lets a passenger use their own weapon from inside a
vehicle. Very unlikely to happen: the engine stub is half baked, and it needs a lot more
than patches. The PassengerSlot entity builds no weapon and no aimer (so there is no
reticle), `UpdatePilotAnimation` passes hardcoded -1 aim arguments (so the body never
aligns with where the passenger is looking), and `GetMatrix` recomputes the raw mount
matrix against the vehicle every frame with no smoothing (so the camera and aim are welded
to the vehicle's collision jitter). Fixing it properly means building the missing weapon
and aimer path, not patching the existing one.

## Soldiers

**Charged jump for soldiers** - Speeder bikes get a hold-to-charge jump driven by five
`EntityHoverClass` ODF properties; soldiers only get a single fixed `JumpHeight` impulse.
Goal is to give soldiers the same model:

```
JumpTimeMin      = 0.1
JumpTimeMax      = 0.35
JumpForce        = 50.0
JumpMinSpeedMult = 0.4
JumpEnergyPerSec = 100.0
```

All five are parsed today in `EntityHoverClass::SetProperty` only, so this means adding the
properties to `EntitySoldierClass` and porting the charge logic (hold window, force scaling
between min and max hold, forward speed multiplier while charging, energy drain per second)
onto the soldier jump path rather than replacing `JumpHeight` outright. `JumpHeight` should
keep working for ODFs that do not opt in.

**Melee-only blocking** - Right now a blocking melee weapon deflects everything it is set up
to deflect, which makes any shield or blocking stance behave like a lightsaber. Goal is an
ODF or combo property that restricts a weapon's block to incoming melee attacks only, so
blaster fire passes through. The deflect call site is already known from the saber block
work, so this is a matter of finding what identifies the incoming attack at that point and
adding a per-weapon gate, not new plumbing.

**Real riot shields** - A shield that actually stops shots by geometry rather than by a
deflect rule. Needs per-unit collision on the shield part, which the soldier collision model
does not currently provide: soldiers use a single capsule, and the only existing example of
custom soldier collision is the acklay style units, whose collision also does a ground check
that a shield must not inherit. So the real work is a soldier collision path that supports
extra attached collision volumes without dragging the ground handling along with it. Large,
and gated on that collision work rather than on anything shield specific.

**Improved dual pistols** - Two visible pistols that alternate fire, instead of the current
one model and one muzzle. The only dual wield support the engine offers today is
`OffhandGeometryName`, and every stock use of it is on a lightsaber, so a `cannon` class
weapon that wants a second pistol has nothing to hang it on. Two routes:

- Make `OffhandGeometryName` work outside melee weapons. The smaller change, but it only
  ever attached a second *model*, so it buys the look and none of the behaviour.
- Preferred: a new `dualcannon` ClassLabel deriving from `cannon`, owning both the second
  model and the fire alternation.

Either way the offhand attachment point should be named from the ODF rather than hardcoded,
something like `OffhandHardPoint = "hp_weapons2"`, with the matching hardpoint added to the
skeleton and model. The stock soldier skeleton carries only `hp_weapons` (CRC `0x2b960099`).

The `dualcannon` route is cheaper than it sounds: registering a new `ClassLabel` is one
allocation plus one constructor call, and the whole extension surface is two pure virtuals
(`Derive` and `Build`) plus `SetProperty`. Extra per-instance state is free because the new
class owns every allocation of its own type. Full write-up, including what still has to be
checked before it could ship, in
[docs/RE/WeaponClassFactory.md](docs/RE/WeaponClassFactory.md). That does not settle the
open question below, which is the part that actually needs deciding.

Firing model: when weapon 1 finishes its salvo, switch to weapon 2; when weapon 2 finishes,
switch back. An ODF option should pick the timing:

- continuous - one trigger pull alternates 1, 2, 1, 2 for as long as it is held
- per shot - one salvo per trigger pull, so fire 1, release, fire 2

What is already mapped, so this does not start from nothing:

- `Weapon` carries an `mIsOffhand` bit (`+0x2B0` bit 7), so the engine already distinguishes
  an offhand weapon instance.
- `EntitySoldier` carries a dual wield flag byte (modtools `+0x24A`, release `+0x232`,
  bit 0), already in `entity_layout.hpp`.
- That flag already reroutes input: the character weapon path treats channel 1 of a dual
  wield pair as firing off the *reload* trigger, with no reload of its own. So the engine's
  existing notion of dual wield is "two channels, the second one on the reload trigger",
  not "one channel that alternates". Deciding whether to extend that or bypass it comes
  first, because it settles whether this is a `Weapon` level change or an `EntitySoldier`
  input change.

Open question: whether the offhand should be a real second `Weapon` instance (two ammo
pools, two reloads, two muzzle effects) or one weapon that alternates its fire origin
between two hardpoints. The first is what "dual pistols" implies and is what the alternating
salvo logic naturally wants; the second is far cheaper and may be enough if the ask turns
out to be visual plus muzzle alternation.

## Weapons

**Force pushable grenades** - Force push moves units and ignores thrown ordnance, so a
grenade sails straight through a push that would have thrown a soldier across the room. Goal
is to let a push pick up a live grenade and send it back.

Needs tracing first: what the push collects as targets and whether an in flight ordnance can
join that set at all, and how to move one once found, since the ordnance is already running
its own trajectory.

Identifying a grenade at the push site is the part that needs care. `ClassLabel = "grenade"`
sits on the *weapon* ODF; what flies is an ordnance instance from a separate ordnance ODF,
so nothing at the push site sees that label. Walking back to the spawning weapon to test it
would work but is blanket, catching any weapon that borrowed the label. Preferred is an opt
in property on the ordnance class: the grappling hook already adds `PullSpeed` and `MaxRange`
that way through `OrdnanceClass::SetProperty`, and an `Ordnance` carries its `OrdnanceClass*`
at `+0x30`, so the lookup is solved. What it must not be is the ordnance ClassLabel, which
would sweep up rockets, mines and anything else on the same label.

## Rendering

**Restore the decal system** - BF2 ships a complete decal pipeline with only the
`Add*Decal` entry points gutted, so surfaces never take burn or impact marks. Target is
marks on walls and props; character marks are out of scope, since the engine has no
per-bone decal skinning and no Ghoul2 equivalent. Ordered plan:

- **A. Proof of life.** Build a `DecalClass`, allocate one `Decal` from `Decal::sMemoryPool`,
  fill a hardcoded 4-vertex quad, link it into `m_decals` and see if it draws. This answers
  what disassembly cannot: whether a `DecalClass` instantiates end to end, whether the shader
  resolves, and whether the empty `PlatformInit` is fatal. If a quad will not render, stop.
- **B. Terrain decals.** Transcribe Phantom's `ComputeTerrainDecal` and drive it from A's
  spawn path. Retail dead-stripped it, so Phantom's body is the original shipping
  implementation - transcription, not invention. Gives a validated reference decal to check
  step D against.
- **C. Find a `RayTest` that returns hit position plus normal.** Untraced. Blaster bolts must
  already compute one to place impact effects. Unblocks D and may shrink it considerably.
- **D. Write `ComputeObjectDecal`.** The real work, and the only piece with no existing body
  to copy - it is empty in all three builds, Phantom included. Clip the projection quad
  against the collision object's triangles. Q3-derived implementations are GPLv2 and this
  repo is MIT, so read for design only.
- **E. Wire it up.** Hook `WeaponMelee::UpdateFire` after the ray test returns true; the
  object, ray index and segment are all in hand there.

A and B are confidently estimable. C and D are where the schedule can move. Once the
pipeline exists, blaster impacts and explosion scorch marks are nearly free and will be far
more visible in normal play than saber marks.

## Sound

**Sound region and stream manipulation** - Goal is runtime control over ambient sound
regions and streams. Current state of the problem:

- Lua `SetProperty` can never reach sound entities. `EntitySound` derives from `Entity`,
  not `EntityEx`, so it is absent from the id map `SetProperty` looks in. This is
  structural, not a missing case.
- `SetClassProperty` does work on `SoundAmbienceStatic` and `SoundAmbienceStreaming`, but
  only for four properties (`Sound`, `SoundStream`, `MinDistance`, `MaxDistance`) and only
  before the entity is created, since it edits the class and not the instance.
- Sound regions clobber those values anyway once they take over.
- So the open question is a respawn or re-apply path: change the class, then force the
  existing sound entities to rebuild from it, without regions immediately overwriting the
  result.

**Audio stream queue-item pool** - Raising the audio stream limit from 6 to 12 slots left
the shared queue-item pool at 24 entries. Each slot queues to a depth of four, so the
demand ceiling is 48 requests against a pool of 24, and running the pool dry is a null
dereference rather than a dropped request. It cannot be grown where it sits: 24 entries of
`0x34` bytes from `0x0233a240` end at `0x0233a720`, which is itself a live global with
five references, so the pool has to be relocated the way the stream arrays themselves were.
Exhaustion should also be made to degrade into dropping the request instead of crashing.

**The EAX crackle - investigated, not found in BF2** - Recorded here so nobody re-treads
it. The symptom is a random, loud, distorted burst during matches with EAX enabled. Seven
hypotheses were tested and all seven were eliminated: five by static analysis, and the two
that survived that by runtime instrumentation.

- `DSBufferRenderer::UpdateGain` (`0x008997C0`) converts to Q15 with
  `(int)(gain * 32767.0f) << 16` and no clamp. Refuted: 483,253 calls, every one of them
  on the unclamped path, and the gain product never exceeded 1.0, with a maximum seen of
  exactly 1.000. The same measurement showed flags bit `0x10` is never set, which makes
  the float gain path unreachable dead code.
- `StreamResampler::GetPacket` publishes `mOutputPacket.mBufferUsed` in samples on the
  unity-rate path (`0x008A59A8`) but in bytes on the interpolating path (`0x008A5A99`),
  while consumers read bytes. The unit mismatch is real but harmless: `mOutputPacket` is
  refcount protected (`0x008A58B8`) and `GetPacket` returns NULL rather than overwrite a
  held packet, and of 7084 measured unity-rate calls none had a held input packet. The
  feared `WriteData` runaway went the same way - maximum packet cursor 110,544 against a
  packet size of 110,592 across 1.88 million writes, so the exact-equality release works.

BF2's own output never approaches full scale either: the loudest sample in a whole session
was 9,829 of 32,767, about -10.5 dBFS, with no saturation bursts at all. No mechanism
inside BF2 that could produce a loud burst survives. Because a driver reporting 129
hardware 3D buffers proves a DirectSound wrapper is in use - native Vista and later report
zero - the wrapper itself is the remaining suspect. That is a conclusion about where not
to look rather than a fix, and the next move is measurement outside the engine. The sound
diagnostic that shipped alongside the investigation watches the PCM leaving the engine for
runs pinned at full scale, so a burst caught in the act would be logged with a timestamp
and voice index.

## Retail builds

**Voice limit is modtools only** - The patch installs on modtools and no-ops on Steam
and GOG, so retail players do not get it. This is not an address lookup: the retail
addresses are known, but six of the sites are encoded differently and the installer
hardcodes the modtools forms. The clamp is a `CMOVcc` rather than a `C7 05` store and
the `smVoices` write is split, so both operands sit at +1 instead of +6;
`SetCentrePeakMode` uses the 6-byte general compare rather than the 5-byte accumulator
form, moving its operand to +2; the four probe-array references are 6-byte
`LEA r32,[EBP+disp32]` rather than 7-byte `[ESP+disp32]`, so the replacement needs one
NOP and not two, and one of them changed target register. Worst, the software-pin site
is a 3-byte `MOV EAX,[EBP+0x20]` against modtools' 7 bytes AND is a branch target, so
there is no byte-for-byte replacement at all.

Porting it therefore means restructuring the installer around per-build encoding
descriptors rather than adding addresses. One further trap: the `smVoices` site's
expected value is a POINTER, and the retail images are rebased at load, so the
installer's own verification needs the same treatment `values_are_va` gave the patch
table.

The concurrent voice raise touches several sites at once - `gMaxVoices`, the hardware
buffer probe's count and its array, the `Voice` pool and both voice ceilings - and only
the modtools addresses for those have been derived. The installer verifies every site
against its expected bytes and disables the whole feature if any one of them fails to
match, so this is a matter of locating each site on retail, not of teaching the patch to
tolerate a partial match. Verifying a port needs checking both mixing paths, since the
hardware and software branches of `Engine::Open` reach the pool through different code and
only one of them is exercised on any given machine.

**Branch region fix unverified on retail** - The fix carries addresses for all three
builds. The vtable slot correction has a vtable, a wrong slot and the real `CreateRegion`
recorded for modtools, Steam and GOG, and the `mHashID` offset at `+0x20` that the id
re-stamp writes is verified on modtools and on both retail builds. It has only been
confirmed in play on modtools, though, where all 24 branch region lookups resolve with no
warnings. Steam and GOG have never been exercised in an actual match.

Retail is also harder to check than modtools was, because it strips the `RedWarning`
string: a failed lookup produces no `Unable to find branch region` line, or any other
line. A retail pass has to lean on `[Diagnostic] BranchRegionDebug=1` and on watching units
actually take the branch, rather than on the absence of warnings.

## Sound

**Only the first hero per team gets its SndHero* VO** - reported in play, mechanism located
2026-08-21, NOT yet fixed.

The four hero ODF params `SndHeroSelectable`, `SndHeroSpawned`, `SndHeroDefeated` and
`SndHeroKiller` land in PER-TEAM arrays of three on `HeroMessageDisplay`, not on the hero
class:

| global | phantom addr |
|---|---|
| `mHeroesUnlocked` | `0x00B26BB4` |
| `mHeroUnlocked` | `0x00B26BB8` |
| `mHeroSelectable` | `0x00B26BC4` |
| `mHeroSpawned` | `0x00B26BD0` |
| `mHeroDefeated` | `0x00B26BDC` |

Every one of them is written ONLY in `HeroMessageDisplay::Create` (writes at `0x005EAC92`,
`0x005EACBD`, `0x005EACDF`, `0x005EAD01`, `0x005EAD23`) and in `Destroy`. There is one slot per
team, so whichever hero class populates it first owns those sounds for the whole round. Playback
then just reads the slot - `PlayHeroSoundUnlock` `0x005EB3F0`, `PlayHeroSoundSelectable`
`0x005EB170`, `PlayHeroSoundSpawn` `0x005EB1A0`, `PlayHeroSoundDefeat` `0x005EB0D0`.
`ScriptCB_SetSoundEffect` (`0x00664758` region) can also overwrite them from script.

**Very likely the same root as a second confirmed bug.** `Team::GetHeroClass` (phantom
`0x00775D50`) is a linear scan that returns the FIRST class in the team's `mClassArray` whose
`GameObjectClass+0x78` has bit 1 set - it has no idea which hero is actually active:

```c
for (i = 0; i < mClassCount; i++) {
   pGVar2 = mClassArray[i];
   if (pGVar2 && (pGVar2->field_0x78 & 2) && pGVar2->mLabel)
      return pGVar2;            // first match, always
}
```

`Lua_Callbacks::SetHeroClass` (`0x00653680`) just appends via `Team::AddSpecialClass`, so the
first `SetHeroClass` call in the mission script wins permanently. Its three consumers are all
wrong with multiple heroes per side: `Character::GetHeroName` (`0x0049DDA0` - takes the name
from `GetHeroClass(team)` instead of from the character's OWN class, so hero 2 displays under
hero 1's name), `NetGame::AddKillMessage` (`0x0066E69A`), and `PlayHeroSoundSpawn`
(`0x005EB24F`).

PROPOSED FIX, not built: hook `Team::GetHeroClass` and, when a hero is currently active for that
team, return that unit's actual class, falling back to the stock scan. Resolve the active hero
from `netHeroPlayerID[team]` / `netHeroAIID[team]`, which `PlayHeroSoundSpawn` already uses. That
one hook fixes the name and the kill message. Whether it also fixes the VO depends on confirming
that `HeroMessageDisplay::Create` sources those five sounds via `GetHeroClass` - READ `Create`
(`0x005EAC50` region) FIRST; if it instead reads from a script-set or team-registration path, the
VO needs its own fix, most likely re-populating the slots when the active hero changes.

NOT the same thing: `ScriptCB_EnableHeroVO` (`0x006656D0`) only sets
`EntitySoldier::sEnableAnnouncementVO` (`0x00A8EDF3`), read once in `EntitySoldier::Init` at
`0x0056FA5D`, which gates a VO member on the spawning soldier's OWN class (`mClass+0x1464`).
That path looks correct and is unrelated to the `SndHero*` slots.

## Limits

**Attached effects past 64** - Capped at **255 no matter what**, and not recommended.

WHAT ACTUALLY CONSUMES A SLOT, traced 2026-08-21. `AttachedEffectsClass::SetProperty` has
exactly ONE caller, `EntityGeometryClass::SetProperty`, and `BuildAttachedEffectsClass` has
exactly one, `EntityGeometryClass::PostReadSetup` - which drains the table and sets
`s_uiNumAttached = 0`. So the 64 is **per geometry class**, accumulated across that one ODF's
property read and flushed at the end of it.

It is a stage-then-commit design; of the five property hashes only two increment:

| Hash | Parses | Count |
|---|---|---|
| `0x576b09cd` | `"%s %s"` effect + bone | **+1** |
| `0x3be7b80a` | `"%s %f %f"` bone + offsets | **+1** |
| `0x6a6c7e0d` | effect name -> `_Find` in the 256-slot effect table | stages only |
| `0xa9d0d48b` | odf name, must be an `EntityLight` | stages only |
| `0x51e2c845` | `atoi` -> dynamic flag | none |

So one attachment is typically TWO ODF lines but ONE slot: the limit is ~64 attached
effects/lights on a single object, which is why normal content never approaches it.

**CORRECTION.** An earlier note here claimed an inflated count "leaks from a non-geometry ODF
into the next geometry class". That is WRONG and was never true: the only path into the counter
is `EntityGeometryClass::SetProperty`, so a non-geometry ODF cannot contribute at all.

Not to be confused with `EntityProp`'s own separate attachment cap, which has its own warning
("EntityProp '%s' AttachToHardPoint '%s' too many attached odfs (max %d)") and its own array.
`AttachedEffectsClass::m_uiNumAttached` is a `uint:8`: the count is written with a BYTE store
(modtools `0x004C1BF6 MOV byte [EBP+4],AL`, Phantom `0x00490376`) and all three consumer loops
read it back masked `& 0xFF`, so a 300-effect class silently becomes a 44-effect one. Past 255
means growing the object from 8 bytes and re-laying out the `bDynamic` bit at bit 8.
(`uiNumParams` is `uint:7`, its own separate ceiling.)

There is also no room after the array - modtools' next byte at `0x00B7A7D8` is `s_bDynamic`,
Phantom's next dword at `0x00ABC058` is the counter itself - so the table has to be relocated to
a fresh `20*NEW` allocation, and roughly twenty baked-in absolute displacements rewritten per
build: the six `CMP ...,0x40` sites (modtools `0x004C27B8`, `0x004C285B`, `0x004C28A5`,
`0x004C29AC`, `0x004C29E0`, `0x004C2ACF`), the memcpy source constant at `0x004C1C20`
(`MOV ESI,0xB7A2D8`), and every `0x00B7A2C8/CC/D8/DC/E0` displacement in `SetProperty`.
Recommendation: leave it at 64 and let it refuse, which is what the shipped fix does.

**AI reservation pool past 127** - `[LimitIncreases] ReservationPoolSize` now raises
`ReserveManager::sList` from 60 to at most 127, which is a hard encoding ceiling: the count
reaches the allocator through a `PUSH imm8` on every build and `6A ib` is sign-extended, so 0x80
and above makes the count negative, the allocation ~4 GB, `new[]` return NULL and the first
`Reserve` write through NULL. Going higher means re-encoding that push as `PUSH imm32`: a 31-byte
in-place rewrite on modtools, where the 24 bytes of `0xCC` at `0x005C6228` are confirmed free, and
three spare bytes on retail, where the push at Steam `0x00630146` is followed immediately by
`LEA EDI,[EAX+4]`. Only worth doing if 127 is measured to still saturate - and note the warning's
own number cannot measure that, since `mPeak` counts rejected adds since level load rather than
live demand. Every query is a linear scan on `mLength`, so the real cost of a large pool is frame
time, not the 24 bytes per entry.

**Command posts past 16, single player only** - Multiplayer is closed: it is a wire format, not
an array bound. `REL_CHANGECOMMANDPOSTTEAMS` packs 2 bits per post into a uint32 `mTeamBits`,
1 bit per post into a uint16 `mAliveBits`, and every net reference to a post is a 4-bit index
(`WriteBits(pkt, mPostIndex, 4)`). Post 16 writes bit 32 of a 32-bit field; x86 masks the shift
by `&0x1f` so it silently ALIASES ONTO POST 0. Widening changes packet length and there is no
version negotiation on that event.

Single player is feasible, and the earlier "HUD::ElementMap is the blocker" framing is
avoidable. `HUD::ElementMap` embeds `mPost[16]`, `mPostScale[16]`, `mPostText[16]` and
`mPostSelect[16]` INLINE with displacements baked into the accessors, so growing them is a
structural rewrite - but it is not necessary. Leave the HUD at 16 and grow only the game-side
arrays: the HUD iterates its own 16 and indexes `gPost[i]`, so it stays in bounds as long as
`gPost` has at least 16. Posts 17+ then work but do not appear on map or radar.

Sites involved (NOT byte-verified, scope before building):
- `sPostArray` client/host, modtools `0x00B93B58` / `0x00B93B98`, retail both `0x01E308A0` /
  `0x01E308E0`. Base imm32s at modtools `0x0064972F`, `0x00649820`, `0x00649850`;
  retail `0x0047AA51`, `0x0047AB20`.
- memset size: modtools `0x00649723` imm32 (any value); retail `0x0047AA40` `6A 40` imm8
  sign-extended, so max 0x7F = 31 slots without re-encoding.
- `CommandPostItor::operator_bool` - the funnel ~20 AI/spawn/HUD callers pass through. TWO
  immediates, both must move: modtools `0x00650035` and `0x0065004E`; Steam/GOG `0x0047F8B5`
  and `0x0047F8C7`. imm8, max 0x7F.
- `TargetManager::_gPost[16]` (Phantom `0x00C4DCE8`, 48 B/elem) relocatable via its pointer at
  `0x00A9A1B4`. **`AddPost` `0x00773F10` uses `0x10` as BOTH a loop bound AND a no-free-slot
  sentinel across three coordinated occurrences** - a partial patch turns "no slot" into an
  out-of-bounds write.
- `PlayerStats::AddKill` `0x007217C0` - two loops, `!= 0x40` (byte count) and `< 0x10` (index).

Fix regardless of any of this: `CommandPost::BuildPost` (modtools `0x0064FDF0`) checks
`83 FF 10` at `0x0064FF13`, logs "Exceeded %d command posts!" and then **writes out of bounds
anyway**. Retail stripped the check entirely, so post 17 on a retail host already overwrites the
`sActivePostCount` pointer today.

THE FOOTGUN: a static byte patch cannot tell single player from multiplayer. Enabled online it
corrupts post ownership silently. This needs a runtime `netEnabled` check, so it is a hook, not
a table patch.

## AI

**Flyers on maps with no flyer paths** - The circling investigated at length in
docs/RE/FlyerAI.md turned out to be a map with no flyer paths authored, not an engine
fault. The `AIUtil::StopDist` / `GonePastDest` threshold analysis there is verified and
still stands, but it was not the cause and is no longer a priority.

The remaining idea is a feature rather than a fix: `UnitFlyAgent`'s FLY state has no
failed-path branch, while the engine already has a path-free movement mode in
`AILowLevel::SetNavigator_GotoDirect` (phantom 0x00481050), used by `DoFlyDirect` for
combat strafing and by `UnitRandomAgent::PickRandomDest`. Falling back to it when the
path request fails would make flyers usable on maps that never authored a flyer path
network. Not costed. Would need the failed-path signal (`AILowLevel` navigator failed
flag) wired into the FLY state, and modtools offsets only - retail EntityFlyer interior
offsets differ.

## First person

**SHELVED 2026-08-21, both approaches, by the map author's judgement after testing.**
Neither `FirstPersonMelee` nor `TrueFirstPersonBody` ships any more - both patch sets and
both INI keys were removed. The verdict: BF2's first person is built around a floating arms
model, the camera is a static eye offset with no head tracking, and it reads as janky for a
saber hero no matter which route you take. Fixing it properly means rewriting the first
person system, not patching it.

Everything below is preserved so a future attempt does not start from zero. All addresses
were verified from bytes.

### What worked

Saber heroes CAN be put in first person, and the blade CAN be made to draw:
- `EntitySoldier::IsForcedThirdPerson` returns true whenever either weapon slot answers
  `IsMelee`, which is what pins them to third person. modtools `0x0052B670`, Steam and GOG
  both `0x004DE390`, all four bytes `56 57 8B F9` -> `32 C0 C3 90`. It is `__thiscall` with
  no stack args, so a bare RET is correct. Global side effect: un-forces third person for
  every melee unit, Wookiees and Tuskens included.
- `FirstPersonRenderable::RenderSoldier` ORs only `0x00080001` into the render flags, but
  `WeaponMelee::Render` tests `0x4000000` before flushing the blade's particle cache.
  Top byte of that imm32: modtools `0x004AA456`, Steam and GOG `0x005204DD`, `0x00` -> `0x04`.
  Without it the hilt draws and the blade does not.
- Needs the cockpit camera enabled in player options, or nothing looks different.
- A hero also needs a `FirstPerson` ODF line AND its `FPM\<side>\<lvl>.lvl` actually built.
  Most stock saber heroes have no `FirstPerson` line at all - only the blaster heroes do,
  plus Luke Jedi. A model present in memory via another req is NOT enough; the FP path loads
  `FPM\<token>.lvl` specifically.

### Why true first person failed

`TrueFirstPersonBody` neutered the tail call in `EntitySoldier::RenderTrackable`
(modtools `0x0052A3A6`, 8 bytes -> `MOV AL,1` + NOPs). That call is NOT a pure cull
predicate: `Trackable::RenderTrackable` consumes `IsFirstPersonView` and participates in
driving the first person path, so skipping it removed first person rather than un-hiding the
body. Result was a full third-person body with a third-person camera.

The inverted approach, untried: let the call run and patch the decision INSIDE
`Trackable::RenderTrackable` - modtools `0x004BAE03` `74 15` -> `EB 15`, Steam `0x006589E4`
and GOG `0x00659A84` `74 16` -> `EB 16`, Phantom `0x0077C061`. Cost: un-hides vehicle hulls
in cockpit first person.

### Facts worth keeping

- The FP camera is a static per-stance eye offset, `sEyePointOffset[0] = 0.06, 1.70, 0.00`,
  transformed by the entity root. Already at eye height, already free of animation shake -
  and, per testing, this is exactly why it looks wrong: there is no head motion at all.
- The body is already submitted every frame at alpha 0 rather than skipped, which is why the
  player's shadow has always been correct.
- Aim is already the camera ray (`Aimer::SetSoldierInfo(firePos, mEyeDir)`), so shots go
  where the crosshair is regardless of where the body animation points the weapon.
- Camera near plane is 0.7 m (`RedCamera::SetPerspective(0.7, 120.0)` from
  `CameraManager::SetNumCameras`; modtools imm32 `0x004A110B`, Steam `0x0044EC55`, GOG
  `0x0044EC35`). It hides head and torso for free; pulling it to 0.1 to stop the arms being
  sliced is what exposes the head. Head-hiding was never resolved on Steam or GOG because
  `RedModel::Render` could not be located on retail.
- TRAP: the one-byte alternative on modtools is at `0x00535EAA`, NOT `0x00535EA8`.
  `0x00535EA8` is `84 C0`; writing `EB` there gives `EB C0`, a backward jump, instant crash.

### First person weapon and sync states, if this is ever revisited

Distinct from the swing-animation work above and not costed. The first person weapon has its
own state machine that does not track the third person one, so reload, fire, charge, deflect
and melee read as two separate characters doing two different things. What a fix would need:

- The FP state index comes from `FirstPersonRenderable::UpdateSoldier` and is derived from the
  weapon's own fire state plus `Weapon+0xAC` bit 1 (the SignalFire latch, consumed and cleared
  in the same frame). The third person side runs off `SoldierAnimator` with its own timers.
  Nothing reconciles the two.
- FPR playback fields are known: `m_pkAnim +0x1534`, `m_fAnimSpeed +0x154C`, `m_fCurT +0x1550`,
  `m_fLastT +0x1554`, `m_bLoop +0x155E`, `m_bAnimFinished +0x1560`, `mBlendFactor +0x1564`.
  Third person upper body: `SoldierAnimator +0x1600` `m_fCurT`, `+0x160E` `m_bLoop`,
  `+0x1610` `m_bAnimFinished`, `+0x15E4` `m_pkAnim`. `SoldierAnimator` is `EntitySoldier+0x760`.
- So phase sync is expressible as a per-frame float copy, but the two use DIFFERENT skeletons
  (FP has ~8 joints, the character has ZephyrSkeleton<32>), so the animations are not the same
  asset and matching phase does not by itself match pose.
- Do NOT poke `FPR+0x1534` directly - `ZephyrAnimInst<32>::SetAnim` rebuilds the joint index
  tables. Go through `FirstPersonRenderable::SetAnimation` (modtools `0x004A9B80`, thiscall,
  `RET 8`).

Shelved with the rest of first person; the engine's FP is built around a floating arms model
and reconciling it with the body is the same fight documented above.

### Dedicated first person animation, if this is ever revisited

The animation table is NOT the obstacle, contrary to an earlier assessment here. Every saber
attack routes to one slot, `mAnim[24]` = TOOL(2)*11 + SHOOT1(2), re-read with `force=true` on
every new attack state. The per-swing key is one byte, `WeaponMelee+0x1AB` (`m_uiAnimIndex`,
confirmed at `WeaponMelee_data+0x6B` with the `_data` base at `0x140`), written by
`EnterState` from `Combo::State+0x28` BEFORE `mState=FIRE`. Deflects overwrite the same byte,
so they come free. The project already has the injection point: `FirstPersonAnimationBank` as
a custom ODF property, and a per-frame save/overwrite/restore of `mAnim[48]` around
`FirstPersonRenderable::UpdateSoldier`.

Build-divergence traps for that work: `_GetWeaponClassFromWeapon` is `__fastcall` (weapon in
ECX) on modtools but `__cdecl` on Phantom; `MELEE_BASE` is `0x86` on modtools and `0x87` on
Phantom. Never write NULL into an `mAnim[]` slot - `ZephyrPoseDyn<32>::Update` null-derefs
`anim+8`.

## Controller

**Shell and menu navigation** - Gamepad support is gameplay only today. Every binding mode
(`Unit`, `Vehicle`, `Flyer`, `Hero`, `Turret`) maps to an in game control path, so the pad
does nothing in the front end: the main menu, the spawn screen, the map and unit selection,
and the pause menu all still need mouse and keyboard. That makes the controller support a
half answer for anyone actually playing from the couch.

Goal is a shell input mode that drives the existing UI navigation rather than faking mouse
movement: directional input to move the selection, a confirm and a back action, and shoulder
buttons for tab or page changes where the screen has them. Needs the shell's own input and
focus handling traced first, including whether the spawn screen and the pause menu go
through the same path as the main menu or each roll their own, since that decides whether
this is one mode or several.

## Lua API

**`GetProperty` / `GetClassProperty`** - The read side of the existing `SetProperty` /
`SetClassProperty` pair. Same entity and class lookup, same property name resolution,
returning the current value instead of writing one. The sound entity limitation above
applies identically to `GetProperty`.

**Hero health drain switch** - A way to stop heroes from bleeding health over time. This
belongs in Lua rather than in an ODF property: the drain is a game rule, and an ODF entry
would force every hero class to be edited individually and would not let a script turn it off
for one mode and leave it on for another. Needs the code that applies the drain traced first,
then a toggle hung off that path.

## Documentation

**AI systems documentation** - Write up the goal layer (`AIGoalManager::AssignUnit`) and the
combat response layer (`SelectCombatResponse`), how a unit gets from a team level goal to an
individual combat action, and which ODF and Lua knobs actually feed into it. Should also
record the confirmed no-ops so others stop trying to use them.

**Improve RedConsoleCommands documentation** - The existing
[reference](docs/RE/RedConsoleCommands.md) was largely built from symbol names, so a number of
entries are inferred rather than verified. Needs a pass that actually exercises the
commands, corrects the inferred descriptions, and marks the ones confirmed to be dead
no-ops.

## Debugging

**Class registries: DONE 2026-08-22.** All four unknown list globals from the earlier sweep are
identified, and the census now counts and buckets them.

| modtools global | holds |
|---|---|
| `0x00ACD2C4` | `Factory<Entity,EntityClass,EntityDesc>::sList` (was already known) |
| `0x00AD43BC` | `Factory<Weapon,WeaponClass,WeaponDesc>::sList` |
| `0x00AD3A60` | `Factory<Ordnance,OrdnanceClass,OrdnanceDesc>::sList` |
| `0x00AD388C` | `Factory<Ordnance,ExplosionClass,OrdnanceDesc>::sList` |
| `0x00AC69F0` | `GamePathFactory::sList` - **never walk it**, see below |
| `0x00AD3450` | EntityPath branch regions (was already known) |

`GamePathFactory` is deliberately reported as a bare count and never traversed. The path-chunk
parser declares a factory in a **stack local** and links that frame into the global list once per
`path` record (modtools `FUN_0044B8E0`, `int local_480[6]` = 0x18 bytes = exactly one factory),
so dereferencing a node there can mean reading another thread's live stack. Its count at
`0x00AC6A00` sits in `{0, 3, 4}`: 0 before the first load and after teardown, 3 while a state is
live, 4 transiently during path parsing.

**Bucketing is by parent chain, not by `IsRtti`.** Every `Factory<>` object carries `mParent`
`+0x14` and `mId` `+0x18`, and both are written *before* the node becomes reachable (entity ctor
`0x004D0C20` stores them at `0x004D0C43`; the forward link that makes the node findable is not
written until `0x004D0CBA`). The vptr is written LAST, so those two fields are the only ones
guaranteed valid for every node a walker can see. Following `mParent` lands on one of the 46
built-in roots created by `GameState::CreateBaseEntityClasses` (modtools `0x0044CDA0`, Steam
`0x00539F60` - same names, same order, so the table is build-invariant), all through the
one-argument ctor, so every root has `mParent == 0` and the chain always terminates.

**`IsRtti` was rejected outright and must stay rejected.** The `Factory<>` base vftable has only
THREE slots and `IsRtti` sits past the end of it. An object carries that base vftable in two
windows during which it is fully linked and reachable, and in those windows the slot reads 0 on
modtools. Calling it from the diagnostic thread would be a call through a null or non-code
pointer; `__try` catches an access violation but does not contain execution that has already
wandered. **Never call a virtual on a walked object.**

Two corrections to earlier notes, both now fixed in `game_addrs.hpp`:
- `mFilename[32]` is at **`+0x20`**, not `+28`. Proven by `EntityClass::Read` (modtools
  `0x004D0830`): `strlcpy(obj+0x20, buf, 0x20)`. The modtools Ghidra database has the same error,
  so decompiler output naming `mFilename` at `+0x18` is really reading `mId`.
- The name offset differs per registry: entity `+0x20`, weapon `+0x30`, ordnance `+0x28`,
  explosion `+0x20`.

Because `EntityClass::Read` hashes the TYPE-chunk buffer into `mId` and then copies *that same
buffer* to the name field, `PblHash(name) == mId` is a self-check that can only match. The census
runs it and reports mismatches separately from truncation (`strlcpy` caps at 31 chars + NUL, so a
31-character name legitimately will not hash back).

**Names exist on retail only for weapons**, at `+0x30` (Steam `WeaponClass::Read` `0x0067A390`;
the same 13-byte sequence occurs exactly once in GOG at `0x0067B430`). Retail `EntityClass` is
0x60 bytes and `+0x20` is `mLabel`, a `wchar_t*` - reading it as `char[32]` yields wide-char soup,
so entity/ordnance/explosion name offsets are deliberately absent for those builds and the census
omits the column rather than printing garbage.

Still open on this thread: whether any entity class is created outside `CreateBaseEntityClasses`
and `EntityClass::Read`. `CreateClass` is a vtable slot so static xrefs cannot close it. The
census answers it empirically - a root whose `mId` is not one of the 46 prints as
`unknown-root`.

**Content budget report ("what did I actually put in this map?")** - requested 2026-08-21. A
census of the things a MODDER AUTHORS, reported as occupancy against the ceiling, because the
ceilings are invisible until you hit them. Deliberately NOT a runtime profiler: voices, AI
reservations and the particle/renderer caches are engine state the author does not control, so
they are out of scope.

The neat framing: **report against every ceiling BF2GameExt already knows how to raise.**
Everything in `[LimitIncreases]` is a limit we have already located, so the ceiling half is
mostly free. Candidate metrics, all author-controlled:

  effect classes (256) - the headline, and it settles the softlock question on its own
  object count (1024 / 2048)      combo animations (30 / 90)
  high-res animations             string pool
  sound layers                    DLC missions
  GC visual limits                command posts (16)
  attached effects per class (64) tentacles per class (4)

**Where the work actually is.** Knowing each ceiling is easy - we patch them. Knowing current
OCCUPANCY is the new reverse engineering, and it differs per subsystem:
  - Hash tables are trivial and self-verifying: scan the key slots and count non-zero. Use this
    wherever possible rather than trusting a stored counter, which may be a different field than
    you think (`_head._pObject` vs `_iCount` already burned us once).
  - Others need a live counter global located per build, three times over.
  - `s_uiNumAttached` is drained per class, so a live read is meaningless - it needs a hook to
    record the PEAK across a level load.

**Triggering.** One census function behind three triggers, in order of importance:

  1. **Periodic, every ~30 s** - the default, works on every build, needs no user action. This is
     the one that matters on retail, and a 30 s cadence also catches anything that grows during
     play rather than only the load-time picture.
  2. **Lua binding** - on demand, and note this works on ALL THREE BUILDS, not just modtools:
     Lua is the mission scripting engine and is present everywhere. Callable from a mission
     script, so it covers retail where there is no console.
  3. **Debug console command** - modtools only (the console is gated in `lua_hooks.cpp`), but
     free to add since it calls the same function.

The periodic print is what makes this useful for the softlock hunt specifically: it prints the
effect-class occupancy seconds before the reported freeze, on the build the freeze happens on.

**HASHES ARE READABLE - use `StringDB`.** Scanning a hash table yields keys, not names, which
would make the report useless for finding WHICH effect is the problem. The engine already solves
this: `StringDB::Store(hash, str)` (Phantom `0x00773620`) copies the string into the string pool
and records hash -> pointer in `mMap`, a 4096-slot table. So
`_Find(StringDB::mMap._uiTable, 0x2000, hash)` gives the original name back for anything that
was stored. `StringDB::Remove` is at `0x00773600`.

For names that never went through StringDB, the fallback is one hook on
`PblHash::PblHash(PblHash* out, const char* str)` - every name the game hashes passes through
it, so we can build a complete reverse map ourselves. Noisy during load, fine for a diagnostic.

**StringDB is itself a metric worth reporting**, and a purely authored one: `mMap._iNumEntries`
against 4096, plus string pool bytes used against `mPoolSize`. It has its own warning,
"String pool is full: %i pool is not big enough!", and it is the same limit
`[LimitIncreases] StringPoolIncrease` already raises.

**Third instance of the uncapped-probe hang.** `StringDB::Store` calls the same
`PblHashTableCode::_Store`, so a StringDB map at 4096 distinct strings would spin forever
exactly like the effect table at 256. That is now three tables sharing one defect
(`s_EffectClasses`, `s_EffectFactories`, `StringDB::mMap`), which argues the real fix is a probe
counter inside `_Find`/`_Store` rather than per-table capacity guards - though that changes
behaviour for every hash table in the game and needs care.

**Not mod-addable, do not include:** effect FACTORIES (32 slots, ~26 used by built-in effect
managers) are engine types registered by `FLEffect::InitAll`, not content. Worth knowing the
headroom is thin, but an author cannot change it.



**All diagnostics are now on all three builds.** `AIUpdateDiag`, `SoundDiagnostic` and
`BranchRegionDebug` were ported 2026-08-21; the VoiceVirtual offsets were verified identical on
retail rather than assumed, and the full teardown is in `docs/RE/SoundSystem.md`.

Two defects in the shipped modtools diagnostics were found and fixed on the way:

- `BranchRegionDebug`'s live count read `0x00AD345C`, which is `sList._head._pObject` and on a
  list HEAD is structurally always null. It has no read-write reference in the image. The count
  is `_iCount` at `0x00AD3460`, written by `BranchRegion`'s ctor and dtor. **Every `(live=%u)`
  the module ever printed was a hardcoded zero.**
- `SoundDiagnostic` mapped a renderer back to a voice with `voice = renderer - 0xE8`, believing
  a `DSBufferRenderer` was embedded at `Voice + 0xE8`. Phantom's PDB says that offset holds a
  `StreamRenderer` (748 bytes) and `DSBufferRenderer` is a different 416-byte class, so every
  voice index printed was garbage. It now scans the pool and matches pointers, so a wrong
  assumption yields "unknown" rather than a confident wrong answer.

Still open, and deliberately not guessed: GOG's `smRendererList` head, GOG `smTimeElapsed`,
GOG `DSBufferRenderer::SetFormat`, and the GOG voice-count command-line global were never read
out of the GOG image. None is needed by the shipped diagnostics.

## Will not do

**Splitscreen.** For anyone reading this: no, splitscreen will not be a thing.

It is not a matter of flipping the disabled flag. The PC build compiles the relevant arrays
down to a single element, so simply unclamping the camera count writes past the end of those
arrays and corrupts memory rather than producing a second viewport. Making it real means
rebuilding those structures and every consumer of them. Notes on the system are in
[SplitscreenSystem.md](docs/RE/SplitscreenSystem.md) for the curious, but it is documentation, not
a plan.
