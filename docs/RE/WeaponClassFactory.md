# Weapon Class Factory — adding a `ClassLabel`

How BF2 turns `ClassLabel = "cannon"` in a weapon ODF into a live C++ class, and what
it would actually cost to register a new one (`dualcannon`, say) from the DLL.

All addresses are **Phantom** (`Battlefront2_Phantom.exe`, the named-PDB reference
build) unless stated otherwise. See [Porting checklist](#porting-checklist) for what
has to be derived per build before any of this can be implemented.

---

## Verdict

| Goal | Cost |
|------|------|
| A new `ClassLabel` that behaves exactly like an existing one | One allocation and one constructor call. Trivial. |
| A new `ClassLabel` with its own ODF properties and its own fire behaviour | Two synthesized vtables and four overridden virtuals. Moderate, and bounded. |
| Extra per-instance state on the new weapon | Free — we own every allocation of our own type. |

There is no engine resistance here. The factory was written to be extended, the
extension points are two pure virtuals, and nothing about the design is hostile to
doing it from outside the exe. The hard part of any feature that wants a new
ClassLabel is the feature, not the class plumbing.

---

## How a ClassLabel becomes a class

### Registration

`GameState::CreateBaseWeaponClasses` (`0x5DE580`) is a flat list of twenty
allocations, one per stock ClassLabel:

```cpp
new WeaponCannonClass(PblHash("cannon"));       // operator new 0x3DC
new WeaponLauncherClass(PblHash("launcher"));   //             0x3E4
new WeaponMeleeClass(PblHash("melee"));         //             0x408
...
```

The base constructor `WeaponClass::WeaponClass(uint hash)` (`0x7AC140`) does the
registering itself. `WeaponClass` derives from `Factory<Weapon,WeaponClass,WeaponDesc>`,
and the Factory sub-object is what carries the identity:

| Offset | Field | Notes |
|--------|-------|-------|
| +0x00 | vptr | Factory vtable `0xA1C42C` during base construction, then the concrete class's |
| +0x04 | `Node mNode` | intrusive list node, linked into `sList` |
| +0x18 | `uint mId` | **the ClassLabel hash** |
| +0x1C | `uint mIndex` | from `sCounter`, post-incremented |

Globals:

| Item | Address |
|------|---------|
| `Factory<Weapon,WeaponClass,WeaponDesc>::sList._head` | `0xA9D0F4` |
| ↳ `_head._pNext` (iteration start) | `0xA9D0F8` |
| ↳ `_head._pPrev` | `0xA9D0FC` |
| ↳ `_iCount` | `0xA9D104` |
| `Factory<...>::sCounter` | `0xC73B90` |

Both the hash constructor (`0x7AC140`) and the copy constructor (`0x7AC790`) register
and take an index, so **every ODF-derived weapon class consumes an index too**, not
just the twenty base classes.

### The ODF load path

`WeaponClass::Read(PblFileChunk*)` (`0x7AE540`) is the munged-ODF reader and the only
consumer of the registry. It walks the chunks:

| Chunk | ASCII | What it does |
|-------|-------|--------------|
| `0x45534142` | `BASE` | reads the **ClassLabel** string, hashes it, and does a **linear scan of `sList` for a matching `mId`**. Miss → `RedWarning "Weapon base class \"%s\" not found"` (Weapon.cpp:0x644) |
| `0x45505954` | `TYPE` | reads the weapon's own name, hashes it, scans `sList` to reject a duplicate, then calls **`base->Derive(nameHash)`** (vtable `+0x04`) to mint the per-ODF class. Stores the name in `StringDB` and resolves the localized label. |
| `0x504f5250` | `PROP` | reads a **pre-hashed uint property id** plus its value string and calls **`derived->SetProperty(id, value)`** (vtable `+0x18`) |

Then it walks the charge chain calling `PostReadInit()` (vtable `+0x1C`).

Two consequences worth being explicit about:

- **Registering a base object with `PblHash("dualcannon")` is genuinely all it takes**
  for `ClassLabel = "dualcannon"` to resolve in an ODF. The lookup is a hash compare
  against a linked list, nothing is baked into a static table.
- **The per-ODF class comes from our own `Derive`**, so every class the loader derives
  from ours inherits our vtable automatically. There is no second registration step.

Property ids arrive **already hashed** from the munge, so a new ODF property name needs
its PblHash computed up front. PblHash is FNV-1a over `(c | 0x20)`, not raw bytes —
`ToolsFL\bin\Hash.exe <str>` is ground truth (see `docs/RE/` notes on PblHash).

---

## The vtable

`WeaponClass` vtable `0xA1C440`; `WeaponCannonClass` vtable `0xA1CC6C`. Twelve slots:

| Slot | Method | Needed for a new class? |
|------|--------|--------------------------|
| +0x00 | `scalar deleting destructor` | inherit |
| +0x04 | **`Derive(uint hash) -> WeaponClass*`** | **override** — mints the per-ODF class; we choose the allocation size |
| +0x08 | **`Build(WeaponDesc*) -> Weapon*`** | **override** — mints the instance; we choose the type and size |
| +0x0C | `IsRtti(uint)` | inherit — keeps existing `IsRtti(WeaponCannon)` checks passing |
| +0x10 | `GetDerivedRtti()` | inherit |
| +0x14 | `GetDerivedRttiName()` | inherit |
| +0x18 | **`SetProperty(uint nameHash, char* value)`** | **override** — new ODF properties, chain to the original for the rest |
| +0x1C | `PostReadInit()` | inherit unless the new props need cross-validation |
| +0x20 | `PostLoadInit()` | inherit |
| +0x24 | `GetHeatPerSalvo()` | inherit |
| +0x28 | `GetShotsPerSalvo()` | inherit |
| +0x2C | `GetBarrageMin()` | inherit |

`Derive` and `Build` are the two pure virtuals on the `Factory` base — the Factory
vtable is `{ scalar_deleting_destructor, _purecall, _purecall }`, so those two slots
are the entire contract a concrete class has to satisfy.

Both are one-liners in every stock class:

```cpp
// WeaponCannonClass::Derive  (0x40EE12 thunk)
WeaponCannonClass* Derive(uint hash) {
    void* p = operator new(0x3DC);
    return p ? WeaponCannonClass(p, this, hash) : nullptr;   // copy ctor
}

// WeaponCannonClass::Build  (0x4127EC thunk -> 0x7B52A0)
WeaponCannon* Build(WeaponDesc* d) {
    PostLoadInit();                                          // vtable +0x20
    void* p = MemoryPool::Allocate(&Weapon::sMemoryPool, 0x1C0);
    return p ? WeaponCannon(p, this, d) : nullptr;
}
```

Synthesizing both vtables from the DLL is routine: `memcpy` `WeaponCannonClass`'s
twelve slots and `WeaponCannon`'s into DLL-owned arrays, swap the slots we want, point
our objects at them. Same technique `weapon/barrel_fire_origin.cpp` already uses,
except allocating a fresh table instead of editing the engine's in place.

---

## Allocation and sizes

### Class objects

Ours, entirely. `Derive` picks the size, so extra class-level fields (new ODF
properties) just extend the object.

**But the engine frees it.** `GameState::PostStateCleanup` (`0x5DF0D0`) destroys every
registered class, so the class object must come from the **engine's `operator new`**
(`0x403BF7`), never the DLL's CRT allocator. Pairing our `malloc` with the engine's
`operator delete` would corrupt the heap on mission teardown.

| Class | Size |
|-------|------|
| `WeaponClass` | 0x304 |
| `WeaponCannonClass` | 0x3DC |
| `WeaponLauncherClass` | 0x3E4 |
| `WeaponMeleeClass` | 0x408 |
| `WeaponShieldClass` | 0x4A4 |

### Instances

`Build` allocates from `Weapon::sMemoryPool` (`0xC73BB8`), a shared fixed-stride pool.
`MemoryPool::Allocate` strides its free list by `mSize`, and only **warns** when the
requested size exceeds it — so overrunning `mSize` is silent heap corruption, not a
failed allocation.

The pool is declared with the *base* `Weapon` size (`MemoryPool(&pool, "Weapon", 0x140)`),
which is smaller than most weapons. It gets raised at startup by a pass at `0x5CC620`
that, for each weapon class size, does:

```asm
mov eax, [Weapon::sMemoryPool.mSize]
cmp eax, <size>
jae  skip
push <size>
mov  ecx, offset Weapon::sMemoryPool
call MemoryPool::SetSize
```

Sizes fed to that pass: `0x140, 0x150, 0x160, 0x180, 0x190, 0x1C0, 0x1D0, 0x1E0,
0x200` — so the pool settles at **0x200 (512)**, and `WeaponCannon` at `0x1C0` has
headroom below that. (One `Build` site allocates `0x1A0` without a matching entry in
the pass; harmless, since it is under the maximum, but it means the pass is a
hand-maintained list rather than something generated, so a new class has to add
itself.)

`MemoryPool::SetSize` (`0x8B1FD0`) is public and refuses only once `mPool` is non-null,
i.e. once the pool has actually been created (lazily, on the first `Allocate`). So a
new weapon subclass **can** be larger than any stock weapon: call `SetSize` at
registration time, exactly the way the engine's own pass does. A second ammo pool, a
channel index, an alternation timer — all free.

Pool count, separately, comes from mission Lua (`SetMemoryPoolSize("Weapon", n)` →
`MemoryPool::SetCount`), which also drags `AmmoCounter` and `EnergyBar` up to match.

---

## Lifetime — this is per game state, not per process

`GameState::CreateBaseWeaponClasses` has exactly one caller:
`GameState::PreStateInit(bool)` (`0x5DF6D0`, call site `0x5DF97A`). The matching
teardown is `GameState::PostStateCleanup(bool)` (`0x5DF0D0`), which destroys the
registered classes and **resets `sCounter` to zero**.

So base weapon classes are built and torn down around every mission load.
**Registration has to happen every time**, not once at DLL load.

The clean hook is `CreateBaseWeaponClasses` itself: call the original, then register
ours. That gets the timing right by construction, re-runs per state automatically, and
puts our class at a deterministic position in the list every time.

---

## Constraints and risks

**Verified:**

- **`sCounter` warns above 254** (`Factory.h:0x1D`, `"Factory::sCounter=%lu"`). It counts
  *every* weapon class, including one per ODF, so a level with many weapons is already
  consuming most of the range. A new base class costs exactly one.
- **`operator new` / `operator delete` must be the engine's** (see above).
- **`WeaponClass::Read` rejects a duplicate `TYPE` hash**, so two ODFs with the same
  weapon name silently keep the first. Not new, but relevant when testing.
- **The `BASE` lookup is a linear scan**, so registration order does not affect
  correctness of the lookup, only `mIndex`.

**Not verified — needs checking before shipping, not after:**

- **What consumes `Factory::mIndex` (+0x1C).** The 254 cap implies a byte-wide consumer
  somewhere. `Weapon::Write(NetPktGroup*)` (`0x6E2880`) does **not** write a class index
  — it writes state, ammo, energy, zoom and aimer data only — so the obvious MP theory
  is unconfirmed. Until the consumer is found, assume index stability matters and keep
  registration at a fixed point in the order.
- **Mixed modded / unmodded MP.** If `mIndex` does reach the wire anywhere, a client
  without the extra class disagrees with one that has it from the first ODF onward.
- **Save games.** Untraced. If anything persists a class index rather than a hash, the
  same concern applies.

---

## What a `dualcannon` would actually take

1. Hook `GameState::CreateBaseWeaponClasses`; after the original returns, engine-`new`
   a `WeaponCannonClass`-sized (or larger) object and run the hash constructor with
   `PblHash("dualcannon")`.
2. Point it at a DLL-owned copy of the `WeaponCannonClass` vtable with `Derive`,
   `Build` and `SetProperty` replaced.
3. `Derive` allocates our size and chains the `WeaponCannonClass` copy constructor, then
   re-points the vptr at our table.
4. `Build` raises the pool item size if needed, allocates, chains the `WeaponCannon`
   constructor, re-points the instance vptr at a DLL-owned copy of the `WeaponCannon`
   vtable.
5. `SetProperty` handles the new hashes (`OffhandHardPoint`, alternation mode, …) and
   forwards everything else to the original.
6. Override whichever of `Fire` / `UpdateFire` / `CheckFire` the behaviour needs.

Steps 1–5 are plumbing and are the part this document says is cheap. Step 6 is the
feature.

---

## Address reference (Phantom, base 0x400000)

| Item | Address |
|------|---------|
| `GameState::PreStateInit(bool)` | `0x5DF6D0` (calls the below at `0x5DF97A`) |
| `GameState::CreateBaseWeaponClasses` | `0x5DE580` |
| `GameState::PostStateCleanup(bool)` | `0x5DF0D0` |
| `WeaponClass::WeaponClass(uint hash)` | `0x7AC140` |
| `WeaponClass::WeaponClass(const WeaponClass*, uint hash)` | `0x7AC790` |
| `WeaponCannonClass::WeaponCannonClass(uint hash)` | `0x7B4C90` (thunk `0x411A5E`) |
| `WeaponCannonClass::WeaponCannonClass(const WeaponCannonClass*, uint hash)` | `0x7B4EC0` (thunk `0x41002D`) |
| `WeaponClass::Read(PblFileChunk*)` | `0x7AE540` |
| `WeaponClass::Derive` / `Build` | thunks `0x4197C7` / `0x41794F` |
| `WeaponCannonClass::Derive` / `Build` | thunks `0x40EE12` / `0x4127EC` (Build body `0x7B52A0`) |
| `Weapon::Render` | `0x7AE8C0` (see [barrel-fire-origin.md](barrel-fire-origin.md)) |
| `Weapon::Write(NetPktGroup*)` | `0x6E2880` (thunk `0x40998F`) |
| Factory vtable | `0xA1C42C` |
| `WeaponClass` vtable | `0xA1C440` |
| `WeaponCannonClass` vtable | `0xA1CC6C` |
| `Weapon` vtable | `0xA1C30C` |
| `Factory<Weapon,WeaponClass,WeaponDesc>::sList._head` | `0xA9D0F4` |
| `Factory<Weapon,WeaponClass,WeaponDesc>::sCounter` | `0xC73B90` |
| `Weapon::sMemoryPool` | `0xC73BB8` |
| ↳ pool item-size raise pass | `0x5CC620` |
| `MemoryPool::MemoryPool(char* name, int size)` | `0x8B1710` (thunk `0x4179C7`) |
| `MemoryPool::Allocate(uint size)` | `0x8B1820` (thunk `0x4188B8`) |
| `MemoryPool::SetSize(uint)` | `0x8B1FD0` (thunk `0x418179`) |
| `MemoryPool::Setup(count, size, grow)` | `0x410807` |
| `operator new` | `0x403BF7` |
| `PblHash::PblHash(char*)` | `0x404AED` |

### Porting checklist

Nothing here is implemented, so nothing has been ported yet. A proof of life needs
these derived for modtools, Steam and GOG:

- `GameState::CreateBaseWeaponClasses` (the hook point)
- `WeaponCannonClass` hash constructor and copy constructor
- `WeaponCannon` constructor
- `WeaponCannonClass` and `WeaponCannon` vtables (for the memcpy source)
- `operator new`
- `Weapon::sMemoryPool` and `MemoryPool::SetSize`
- `PblHash::PblHash` (or reuse the existing `hash_string` entry in `game_addrs.hpp`)

Code addresses port with `tools/port_gog.py code` from Steam; Steam comes off the
modtools→Steam pass in `docs/RE/` (see `project_steam_hook_porting`). Vtables and
globals are data addresses — port them through a referencing instruction, or read the
vtable out of the class constructor's `mov [reg], imm32`, which is how the barrel fix's
slots were confirmed.

---

## Related

- [barrel-fire-origin.md](barrel-fire-origin.md) — vtable patching of `WeaponCannon` /
  `WeaponLauncher` in practice, including the `__thiscall`-under-LTCG check that any new
  virtual override will need repeating.
- `ROADMAP.md` → Soldiers → **Improved dual pistols**, the feature this was scoped for.
  The open design question there (real second `Weapon` instance vs one weapon
  alternating its fire origin, and whether to extend the engine's existing dual-wield
  notion of "channel 1 on the reload trigger") is unaffected by anything in this
  document and is the decision that actually needs making first.
