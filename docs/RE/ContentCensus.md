# Content census: class registries and the effect-class ceiling

`[Diagnostic] ContentCensus` (`PatcherDLL/src/util/content_census.cpp`) reports what a map
actually loaded against the ceilings the engine imposes. It deliberately covers only what a
modder authors: voices, AI reservations and the particle and renderer caches are engine state
the author does not control, so they are out of scope.

Knowing each ceiling is easy, since `[LimitIncreases]` already patches most of them. Knowing
current OCCUPANCY is the real work, and it differs per subsystem:

- Hash tables are trivial and self-verifying: scan the key slots and count non-zero. Prefer this
  to a stored counter, which may be a different field than you think (`_head._pObject` vs
  `_iCount` already caused one bug, see [EntityPathBranchRegions.md](EntityPathBranchRegions.md)).
- Other counters need a live global located per build.
- `s_uiNumAttached` is drained per class, so a live read is meaningless. It needs a hook that
  records the peak across a level load.

The census runs periodically (works on every build and needs no user action), on demand from
Lua as `GameExtContentCensus()` (all builds), and from the debug console on modtools.

---

## The uncapped hash probe - why effect classes are the headline

`PblHashTableCode::_Find` is an open-addressing probe that walks backwards with wraparound and
has NO iteration cap. Its only two exits are "key matches" and "slot is zero", so a table that is
100% full plus a lookup for an absent key spins forever on the main thread: GUI frozen, audio
still playing.

| Build | `_Find` | Probe loop |
|---|---|---|
| modtools | `0x007E1A40` | `0x007E1A62`-`0x007E1A75`, `JNZ` back with no counter |
| Steam | `0x00726E00` | `0x00726E28` |
| GOG | `0x00727ED0` | `0x00727EF8` |

`_Store` has the same shape.

It is reachable from `AttachedEffectsClass::SetProperty`, which calls
`_Find(FLEffect::s_EffectClasses._uiTable, 0x200, effectNameHash)` - 256 key slots. `FLEffect::Read`
registers through `_Store` and tracks `_iNumEntries` but NEVER compares it to capacity; there is no
"too many effect classes" guard anywhere in the image. So a mod with 256+ distinct effect classes
fills the table, and the next lookup of a name that is not in it - exactly what a missing or
typo'd `AttachEffect` line produces - hangs during level load.

| Build | `_Find` callers | `s_EffectClasses` table |
|---|---|---|
| modtools | `0x004C2942`, `0x004C2AFD` | `0x00CF55B4` |
| Steam | `0x004477DF`, `0x004478E7` | `0x01EBD144` |
| GOG | `0x004477BF`, `0x004478C7` | `0x01EBE5F4` |

This is a different condition from the 64-entry attachment overflow that
`[Fixes] AttachedEffectsOverflowFix` handles (see [EngineLimits.md](EngineLimits.md)).

`StringDB::Store` calls the same `_Store`, so a StringDB map at 4096 distinct strings would hang
exactly like the effect table at 256. That makes three tables sharing one defect
(`s_EffectClasses`, `s_EffectFactories`, `StringDB::mMap`), which argues the real fix is a probe
counter inside `_Find` / `_Store` rather than per-table capacity guards. That needs a hook rather
than a byte patch, and it changes behaviour for every hash table in the game, since `_Find` is
generic.

Effect FACTORIES (32 slots, about 26 used by the built-in effect managers) are engine types
registered by `FLEffect::InitAll`, not content. The headroom is thin, but an author cannot change
it, so the census leaves it out.

### Turning hashes back into names

Scanning a hash table yields keys, not names. `StringDB::Store(hash, str)` copies the string into
the string pool and records hash -> pointer in `mMap`, a 4096-slot table, so
`_Find(StringDB::mMap._uiTable, 0x2000, hash)` gives the original name back for anything that was
stored. For names that never went through StringDB, the fallback is a hook on
`PblHash::PblHash(PblHash* out, const char* str)`, which every hashed name passes through.

StringDB is itself an authored metric: `mMap._iNumEntries` against 4096, plus string pool bytes
used against `mPoolSize`. Its warning is "String pool is full: %i pool is not big enough!", the
same limit `[LimitIncreases] StringPoolIncrease` raises.

---

## Class registries

| modtools global | holds |
|---|---|
| `0x00ACD2C4` | `Factory<Entity,EntityClass,EntityDesc>::sList` |
| `0x00AD43BC` | `Factory<Weapon,WeaponClass,WeaponDesc>::sList` |
| `0x00AD3A60` | `Factory<Ordnance,OrdnanceClass,OrdnanceDesc>::sList` |
| `0x00AD388C` | `Factory<Ordnance,ExplosionClass,OrdnanceDesc>::sList` |
| `0x00AC69F0` | `GamePathFactory::sList` - **never walk it**, see below |
| `0x00AD3450` | EntityPath branch regions |

`GamePathFactory` is reported as a bare count and never traversed. The path-chunk parser declares
a factory in a **stack local** and links that frame into the global list once per `path` record
(modtools `0x0044B8E0`, `int local_480[6]` = 0x18 bytes = exactly one factory), so dereferencing a
node there can mean reading another thread's live stack. Its count at `0x00AC6A00` sits in
`{0, 3, 4}`: 0 before the first load and after teardown, 3 while a state is live, 4 transiently
during path parsing.

### Bucketing is by parent chain, not by `IsRtti`

Every `Factory<>` object carries `mParent` `+0x14` and `mId` `+0x18`, and both are written
*before* the node becomes reachable (entity ctor `0x004D0C20` stores them at `0x004D0C43`; the
forward link that makes the node findable is not written until `0x004D0CBA`). The vptr is written
LAST, so those two fields are the only ones guaranteed valid for every node a walker can see.
Following `mParent` lands on one of the 46 built-in roots created by
`GameState::CreateBaseEntityClasses` (modtools `0x0044CDA0`, Steam `0x00539F60` - same names, same
order, so the table is build-invariant), all through the one-argument ctor, so every root has
`mParent == 0` and the chain always terminates.

`IsRtti` must stay rejected. The `Factory<>` base vftable has only THREE slots and `IsRtti` sits
past the end of it. An object carries that base vftable in two windows during which it is fully
linked and reachable, and in those windows the slot reads 0 on modtools. `__try` catches an access
violation but does not contain execution that has already wandered. **Never call a virtual on a
walked object.**

### Names

- `mFilename[32]` is at **`+0x20`**, proven by `EntityClass::Read` (modtools `0x004D0830`):
  `strlcpy(obj+0x20, buf, 0x20)`. The modtools Ghidra database has this wrong, so decompiler
  output naming `mFilename` at `+0x18` is really reading `mId`.
- The name offset differs per registry: entity `+0x20`, weapon `+0x30`, ordnance `+0x28`,
  explosion `+0x20`.
- `EntityClass::Read` hashes the TYPE-chunk buffer into `mId` and then copies that same buffer to
  the name field, so `PblHash(name) == mId` is a self-check that can only match. The census reports
  mismatches separately from truncation (`strlcpy` caps at 31 chars + NUL, so a 31-character name
  legitimately will not hash back).
- **Retail only stores names for weapons**, at `+0x30` (Steam `WeaponClass::Read` `0x0067A390`,
  GOG `0x0067B430`). Retail `EntityClass` is 0x60 bytes and `+0x20` is `mLabel`, a `wchar_t*`, so
  the entity, ordnance and explosion name columns are omitted on Steam and GOG.

### A vehicle registers one EntityClass per passenger seat

All of them carry the vehicle's own name and therefore the same `mId`. Confirmed in a live map:
`republic_fly_transport` and `cis_fly_transport` each showed seven extra registrations, the bombers
two and one, `republic_walk_atte` two, `republic_hover_tank` and the turrets one each. Only the
first of the set is reachable through the class chain; the rest are parentless.

- Equal `mId` means the same ODF, so an unattributed record adopts the family of any sibling
  sharing its `mId`. This is the strongest attribution route and runs last, because it needs a
  sibling to have resolved first.
- The raw registration count overstates authored content, so the census reports the distinct
  `mId` count alongside it.

Still open: whether any entity class is created outside `CreateBaseEntityClasses` and
`EntityClass::Read`. `CreateClass` is a vtable slot, so static xrefs cannot close it. The census
answers it empirically: a root whose `mId` is not one of the 46 prints as `unknown-root`.
