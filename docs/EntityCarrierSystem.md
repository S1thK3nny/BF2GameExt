# EntityCarrier System — Reverse Engineering Notes

Reverse-engineered from `BF2_modtools.exe` using Ghidra and static analysis.
All addresses are **unrelocated** (imagebase = `0x400000`). Resolve at runtime via:

```cpp
uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
void* resolved  = (void*)((unrelocated_addr - 0x400000u) + base);
```

---

## Status

| Finding                                                        | Status       |
|----------------------------------------------------------------|--------------|
| EntityCarrier inherits EntityFlyer                             | ✅ Confirmed |
| `mClass` pointer at `inner+0x66C (= primary+0x8AC)`            | ✅ Confirmed |
| `mCargoSlots[4]` at `inner+0x1DD0 (= primary+0x2010)`, stride `0x14` | ✅ Confirmed |
| `CargoSlot` layout (offset + PblHandle ptr+gen)                | ✅ Confirmed |
| `EntityCarrierClass::mCargoInfo[4]` at `+0x1180`, stride `0x10`| ✅ Confirmed |
| `EntityCarrierClass::mCargoCount` at `+0x11C0`                 | ✅ Confirmed |
| `mSoundCargoPickup` descriptor at `+0x11C4`                    | ✅ Confirmed |
| `mSoundCargoDropoff` descriptor at `+0x11D8`                   | ✅ Confirmed |
| SetProperty cargo-node overflow (count ≥ 4 → self-corruption)  | ✅ Bug fixed |
| AttachCargo/DetachCargo missing slotIdx bounds check           | ✅ Bug fixed |
| AttachCargo null cargo ptr before vtable call                  | ✅ Bug fixed |
| GetDerivedRtti/GetDerivedRttiName — COMDAT-folded with GetRtti | ✅ Confirmed |
| `unaff_EBX + 0x340` in Update — decompiler artifact           | ✅ Confirmed |
| Float cast in AttachCargo offset math — decompiler artifact    | ✅ Confirmed |

---

## Pointer Layout

EntityCarrier / EntityFlyer use **two distinct `this`-pointer bases**:

- **Primary base** — ECX received by `EntityCarrier::Update` (`0x004D7FE0`) and
  `EntityFlyer::Update` (`0x004fc930`).  The thunk at `0x00412ad0` is a plain
  `JMP 0x004fc930` with **no ECX adjustment**, so both functions see the same pointer.
- **Inner base** — ECX received by `AttachCargo` (`0x004D81F0`),
  `DetachCargo` (`0x004D8350`), `UpdateLandedHeight` (`0x004D8130`), and most
  other EntityCarrier methods.  Always equals **primary + 0x240**.

The old documentation used a "struct_base" concept where "struct_base + 0x240 = Update's ECX".
That was incorrect — **Update's ECX is the primary base**; there is no separate struct_base.

Confirmed by disassembly:
- `EntityCarrier::Update` reads cargo at ECX+0x1b9c, count at ECX+0x1be0 (primary offsets).
- `EntityCarrier::AttachCargo` reads cargo at ECX+0x1DDC, count at ECX+0x1E20 (inner offsets).
- All differences are exactly 0x240.

---

## Class Hierarchy

```
EntityEx
 └─ EntityFlyer          (base of all flying entities)
     └─ EntityCarrier    (adds cargo-slot system on top of EntityFlyer)
```

`EntityCarrier::EntityCarrier` (004D74F0) takes `EntityFlyerClass*` as the class arg,
confirming it delegates most of the entity setup to EntityFlyer.  The carrier-specific
data (mCargoSlots, mClass link) is initialised in the EntityCarrier ctor body.

---

## Vtables

### EntityCarrier primary vtable — `0x00A3A670` (72 entries)

```
0x00A3A670  vtable[0]   = EntityCarrier::EntityCarrier (ctor thunk)
...
0x00A3A78C  vtable[71]  = (last entry)
(followed by nulls, then string "EntityCarrier CargoPickupSound")
```

RTTI name string `"EntityCarrier\0"` → `0x00A3A2EC`
RTTI global pointer → `0xB7D4DC`

### EntityCarrierClass vtable — `0x00A3A320` (18 entries)

RTTI name string `"EntityCarrierClass\0"` → `0x00A3A2FC`
RTTI global pointer → `0xB7D484`

`GetDerivedRtti` and `GetDerivedRttiName` are COMDAT-folded into `GetRtti` /
`GetRttiName` respectively — the linker merged the identical 6-byte bodies, so
no separate function symbols exist at distinct addresses.

---

## Struct Layouts

### `EntityCarrier::CargoSlot` — 20 bytes (`0x14`)

Defined as a Ghidra struct. Four of these live at `inner+0x1DD0` (accessible from AttachCargo/UpdateLandedHeight's ECX).

```cpp
struct CargoSlot {          // stride 0x14
    float  mOffset[3];      // +0x00  PblVector3 — attach offset from carrier origin
    void*  mObjectPtr;      // +0x0C  PblHandle::ptr — raw cargo entity pointer
    int    mObjectGen;      // +0x10  PblHandle::generation
};
```

**PblHandle validation**: a slot is considered occupied (and the cargo still alive)
when both of these hold:
```cpp
mObjectPtr != nullptr
*(int*)((char*)mObjectPtr + 0x204) == mObjectGen
```

### `EntityCarrierClass::CargoInfo` — 16 bytes (`0x10`)

Defined as a Ghidra struct. Four of these live at `EntityCarrierClass + 0x1180`.

```cpp
struct CargoInfo {          // stride 0x10
    unsigned int mHash;     // +0x00  node name hash (from kCargoNodeName ODF property)
    float        mOffset[3];// +0x04  attach-point offset vector (from kCargoNodeOffset)
};
```

### Relevant `EntityCarrier` offsets

"Inner" = ECX in AttachCargo/DetachCargo/UpdateLandedHeight.
"Primary" = ECX in Update/EntityFlyer::Update. Primary = inner + 0x240.

| Offset (inner) | Offset (primary) | Type                  | Field                                          |
|----------------|-------------------|-----------------------|------------------------------------------------|
| `+0x66C`       | `+0x8AC`          | `EntityCarrierClass*` | `mClass` — pointer to the class descriptor     |
| `+0x1DD0`      | `+0x2010`         | `CargoSlot[4]`        | `mCargoSlots` (AttachCargo/DetachCargo array)  |
| `+0x1DDC`      | `+0x201C`         | `void*`               | `mCargoSlots[0].mObjectPtr` (derived)          |
| `+0x1DE0`      | `+0x2020`         | `int`                 | `mCargoSlots[0].mObjectGen` (derived)          |
| —              | `+0x1b90`         | `CargoSlot[4]`        | Cargo slots (Update position array)            |
| —              | `+0x1b9c`         | `void*`               | Update cargo slot 0 obj ptr                    |
| —              | `+0x1be0`         | `int`                 | Update cargo count                             |

### Relevant `EntityCarrierClass` offsets

| Offset   | Type           | Field                                              |
|----------|----------------|----------------------------------------------------|
| `+0x1180`| `CargoInfo[4]` | `mCargoInfo`                                       |
| `+0x11C0`| `int`          | `mCargoCount` — number of valid CargoInfo entries  |
| `+0x11C4`| (descriptor)   | `mSoundCargoPickup` — sound played on cargo attach |
| `+0x11D8`| (descriptor)   | `mSoundCargoDropoff` — sound played on cargo detach|

---

## Function Reference

All functions are **thiscall** unless stated otherwise.
The 0x004xxxxx addresses are small thunks (JMP → 0x004Dxxxx body).

### EntityCarrier

| Address    | Symbol                              | Notes                                      |
|------------|-------------------------------------|--------------------------------------------|
| `004D74F0` | `EntityCarrier::EntityCarrier`      | Constructor; delegates to EntityFlyer ctor |
| `004D7DA0` | `EntityCarrier::~EntityCarrier`     | Destructor                                 |
| `004D7090` | `EntityCarrier::ActivatePhysics`    | 20-byte body; enables physics on the carrier|
| `004D7480` | `EntityCarrier::CargoSlot::CargoSlot` | 11-byte trivial ctor; zeros the slot     |
| `004D7490` | `EntityCarrier::CargoSlot::~CargoSlot`| 1-byte (`C3`); trivial no-op dtor        |
| `004D7A80` | `EntityCarrier::GetRtti`            | `A1 DC D4 B7 00 C3` — reads RTTI global  |
| `004D7A90` | `EntityCarrier::GetRttiName`        | `B8 EC A2 A3 00 C3` — returns `"EntityCarrier"` |
| `004D7FE0` | `EntityCarrier::Update`             | Per-frame; iterates slots, dispatches cargo vtable[0x12] |
| `004D8130` | `EntityCarrier::UpdateLandedHeight` | Reads CargoSlot.mOffset.y; computes max hover height |
| `004D81F0` | `EntityCarrier::AttachCargo`        | `(int slotIdx, void* cargo)` — see detail below |
| `004D8350` | `EntityCarrier::DetachCargo`        | `(int slotIdx)` — see detail below        |
| `004D8400` | `EntityCarrier::Kill`               | Calls sub-object Kill then DetachCargo     |

Thunks (same bodies at different addresses, different vtable paths):

| Thunk      | Resolves to             |
|------------|-------------------------|
| `004065EB` | `~EntityCarrier`        |
| `0040B87A` | `EntityCarrier::EntityCarrier` |

### EntityCarrierClass

| Address    | Symbol                                  | Notes                                      |
|------------|-----------------------------------------|--------------------------------------------|
| `004D7E10` | `EntityCarrierClass::EntityCarrierClass`| Ctor; zeroes mCargoInfo[4] loop + sound init|
| `004D71E0` | `EntityCarrierClass::~EntityCarrierClass`| Destructor                                |
| `004D70B0` | `EntityCarrierClass::CargoInfo::CargoInfo`| 3-byte (`8B C1 C3`); trivial ctor returns `this` |
| `004D71C0` | `EntityCarrierClass::GetRtti`           | `A1 84 D4 B7 00 C3` — reads RTTI global  |
| `004D71D0` | `EntityCarrierClass::GetRttiName`       | `B8 FC A2 A3 00 C3` — returns `"EntityCarrierClass"` |
| `004D7210` | `EntityCarrierClass::SetProperty`       | ODF property dispatcher — see detail below |
| `004D7F00` | `EntityCarrierClass::Build`             | Sets up spatial/collision from CargoInfo   |

Thunks:

| Thunk      | Resolves to                    |
|------------|--------------------------------|
| `004069DD` | `EntityCarrierClass` (ctor)    |
| `00410816` | `EntityCarrierClass` (ctor)    |
| `00411EAA` | `~EntityCarrierClass`          |

### EntityFlyer (base class)

| Address    | Symbol                              | Notes                                      |
|------------|-------------------------------------|--------------------------------------------|
| `004fc930` | `EntityFlyer::Update`               | Main update; state machine (0–5); thunked from `0x00412ad0` |
| `004fc730` | `EntityFlyer::SetStateLanded`       | Sets state=0, zeroes velocity, positions on ground |
| `004fc830` | `EntityFlyer::SetStateTakeOff`      | Sets state=2, initial velocity + flight timer |
| `004f1380` | `EntityFlyer::InitiateLanding`      | Sets state=3; called from Lua `EntityFlyerLand` |
| `004f8b70` | (unnamed takeoff)                   | Sets state=1; begins ascent                |
| `00403198` | `EntityFlyerClass::SetProperty`     | ODF property parser; computes mLandedHeight from geometry |

Lua callbacks:

| Address    | Symbol                              | Notes                                      |
|------------|-------------------------------------|--------------------------------------------|
| `00472b00` | `Lua_EntityFlyerSetLanded`          | Forces state=0                             |
| `00472a90` | `Lua_EntityFlyerTakeOff`            | Forces state=2                             |
| `00472a20` | `Lua_EntityFlyerLand`               | Triggers state=3 (initiate landing)        |

### VehicleSpawn

| Address    | Symbol                              | Notes                                      |
|------------|-------------------------------------|--------------------------------------------|
| `00665300` | `VehicleSpawn::CalculateDest`       | Checks carrier state; state=0 → drop cargo, state=2 → destroy |
| `00665a50` | `VehicleSpawn::UpdateSpawn`         | Spawn sequence: cargo + carrier + attach + takeoff |
| `00664c50` | `VehicleSpawn::VehicleSpawn`        | Constructor                                |

### AI Carrier Behavior

| Address    | Symbol                              | Notes                                      |
|------------|-------------------------------------|--------------------------------------------|
| `005af000` | AI carrier goal (6-state FSM)       | Controls entire carrier flight lifecycle   |
| `005aee80` | Get entity from AI goal object      | Returns entity pointer from AI goal param  |
| `004f8b70` | `EntityFlyer::TakeOff`              | Sets state=1 (ASCENDING); called from AI   |
| `00408602` | `thunk_EntityFlyer::TakeOff`        | Thunk → `004f8b70`                         |

Standalone:

| Address    | Symbol              | Notes                                   |
|------------|---------------------|-----------------------------------------|
| `0046C320` | `SetCarrierClass`   | Assigns an EntityCarrierClass to a carrier |

---

## Function Detail

### `EntityCarrierClass::SetProperty` — `004D7210`

Signature: `void __thiscall(EntityCarrierClass* this, uint propHash, const char* propValueStr)`

Dispatches on `propHash`:

| Hash         | Property key          | Behaviour |
|--------------|-----------------------|-----------|
| `0x3E2C4DA4` | `kCargoNodeName`      | Hashes the string via `FUN_007E1C10`, writes `mCargoInfo[mCargoCount].mHash`, looks up offset from an external node table (up to 0x80 entries searched), copies matched `float[3]` into `mCargoInfo[mCargoCount].mOffset`, increments `mCargoCount`. |
| `0x910A89FC` | `kCargoNodeOffset`    | Parses `"%f %f %f"` via `sscanf`, writes `mCargoInfo[mCargoCount]`, increments `mCargoCount`. |
| `0x8897DB28` | `kSoundCargoPickup`   | Forwards args to `mSoundCargoPickup` descriptor at `this+0x11C4`. |
| `0x910A89FC`…| `kSoundCargoDropoff`  | Forwards args to `mSoundCargoDropoff` descriptor at `this+0x11D8`. |
| (fallthrough)| (base class)          | Calls `EntityFlyerClass::SetProperty` at `0x00403198`. |

### `EntityCarrier::AttachCargo` — `004D81F0`

Signature: `void __thiscall(EntityCarrier* this, int slotIdx, void* cargo)`

1. Multiplies `slotIdx × 5` (stride in DWORDs) to address `mCargoSlots[slotIdx]`.
2. **Pre-check**: if `mCargoSlots[slotIdx].mObjectPtr` is non-null and its generation
   still matches, the slot is already occupied — returns immediately without attaching.
3. Otherwise clears the existing stale handle.
4. Reads `this->mClass` (`inner+0x66C`) → `mCargoInfo[slotIdx]` → attach-point offset.
5. Calls `cargo->vtable[0x28](cargo_ptr_arg)` to notify cargo it is being attached
   (no null check before this call — **bug, guarded by our hook**).
6. Computes `slot.mOffset = mCargoInfo[slotIdx].mOffset − cargo_world_pos`
   via FLD/FSUB/FSTP (float bits preserved through integer register copy).
7. Stores `cargo` and `cargo->field_0x204` (generation) into the slot.
8. **Redundant second generation check** — dead code; the equality always holds
   immediately after the store in step 7.
9. Writes back-reference into `cargo + 0x58`.

### `EntityCarrier::DetachCargo` — `004D8350`

Signature: `void __thiscall(EntityCarrier* this, int slotIdx)`

1. Addresses `mCargoSlots[slotIdx]` via `slotIdx × 5` DWORD stride.
2. If `mObjectPtr == null` → no-op return.
3. Validates generation: if `mObjectPtr->field_0x204 != mObjectGen` → stale,
   clears the handle and returns.
4. If valid, zeros `mObjectPtr` and `mObjectGen`.
5. Writes zero to `cargo + 0x58` (clears back-reference).
6. Carries the same **redundant double-validation** pattern as AttachCargo (dead code).

### `EntityCarrier::Update` — `004D7FE0`

Signature: `bool __thiscall(EntityCarrier* this, float deltaTime)`

Iterates `mCargoSlots[0..3]`. For each occupied (live) slot:
- Calls `cargo->vtable[0x12](carrier_this + 0x340)` — notifies cargo of carrier state.

The Ghidra decompiler shows `unaff_EBX + 0x340` here, which appears to be an
uninitialized register bug. **Confirmed not a bug** by disassembly: `[ESP+0x18]`
holds the carrier's `this` pointer saved in the function prologue; `ADD EAX, 0x340`
at `004D80AB` produces `carrier_this + 0x340` correctly.

### `EntityCarrier::Kill` — `004D8400`

Calls the Kill method on the sub-object at `this + 0x140`, then calls
`DetachCargo(this, slotIdx)` with the corrected `this` pointer (subtracts `0x140`
back to reach the EntityCarrier base).

### `EntityCarrierClass::EntityCarrierClass` — `004D7E10`

Zeroes the 64-byte `mCargoInfo[4]` block (4 entries × 16 bytes) via a
DWORD-stepping write loop. Then initialises the two sound description members
(`mSoundCargoPickup`, `mSoundCargoDropoff`).

---

## Bugs Found and Fixed

All fixes live in `PatcherDLL/src/entity_carrier_fixes.cpp`, installed via
Detours in `lua_hooks_install()`.

### Bug 1 — `SetProperty`: `mCargoCount` buffer overflow (critical)

**Location**: `004D7210` — both the `kCargoNodeName` and `kCargoNodeOffset` handlers.

When `mCargoCount == 4`, the write destination becomes:

```
(4 + 0x118) * 0x10 = 0x11C0  →  exactly &mCargoCount itself
```

The 5th write self-corrupts `mCargoCount` with a hash value or float bits.  The
6th write lands at `+0x11C4` = `mSoundCargoPickup`, silently trashing the sound
descriptor.  A malformed ODF with > 4 cargo nodes triggers this every load.

**Fix**: before calling the original, read `mCargoCount` and return early if `>= 4`.

### Bug 2 — `AttachCargo`: No `slotIdx` bounds check

**Location**: `004D81F0` — `slotIdx` is used directly in `slotIdx * 5 * 4`
address arithmetic with no validation.  An out-of-range index walks past
`mCargoSlots[3]` into adjacent struct memory.

**Fix**: `if ((unsigned)slotIdx >= 4) return;`

### Bug 3 — `AttachCargo`: Null `cargo` pointer before vtable call

**Location**: `004D8246` — `CALL dword ptr [EDX + 0xA0]` (vtable[0x28]) is
executed on `cargo` before any null check, crashing immediately on null input.

**Fix**: `if (!cargo) return;`

### Bug 4 — `DetachCargo`: No `slotIdx` bounds check

**Location**: `004D8350` — same arithmetic as AttachCargo, same OOB hazard.

**Fix**: `if ((unsigned)slotIdx >= 4) return;`

### Non-bug observations

- **`AttachCargo` float cast**: Ghidra decompiler shows `(int)(float - float)`,
  implying truncation. Disassembly confirms `FLD / FSUB / FSTP` followed by
  `MOV` of the raw bit pattern — the float value is preserved correctly.
- **`AttachCargo` redundant generation re-check**: The second generation
  check after writing ptr+gen is always true (dead code). Harmless, left alone
  since reimplementing the full function to remove it isn't worth the risk.

---

## ODF Property Reference

Properties parsed by `EntityCarrierClass::SetProperty`:

```
CargoNodeName    = <node_name>          // lookup in node table → mCargoInfo[n].mHash + .mOffset
CargoNodeOffset  = <x> <y> <z>          // direct vec3 → mCargoInfo[n].mOffset
SoundCargoPickup  = <sound_file> ...    // sound descriptor args
SoundCargoDropoff = <sound_file> ...    // sound descriptor args
```

Up to **4 cargo nodes** are supported (`mCargoInfo[4]`).  Additional nodes beyond
the first 4 are silently dropped (enforced by `entity_carrier_fixes`).

---

## VehicleSpawn / vehiclepad Integration

This section covers how the carrier system is driven by the `VehicleSpawn` world entity
and the `VehicleSpawnClass` ODF class.

### ClassLabel: `"vehiclespawn"` vs `"vehiclepad"`

Both registered in `GameState::CreateBaseEntityClasses` (`0x0044d296`):

```cpp
VehicleSpawnClass(hash("vehiclespawn"), isPad=0);  // plain vehicle spawn
VehicleSpawnClass(hash("vehiclepad"),   isPad=1);  // carrier landing pad
```

`VehicleSpawnClass::mIsPad` sits at `VehicleSpawnClass + 0x80`.

`VehicleSpawn::SetProperty_` (mislabeled by Ghidra — `this` is the **world entity**, not the
class) checks this flag before ever setting `UseCarrier`:

```asm
MOV EDX, [EBX + 0x70]    ; VehicleSpawn::mClass (VehicleSpawnClass*)
MOV AL,  [EDX + 0x80]    ; VehicleSpawnClass::mIsPad
TEST AL, AL
JZ   skip_carrier_setup  ; isPad == false → UseCarrier[team] stays false, mFlyerClass[team] = null
```

**Consequence**: if your cargo-spawn entity uses `ClassLabel = "vehiclespawn"`, the carrier
system is completely bypassed regardless of `SetCarrierClass`.  You must use
`ClassLabel = "vehiclepad"`.

### VehicleSpawn world entity struct (from constructor `0x00664c50`)

All offsets from the `VehicleSpawn*` world-entity pointer:

| Offset  | Type              | Field                                      |
|---------|-------------------|--------------------------------------------|
| `+0x70` | `VehicleSpawnClass*` | `mClass`                               |
| `+0x78` | `uint`            | `mSpawnClass hash` (entity class hash)     |
| `+0x80` | `float`           | `mSpawnTime`                               |
| `+0x90` | `EntityClass*[8]` | `mSpawnClass[8]` — cargo class per team    |
| `+0xB0` | `EntityClass*[8]` | `mFlyerClass[8]` — carrier class per team  |
| `+0xD0` | `bool[8]`         | `mUseCarrier[8]`                           |
| `+0xDC` | ptr               | linked list head (spawn-exclusion zones)   |
| `+0xE8` | `int`             | active tracker count                       |
| `+0xEC` | ptr               | `mCarrier` — PblHandle ptr                 |
| `+0xF0` | `uint`            | `mCarrier` — PblHandle generation          |
| `+0xF4` | `int`             | previous spawn team                        |
| `+0xF8` | `int`             | current vehicle team (0-based)             |
| `+0xFC` | `bool`            | spawn timer active flag                    |
| `+0x100`| `float`           | spawn timer countdown                      |

### SetCarrierClass Lua callback (`0x0046C320`)

```lua
SetCarrierClass(teamNumber, "com_fly_vtrans")
```

Writes the carrier class to `SpawnManager::sInstance_->mMaxUnitCount[team-6] + 0x5C`.
`VehicleSpawn::SetProperty_` reads that same `team_entry + 0x5C` to populate
`mFlyerClass[team]`.

**Timing requirement**: must be called in `ScriptInit()` **before** `ReadDataFile("dc:VTR\\VTR.lvl")`
(or whichever file loads the vehiclepad entities), so the carrier class is already
registered when the entity's properties are processed.

### Carrier spawn sequence (`VehicleSpawn::UpdateSpawn`, `0x00665a50`)

When `mUseCarrier[team]` is true and the spawn timer fires:

1. Computes spawn transform (midpoint of vehiclepad's bounding box, using its region).
2. Checks the spawn exclusion zone — returns early if blocked.
3. Spawns the **cargo entity** (e.g. AT-TE): `mSpawnClass[team]->vtable[2](transform)`.
4. Calls `FUN_006646b0(mFlyerClass[team], &spawnTransform)` to **adjust** the carrier spawn
   position (offsets it based on carrier-class bounding box fields at `+0x88c / +0x8ec`).
5. Spawns the **carrier entity** (vtrans): `mFlyerClass[team]->vtable[2](adjustedTransform)`.
6. Calls `EntityCarrier::AttachCargo(carrier, 0, cargo)` — attaches cargo to slot 0.
7. Calls `FUN_004fc830(carrier)` — sets initial velocity (`speed × forward_dir`) and
   thruster effects; **carrier will fly straight in the vehiclepad's facing direction**
   if no AI path takes over.
8. Calls `FUN_004f1380(carrier)` — plays engine sounds, sets flight state, and activates
   the flight AI via `(**(entity+0x0c))->vtable[1]()`.
9. Allocates a `VehicleTracker` for the **cargo** entity (not the carrier) and links it.

### Carrier Spawn Height Calculation

The carrier's initial spawn Y position is computed by `FUN_006646b0`, called in step 4 above.
This function reads `LandingTime`, `LandingSpeed`, and `LandedHeight` from the `EntityFlyerClass`:

```
Y_offset = LandingTime * LandingSpeed * 0.5 + LandedHeight
Z_offset = LandingTime * FlightSpeed * -0.5
```

Where `FlightSpeed` is the carrier's current forward speed. The carrier spawns this far above
the VehiclePad's ground position. In practice:

```
Example: LandingTime = 15, LandingSpeed = 30, LandedHeight = 3
Y_offset = 15 * 30 * 0.5 + 3 = 228 units above the pad
```

After spawning, the carrier's Y is **clamped every frame** by `AIUtil::gMaxFlyHeight`
(global at `0x00b8e9ec`, default `200.0`). The clamp applies as `Y <= gMaxFlyHeight + 0.2`.
This value can be set from Lua via `SetMaxFlyHeight(height)`.

If the computed spawn offset exceeds `gMaxFlyHeight`, the carrier will immediately be pulled
down to the fly ceiling on its first update frame. To get a lower spawn without altering
the landing behaviour, reduce `LandingTime` or `LandingSpeed`, or set `SetMaxFlyHeight`
to the desired ceiling in your mission Lua.

### EntityFlyer State Machine (`primary+0x364`)

EntityCarrier inherits from EntityFlyer.  The thunk at `0x00412ad0` is a plain
`JMP 0x004fc930` with no `this`-pointer adjustment, so `EntityFlyer::Update` and
`EntityCarrier::Update` receive the same ECX value (the **primary base**).
The flight state is at `primary + 0x364`.

| State | Name       | Description                                              |
|-------|------------|----------------------------------------------------------|
| 0     | LANDED     | On the ground; VehicleSpawn::CalculateDest drops cargo   |
| 1     | ASCENDING  | Taking off; rising toward TakeoffHeight                  |
| 2     | FLYING     | In-flight; following AI path or straight-line velocity   |
| 3     | LANDING    | Descending toward the ground; checking landing condition |
| 4     | DYING      | Crash/death sequence; timer counting down                |
| 5     | DEAD       | Crash timer expired; entity ready for cleanup            |

#### State transitions

```
0 (Landed)    → 1 (Ascending)  : FUN_004f8b70 (takeoff button / AI)
1 (Ascending) → 2 (Flying)     : ascent progress >= 1.0 AND altitude > TakeoffHeight + LandedHeight
1 (Ascending) → 3 (Landing)    : throttle threshold exceeded during ascent
2 (Flying)    → 3 (Landing)    : EntityFlyer::InitiateLanding (0x004f1380) or auto-land in Update
3 (Landing)   → 0 (Landed)     : ground_distance < LandedHeight AND surface_normal_dot > 0.9375
3 (Landing)   → 1 (Ascending)  : surface is water / DenyFlyerLand / throttle abort
  any state   → 4 (Dying)      : no passengers, death conditions met
4 (Dying)     → 1 (Ascending)  : re-activated (entities rejoin formation)
4 (Dying)     → 5 (Dead)       : crash timer expires
```

#### State writes in `EntityFlyer::Update` (`0x004fc930`)

| Address      | Value | Context                                                    |
|--------------|-------|------------------------------------------------------------|
| `0x004fd1fd` | 4     | No passengers → dying                                     |
| `0x004fd226` | 1     | Re-activated from dying → ascending                       |
| `0x004fe0e5` | 4     | No carrier spawned → forced death                         |
| `0x004fe848` | 3     | Ascending → landing (progress threshold)                  |
| `0x004fe940` | 2     | Ascending → flying (ascent complete)                      |
| `0x004fe9b8` | 3     | Flying → landing (boundary/timer trigger)                 |
| `0x004fea28` | 1     | Landing → ascending (abort: throttle)                     |
| `0x004febfd` | 1     | Landing → ascending (abort: water/DenyFlyerLand surface)  |
| `0x004fec0b` | **0** | **Landing → landed** (ground reached, surface valid)      |
| `0x005004a7` | 5     | Dying → dead (crash timer expired)                        |

Other functions that write `primary+0x364` (flight state):

| Address      | Function                         | Value | Notes                                |
|--------------|----------------------------------|-------|--------------------------------------|
| `0x004fc730` | `EntityFlyer::SetStateLanded`    | 0     | Zeroes velocity, positions on ground |
| `0x004fc830` | `EntityFlyer::SetStateTakeOff`   | 2     | Sets initial velocity, flight timer  |
| `0x004f1380` | `EntityFlyer::InitiateLanding`   | 3     | Called from Lua `EntityFlyerLand`    |
| `0x004f8b70` | `EntityFlyer::TakeOff`           | 1     | Takes off; called by AI behavior     |

### AI Carrier Behavior State Machine — `005af000`

The carrier's flight lifecycle is controlled by a 6-state AI goal function at `0x005af000`.
This function runs independently of `EntityFlyer::Update` and manages path generation,
destination selection, and flight state transitions. It is called via the AI system between
entity Update frames.

The AI goal object is passed as `param_1`. Key fields:
- `param_1[1]` — current AI state (1–6, used as switch case)
- `param_1[3]` — reference to controller/entity data
- `param_1[0x13]` — VehicleSpawn/destination reference
- `param_1[0x15]` — flag byte (toggled during landing sequence)
- `param_1[0x16]` — timer value

Entity access: `thunk_FUN_005aee80(param_1)` returns the actual entity pointer
(inner/struct base, where `+0x5A4` = flight state, `+0x66C` = class pointer).

#### AI states

| AI State | Name | Behaviour |
|----------|------|-----------|
| 1 | CHECK LANDED | If entity flight state == 0 → clear AI path, return (landing complete). Otherwise → set AI state to 4 (pick destination). |
| 2 | INITIAL TAKEOFF | Creates a 2-point AIPath: current position → 200 units straight ahead. If entity state ≠ 2 (FLYING) → calls `TakeOff` → entity state = 1. |
| 3 | AT DESTINATION | If arrived at destination and flag is set, records timer = `now + 10`. Clears AI path. |
| 4 | PICK NEXT DEST | Picks a **random command post** (`AIUtil::GetRandomInt(0, numCPs-1)`). Loops up to 10 times to find one >100 units away. Sets destination to `CP.position + (0, 20, 0)`. If entity state ≠ 2 → calls `TakeOff` → **entity state = 1 (BUG: interrupts LANDING)**. |
| 5 | LANDING APPROACH | Reads destination from `param_1[0x13]` (VehicleSpawn ref). Adjusts position via `FUN_006646b0` (carrier bbox adjustment). Computes approach direction with `y = -0.2` (slight descent). Creates 3-point AIPath: current → approach point (2× entity radius back) → destination. |
| 6 | HANDLE LANDING | Toggles landing flag. If flag was set, registers landing zone and transitions to AI state 3. Otherwise sets flag, transitions AI to state 3. |

#### AI state flow

```
Spawn → AI state 2 (takeoff) → AI state 4 (pick random CP)
  → fly to CP → AI state 3 (arrived) → AI state 5 (landing approach)
  → AI state 6 (landing) → AI state 3 (at destination)
  → AI state 1 (check landed) → if state==0: cargo drop. else: AI state 4 (cycle)
```

#### Bug: Case 4 interrupts landing

In AI state 4, the check `if (entity_state != 2)` triggers `TakeOff`, which forces
entity state = 1 (ASCENDING). This runs even when the entity is in state 3 (LANDING),
yanking the carrier back up mid-descent. The AI state machine runs between `EntityFlyer::Update`
frames, so the transition is invisible to hooks inside Update.

**Result**: carrier oscillates LANDING → ASCENDING → LANDING indefinitely, never reaching
`gndDist < landedHt`, so it never lands (state 0) and VehicleSpawn never drops cargo.

**Fix**: hook `TakeOff` (`0x004f8b70` / thunk `0x00408602`) to suppress the 3→1
transition. If entity is already in state 3, return early without calling the original.
The existing landing physics will complete the descent naturally.

#### Path generation (not placed splines)

The carrier does **not** use placed spline paths from Zero Editor. The AI behavior generates
its own `AIPath` objects on the fly:
- Case 2: simple 200-unit forward path
- Case 4: single waypoint at random CP + 20 height
- Case 5: 3-point approach path with slight descent angle

The ODF `PathFollower*` properties (`PathFollowerClass`, `PathFollowerBranchPaths`, etc.) may
be consumed by a lower-level path-following system, but the destination selection and path
construction is entirely controlled by `FUN_005af000`.

### Landing condition (state 3 → 0)

The only place state 0 is written in `EntityFlyer::Update` is at `0x004fec0b`.
The condition chain (disassembly at `0x004feb17`–`0x004fec0b`):

```
1. ground_distance + field_0x3c0 < raycast_result   (first proximity check)
2. raycast_distance < field_0x3c0                     (final proximity check)
3. surface_normal_dot > 0.9375                        (surface flatness — cos(~20°))
4. collision node is NULL  OR  NOT (water OR DenyFlyerLand)
```

**`field_0x3c0`** = `primary + 0x3C0` = the **landed height** (hover floor).
This is the distance below which the flyer is considered "on the ground."

If `ground_distance >= field_0x3c0`, landing never completes and the carrier
stays in state 3 (or reverts to 1).  VehicleSpawn::CalculateDest then sees
state ≠ 0 → destroys the carrier instead of dropping cargo.

### `mLandedHeight` — hover floor / landing threshold

**Instance field**: `primary + 0x3C0` (read by EntityFlyer::Update landing condition)

**Note**: `UpdateLandedHeight` writes to `inner + 0x600` (= `primary + 0x840`).  This appears
to be a separate copy — the landing condition reads from `primary + 0x3C0` which is the
EntityFlyer-level field initialized from `EntityFlyerClass + 0x8F4` during construction.

**Class field**: `EntityFlyerClass + 0x8F4`

Computed in `EntityFlyerClass::SetProperty` when `GeometryName` is processed:
```cpp
mLandedHeight = -(*(float*)(redModel + 0x9C));  // = -(bounding_box_min.Y)
```

**NOT an ODF property** — purely derived from the model geometry's bounding box.
If the model's lowest point is at Y = -2.0 in local space, then mLandedHeight = 2.0.

**Instance override**: `EntityCarrier::UpdateLandedHeight` (`0x004D8130`) adjusts
the per-instance value (offsets here use UpdateLandedHeight's ECX = inner base):
```cpp
inner+0x600 = mClass+0x8F4;   // start with class base height  (= primary+0x840)
for each cargo slot:
    cargoHeight = -(cargoOffsetY) - cargoGeometryHeight;
    inner+0x600 = max(inner+0x600, cargoHeight);
```

So the effective landing threshold = `max(carrier_bbox_height, cargo_adjusted_heights)`.

### Carrier flight & drop mechanism

After spawn the carrier follows the world's **AI path network** (flyer paths placed in
Zero Editor).  Without valid AI path nodes, the carrier maintains the straight-line
velocity set in step 7 above indefinitely.

**Normal drop mechanism** — `VehicleSpawn::CalculateDest` (`0x00665300`):
- VehicleSpawn stores a carrier handle at `+0xEC`/`+0xF0` when the carrier reaches
  its destination.
- On subsequent calls, checks `carrier->state` (`primary+0x364`):
  - **State 0** (landed): calls `DetachCargo(0)` — cargo drops at current position.
  - **State 2** (still flying): destroys the carrier (timeout path).
- The carrier must successfully land (state 3 → 0) for cargo to drop.  If it never
  completes landing, VehicleSpawn destroys it.

**Kill-based drop** — `EntityCarrier::Kill` (`004D8400`):
- Kills the sub-object at `this + 0x140`.
- Calls `DetachCargo(this, slotIdx)` for each occupied slot.
- The cargo entity falls / lands at the carrier's current world position.

### Why landing may fail

1. **Carrier never enters state 3**: nothing calls `InitiateLanding` / Lua `EntityFlyerLand`.
   The carrier stays in state 2 forever → VehicleSpawn destroys it.
2. **Terrain slope > ~20°**: the surface normal dot product check (`> 0.9375`) fails.
   Landing aborts and carrier re-ascends (state 1).
3. **Water / DenyFlyerLand surface**: collision model has the deny flag set.
4. **LandedHeight too small**: if `field_0x3c0` is near zero (model bbox bottom at Y≈0),
   the carrier must nearly touch the ground to satisfy the distance check.
5. **External state=1 writes** (confirmed for VehicleSpawn carrier): something outside
   `EntityFlyer::Update` sets state=1 between frames, interrupting the landing approach
   before the carrier reaches `gndDist < landedHt`. This creates a 3→1→3 cycle where the
   carrier oscillates between LANDING (descending ~5-10 units) and ASCENDING (climbing back up),
   never reaching the ground. The source of the external state=1 write is likely `FUN_004f8b70`
   called by the VehicleSpawn/AI path system.

### Required world setup for vtrans carrier

1. **`ClassLabel = "vehiclepad"`** on the cargo/AT-TE VehicleSpawn entity.
2. `SetCarrierClass(team, "com_fly_vtrans")` in `ScriptInit()` before loading the level.
3. **Flyer AI path nodes** in Zero Editor from the vehiclepad → drop zone.  Without these,
   the vtrans flies straight in the vehiclepad's facing direction and never drops cargo.
4. `CargoNodeName = <bone_name>` in `com_fly_vtrans.odf` — names the bone in the vtrans
   geometry where the AT-TE is suspended.  Without it `mCargoCount = 0` and the AT-TE
   attaches at the carrier's geometric centre (`mCargoInfo[0].mOffset = 0`).

### Key EntityFlyer / EntityFlyerClass fields

All offsets use the **primary base** (= ECX in Update / EntityFlyer::Update).
Inner-base equivalents = primary offset − 0x240.

| Primary offset | Type    | Field                                    |
|----------------|---------|------------------------------------------|
| `+0x364`       | int     | Flight state (0–5)                       |
| `+0x368`       | float   | Takeoff/landing progress (0.0–1.0)       |
| `+0x340`       | Vec3    | Velocity vector                          |
| `+0x358`       | float   | Speed magnitude                          |
| `+0x3C0`       | float   | LandedHeight (landing threshold)         |
| `+0x3C4`       | float   | Flight timer                             |
| `+0x8AC`       | ptr     | EntityFlyerClass* / EntityCarrierClass*  |

| Offset (class)  | Type    | Field                                          |
|------------------|---------|-------------------------------------------------|
| `+0x884`         | float   | Gravity (applied per-frame as velocity change)  |
| `+0x888`         | float   | Gravity (alternate, used when boosting)         |
| `+0x88C`         | float   | MinSpeed / FlightSpeed (ODF `MinSpeed`)         |
| `+0x890`         | float   | MidSpeed / MidFlyerSpeed (ODF `MidSpeed`)       |
| `+0x894`         | float   | MaxSpeed / MaxFlyerSpeed (ODF `MaxSpeed`)        |
| `+0x898`         | float   | BoostSpeed (ODF `BoostSpeed`)                   |
| `+0x8E0`         | float   | TakeoffHeight (ODF property)                   |
| `+0x8E4`         | float   | TakeoffTime (ODF property)                     |
| `+0x8E8`         | float   | TakeoffSpeed (ODF property)                    |
| `+0x8EC`         | float   | LandingTime (ODF property)                     |
| `+0x8F0`         | float   | LandingSpeed (ODF property)                    |
| `+0x8F4`         | float   | mLandedHeight = -(model_bbox_Y_min), NOT ODF   |
| `+0x1118`        | byte    | Flags; bit 0 = is carrier class; bit 1 = auto-land capable |

### `FUN_004fc830` / `SetStateTakeOff` field reference

All offsets use the **primary base** (ECX as received by SetStateTakeOff).

| Offset (primary) | Value set       | Notes                                        |
|------------------|-----------------|----------------------------------------------|
| `+0x378..C`      | `gVectorZero`   | clears stored velocity                       |
| `+0x080`         | `-4.0f`         | initial Y-velocity / hover offset            |
| `+0x340..8`      | `speed × fwd`   | initial flight velocity vector               |
| `+0x358`         | `speed`         | flight speed magnitude                       |
| `+0x35C..0`      | zeros           | clears extra velocity state                  |
| `+0x364`         | `2`             | flight mode state = FLYING                   |
| `+0x3C4`         | `15.0f`         | internal timer (`0x41700000`)                |
| `+0x368`         | `1.0f`          | takeoff progress = complete                  |

Speed source: `FUN_004f0a10(entity)` reads `mClass + 0x88c` (EntityFlyerClass flight speed).

---

## ODF Flight Parameters — Detailed Behaviour

All five flight parameters are set in the carrier's ODF (e.g. `com_fly_vtrans.odf`) and stored
in the `EntityFlyerClass` struct. `EntityFlyer::Update` reads them from the class pointer at
`primary+0x42C`.

### `TakeoffHeight` — cls+0x8E0

**ODF**: `TakeoffHeight = 2`

Used in two contexts:

1. **Vertical velocity scaling** (cases 1 and 3, second switch):
   ```
   scale = clamp(gndDist / TakeoffHeight, 0.0, 1.0)
   vertical_speed = (scale * 0.875 + 0.125) * TakeoffSpeed_or_LandingSpeed
   ```
   With `TakeoffHeight=2`, the scale maxes out at 1.0 whenever `gndDist > 2`. This means the
   carrier gets full vertical speed almost immediately. A larger TakeoffHeight would create a
   gradual ramp-up as the carrier gets further from the ground.

2. **Ascending progress gate** (case 1, first switch):
   ```
   if (gndDist > TakeoffHeight + landedHt) {
       progress += dt / TakeoffTime;
   }
   ```
   Progress only increases while the carrier is above `TakeoffHeight + landedHt`. With
   `TakeoffHeight=2` and `landedHt≈0.77`, this gate is always open (carrier is at gndDist≈25).

**Player flyer vs carrier**: for a player-controlled flyer, this is the height at which the
pilot's turn controls ramp up to full authority. For the VehicleSpawn carrier, the value is
irrelevant because gndDist always far exceeds it.

### `TakeoffTime` — cls+0x8E4

**ODF**: `TakeoffTime = 215`

Used only in case 1 (ASCENDING), first switch:
```
progress += dt / TakeoffTime
if (progress >= 1.0) → state = 2 (FLYING)
```

Controls how long ASCENDING takes to complete. At 215 seconds, the carrier needs **3.5 minutes**
of uninterrupted ascending to reach state=2 (FLYING). This is intentionally extreme — the carrier
is never meant to reach FLYING on its own. VehicleSpawn externally sets state=2 via the
`EntityFlyerTakeOff` Lua callback.

**Player flyer vs carrier**: a player flyer typically uses a short TakeoffTime (1-3 seconds) so
the player quickly reaches flight altitude. The carrier uses 215 because VehicleSpawn manages the
lifecycle externally — TakeoffTime is effectively a "safety timeout."

### `TakeoffSpeed` — cls+0x8E8

**ODF**: `TakeoffSpeed = 12.0`

Used in case 1 (ASCENDING), second switch (velocity computation):
```
upward_velocity.y = (scale * 0.875 + 0.125) * TakeoffSpeed
velocity = Interpolate_(upward_vec, forward_velocity, progress)
```

Controls the **upward climb rate** during ASCENDING. The carrier rises at up to 12 units/sec.
The actual rate is blended by `Interpolate_` — at `progress=0.6`, about 40% of the upward
velocity applies (~4.8 u/s effective climb rate).

**Player flyer vs carrier**: same role for both — the speed at which the vehicle rises after
leaving the ground.

### `LandingTime` — cls+0x8EC

**ODF**: `LandingTime = 10`

Used in case 3 (LANDING), first switch:
```
progress -= dt / LandingTime
```

Controls how long the landing approach takes. Progress decreases from its current value toward 0
over `LandingTime` seconds. As progress decreases, the `Interpolate_` in the velocity switch
shifts more weight toward the downward vector, causing the carrier to descend faster.

**Player flyer vs carrier**: for a player flyer, this controls the graceful descent duration.
For the carrier, same physics apply, but the carrier gets interrupted by external state changes
(3→1) before completing the approach.

### `LandingSpeed` — cls+0x8F0

**ODF**: `LandingSpeed = 30.0`

Used in case 3 (LANDING), second switch (velocity computation):
```
downward_velocity.y = -((scale * 0.875 + 0.125) * LandingSpeed)
velocity = Interpolate_(downward_vec, forward_velocity, progress)
```

Controls the **maximum descent rate** during LANDING. With `LandingSpeed=30`, the carrier can
descend at up to 30 units/sec. However, the `Interpolate_` blending with `progress` means the
effective initial descent rate is much lower. At `progress=0.8`, only ~20% of the downward
velocity applies (~6 u/s effective descent rate). As progress approaches 0, the full 30 u/s
descent rate is applied.

**Player flyer vs carrier**: same role for both. However, the carrier never reaches the low
progress values where the full descent rate kicks in, because external code interrupts the
landing by setting state=1 (ASCENDING).

### Why these parameters behave differently for EntityCarrier

The fundamental difference is that an EntityCarrier is **VehicleSpawn-controlled**, not
player-controlled:

| Aspect | Player-controlled flyer | VehicleSpawn carrier |
|--------|------------------------|---------------------|
| State driver | Player input + physics | VehicleSpawn Lua callbacks |
| pilot flag | `(field_0x44 & 3) == 3` (YES) | `(field_0x44 & 3) != 3` (NO) |
| State 1→3 | Auto-transitions when `pilot=YES && progress > 0.75` | Never auto-transitions; only via external `EntityFlyerLand` Lua call |
| State 2→3 | Player presses brake (`field_0x3bc > 0.99`) | Never auto-transitions with `pilot=NO`; only via external `EntityFlyerLand` |
| State 3→0 | `gndDist < landedHt && normal > 0.9375` | Same condition, but carrier rarely reaches it |
| State 3→1 | Only if surface is water/denied, or pilot bails (`progress < 0.05`) | **External code sets state=1 between Update frames** (confirmed: no TRANSITION log from within Update) |

**Critical finding**: the 3→1 state change for the carrier happens **outside** of
`EntityFlyer::Update`. The diagnostic hook captures state before and after calling
`original_CarrierUpdate` — no transition is detected within the Update call. This means
VehicleSpawn or another system (likely `FUN_004f8b70` at address `0x004f8b70`, the unnamed
takeoff function) directly writes `state=1` between frames. This creates the observed
LANDING→ASCENDING→LANDING cycle.

---

## ODF Flight Parameter Reference

These five ODF properties are stored as consecutive floats in `EntityFlyerClass`:

| ODF Property    | Class Offset | Description                              |
|-----------------|-------------|------------------------------------------|
| `TakeoffHeight` | `+0x8E0`   | Altitude threshold for speed scaling and progress gate |
| `TakeoffTime`   | `+0x8E4`   | Duration of ASCENDING→FLYING transition  |
| `TakeoffSpeed`  | `+0x8E8`   | Vertical ascent rate                     |
| `LandingTime`   | `+0x8EC`   | Duration of FLYING→LANDED transition     |
| `LandingSpeed`  | `+0x8F0`   | Vertical descent rate                    |
| `LandedHeight`  | `+0x8F4`   | Ground-contact threshold (auto-computed from model bbox if not set) |

### `TakeoffHeight`

Controls two things during ASCENDING (state 1):

1. **Speed ramp** — vertical speed scales with altitude:
   ```
   heightRatio = clamp(groundDistance / TakeoffHeight, 0, 1)
   speed = (heightRatio * 0.875 + 0.125) * TakeoffSpeed
   ```
   Example: `TakeoffHeight = 20`, `TakeoffSpeed = 10`
   - At ground level: `0.125 * 10 = 1.25 u/s` (slow start)
   - At 10 units up: `(0.5 * 0.875 + 0.125) * 10 = 5.625 u/s`
   - At 20+ units up: `1.0 * 10 = 10.0 u/s` (full speed)

2. **Progress gate** — the ascending timer only ticks while above this height:
   ```
   if (groundDistance > TakeoffHeight + LandedHeight)
       progress += dt / TakeoffTime
   ```
   Example: `TakeoffHeight = 20`, `LandedHeight = 3` — progress only advances
   once the carrier is 23+ units above ground.

Also used as the denominator for the same speed ramp during LANDING (state 3).

### `TakeoffTime`

How long the ASCENDING phase lasts after the carrier clears `TakeoffHeight`:
```
progress += dt / TakeoffTime
if (progress >= 1.0) → state = FLYING
```

Example: `TakeoffTime = 5` — after reaching TakeoffHeight, the carrier stays
in ASCENDING for 5 more seconds before transitioning to FLYING.

For VehicleSpawn carriers, this is typically set very high (e.g. 200+) because
VehicleSpawn manages the state externally. The carrier never reaches FLYING on its own;
`TakeoffTime` acts as a safety timeout.

### `TakeoffSpeed`

Maximum vertical ascent rate in units/second during ASCENDING:
```
speed = (heightRatio * 0.875 + 0.125) * TakeoffSpeed
```

Example: `TakeoffSpeed = 15` — the carrier rises at up to 15 u/s. The `heightRatio`
ramp means it starts slow (1.875 u/s) and reaches full speed at `TakeoffHeight`.

### `LandingTime`

How long the descent approach takes during LANDING (state 3):
```
progress -= dt / LandingTime
```

Example: `LandingTime = 8` — progress decreases from 1.0 toward 0 over 8 seconds.
As progress decreases, velocity shifts from forward flight toward pure downward descent.

### `LandingSpeed`

Maximum vertical descent rate in units/second during LANDING:
```
speed = -((heightRatio * 0.875 + 0.125) * LandingSpeed)
```

Example: `LandingSpeed = 25` — the carrier descends at up to 25 u/s. Like
TakeoffSpeed, the heightRatio ramp reduces speed near the ground.

### Dropoff Animation Interaction

The DLL hooks the render function (`FUN_004f6970`) to swap the takeoff animation
(`class+0x87c`) with a "dropoff" animation looked up from the animation bank
(`class+0x878`) via `ZephyrAnimBank::Find(bank, "dropoff")`.

The render function computes the displayed animation frame as:
```
frame = (numFrames - 1) * progress
```

Where `progress` is the flight transition float at `inner+0x5A8`. During ASCENDING,
the state machine drives this value from 0.0 toward 1.0. The dropoff animation plays
in sync with this progression.

**How ODF parameters affect the dropoff animation:**

| Parameter       | Effect on dropoff animation                           |
|-----------------|-------------------------------------------------------|
| `TakeoffHeight` | Higher = longer speed ramp = carrier rises slower near ground, giving more time for early animation frames |
| `TakeoffSpeed`  | Lower = slower ascent = more time for the animation to play during ascending |
| `TakeoffTime`   | No direct effect on animation speed (only controls ASCENDING→FLYING transition timer, not the progress float used by the render) |

The DLL overrides the progress float in the render hook with a time-based value
(`elapsed / duration`, where `duration = numFrames / 30.0`), so the animation always
plays at 30fps regardless of ascent speed. The animation plays for its full duration
(~2 seconds for 61 frames) even if the carrier has already transitioned to FLYING.

**Tuning example:**
```ini
TakeoffSpeed  = 10     ; ascent rate — carrier rises ~10 u/s
TakeoffHeight = 20     ; speed ramp zone — ~2s to clear at full speed
```
With these values, the carrier spends ~2 seconds visibly ascending near the ground,
during which the dropoff animation (also ~2 seconds) plays completely. The visual
result: cargo drops, doors/mechanism animate open while the carrier rises, then it
flies away normally.

---

## Collision Primitive Generation

EntityFlyerClass (and by inheritance EntityCarrierClass) generates collision primitives
during ODF property processing. The collision model is built from the mesh geometry and
has a fallback path that creates a bounding-volume approximation when no explicit collision
shapes are defined in the `.msh` file.

### `EntityFlyerClass::SetProperty` — `004FA310`

When the `GeometryName` property is processed, `SetProperty` loads the referenced model
and iterates its mesh hierarchy looking for **`p_` prefixed shapes** (collision primitives).
These are the standard naming convention for collision geometry in BF2 mesh files
(e.g. `p_hull`, `p_wing`, `p_body`).

### Primitive type system

Collision primitives use a type enum:

| Type | Shape              |
|------|--------------------|
| 0    | Sphere             |
| 2    | Box                |
| 3    | Capsule / Cylinder |
| 4    | Cylinder           |

### Auto-generated fallback — "main_body"

If the mesh contains **zero** `p_` primitives after scanning, `SetProperty` creates a
synthetic fallback collision primitive derived from the model's axis-aligned bounding box:

1. Reads the model AABB: `min` at `model+0x98`, `max` at `model+0xA4`.
2. Computes half-extents: `half_X = (max.X - min.X) / 2`, `half_Y = (max.Y - min.Y) / 2`,
   `half_Z = (max.Z - min.Z) / 2`.
3. Derives capsule dimensions:
   - `radius = (half_X + half_Z) * 0.5`
   - `height = half_Y * 2.0`
4. Creates a **type 3** primitive (capsule/cylinder) named `"main_body"`.

**Important consequence**: removing all `p_` collision shapes from a `.msh` file does
**not** remove collision from the entity. Instead, the engine falls back to this
auto-generated capsule, which is often a worse fit than hand-authored collision primitives.
The fallback capsule is a rough cylinder/capsule that encloses the entire model bounding box,
so it will be larger and less precise than purpose-built collision shapes.

### ODF-defined collision (alternative to mesh primitives)

Collision can also be specified directly in the ODF via properties such as:

- `VehicleCollision` — vehicle-to-world collision shape
- `OrdnanceCollision` — projectile hit collision shape
- `SoldierCollision` — soldier interaction collision shape

These are processed by the base class property handlers and are independent of the
mesh-based `p_` primitive system.

### Flying collision model — class offset `+0x728`

`EntityFlyerClass` maintains a separate `mFlyingCollisionModel` at class offset `+0x728`.
This is distinct from the standard collision model and is used specifically during flight.
The standard collision model (built from `p_` shapes or the `main_body` fallback) and the
flying collision model may differ, allowing entities to have different collision behavior
on the ground versus in the air.
