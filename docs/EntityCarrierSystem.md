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
| `mClass` pointer at `this+0x66C`                               | ✅ Confirmed |
| `mCargoSlots[4]` at `this+0x1DD0`, stride `0x14`              | ✅ Confirmed |
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

Defined as a Ghidra struct. Four of these live at `EntityCarrier + 0x1DD0`.

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

| Offset   | Type                   | Field                                      |
|----------|------------------------|--------------------------------------------|
| `+0x66C` | `EntityCarrierClass*`  | `mClass` — pointer to the class descriptor |
| `+0x1DD0`| `CargoSlot[4]`         | `mCargoSlots`                              |
| `+0x1DDC`| `void*`                | `mCargoSlots[0].mObjectPtr` (derived)      |
| `+0x1DE0`| `int`                  | `mCargoSlots[0].mObjectGen` (derived)      |

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
4. Reads `this->mClass` (`+0x66C`) → `mCargoInfo[slotIdx]` → attach-point offset.
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

### EntityFlyer State Machine (`+0x5A4`)

EntityCarrier inherits from EntityFlyer.  The flight state is stored at `struct_base + 0x5A4`.
In `EntityFlyer::Update` (actual body at `0x004fc930`), `this` = `struct_base + 0x240`,
so the state field is accessed as `this + 0x364`.

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

Other functions that write `+0x5A4`:

| Address      | Function                         | Value | Notes                                |
|--------------|----------------------------------|-------|--------------------------------------|
| `0x004fc730` | `EntityFlyer::SetStateLanded`    | 0     | Zeroes velocity, positions on ground |
| `0x004fc830` | `EntityFlyer::SetStateTakeOff`   | 2     | Sets initial velocity, flight timer  |
| `0x004f1380` | `EntityFlyer::InitiateLanding`   | 3     | Called from Lua `EntityFlyerLand`    |
| `0x004f8b70` | (unnamed takeoff)                | 1     | Begins ascent                        |

### Landing condition (state 3 → 0)

The only place state 0 is written in `EntityFlyer::Update` is at `0x004fec0b`.
The condition chain (disassembly at `0x004feb17`–`0x004fec0b`):

```
1. ground_distance + field_0x3c0 < raycast_result   (first proximity check)
2. raycast_distance < field_0x3c0                     (final proximity check)
3. surface_normal_dot > 0.9375                        (surface flatness — cos(~20°))
4. collision node is NULL  OR  NOT (water OR DenyFlyerLand)
```

**`field_0x3c0`** = `struct_base + 0x600` = the **landed height** (hover floor).
This is the distance below which the flyer is considered "on the ground."

If `ground_distance >= field_0x3c0`, landing never completes and the carrier
stays in state 3 (or reverts to 1).  VehicleSpawn::CalculateDest then sees
state ≠ 0 → destroys the carrier instead of dropping cargo.

### `mLandedHeight` — hover floor / landing threshold

**Instance field**: `struct_base + 0x600` (= `EntityFlyer::Update this + 0x3c0`)

**Class field**: `EntityFlyerClass + 0x8F4`

Computed in `EntityFlyerClass::SetProperty` when `GeometryName` is processed:
```cpp
mLandedHeight = -(*(float*)(redModel + 0x9C));  // = -(bounding_box_min.Y)
```

**NOT an ODF property** — purely derived from the model geometry's bounding box.
If the model's lowest point is at Y = -2.0 in local space, then mLandedHeight = 2.0.

**Instance override**: `EntityCarrier::UpdateLandedHeight` (`0x004D8130`) adjusts
the per-instance value:
```cpp
entity+0x600 = mClass+0x8F4;   // start with class base height
for each cargo slot:
    cargoHeight = -(cargoOffsetY) - cargoGeometryHeight;
    entity+0x600 = max(entity+0x600, cargoHeight);
```

So the effective landing threshold = `max(carrier_bbox_height, cargo_adjusted_heights)`.

### Carrier flight & drop mechanism

After spawn the carrier follows the world's **AI path network** (flyer paths placed in
Zero Editor).  Without valid AI path nodes, the carrier maintains the straight-line
velocity set in step 7 above indefinitely.

**Normal drop mechanism** — `VehicleSpawn::CalculateDest` (`0x00665300`):
- VehicleSpawn stores a carrier handle at `+0xEC`/`+0xF0` when the carrier reaches
  its destination.
- On subsequent calls, checks `carrier->state` (`+0x5A4`):
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

### Required world setup for vtrans carrier

1. **`ClassLabel = "vehiclepad"`** on the cargo/AT-TE VehicleSpawn entity.
2. `SetCarrierClass(team, "com_fly_vtrans")` in `ScriptInit()` before loading the level.
3. **Flyer AI path nodes** in Zero Editor from the vehiclepad → drop zone.  Without these,
   the vtrans flies straight in the vehiclepad's facing direction and never drops cargo.
4. `CargoNodeName = <bone_name>` in `com_fly_vtrans.odf` — names the bone in the vtrans
   geometry where the AT-TE is suspended.  Without it `mCargoCount = 0` and the AT-TE
   attaches at the carrier's geometric centre (`mCargoInfo[0].mOffset = 0`).

### Key EntityFlyer / EntityFlyerClass fields

| Offset (struct_base) | Offset (Update this) | Type    | Field                                |
|----------------------|----------------------|---------|--------------------------------------|
| `+0x120`             | —                    | float   | World position X (matrix row 3)      |
| `+0x124`             | —                    | float   | World position Y                     |
| `+0x128`             | —                    | float   | World position Z                     |
| `+0x5A4`             | `+0x364`             | int     | Flight state (0–5)                   |
| `+0x5A8`             | `+0x368`             | float   | Takeoff/landing progress (0.0–1.0)   |
| `+0x580`             | `+0x340`             | Vec3    | Velocity vector                      |
| `+0x598`             | `+0x358`             | float   | Speed magnitude                      |
| `+0x600`             | `+0x3C0`             | float   | LandedHeight (landing threshold)     |
| `+0x604`             | `+0x3C4`             | float   | Flight timer                         |
| `+0x66C`             | `+0x42C`             | ptr     | EntityFlyerClass* / EntityCarrierClass*|

| Offset (class)  | Type    | Field                                          |
|------------------|---------|-------------------------------------------------|
| `+0x88C`         | float   | Flight speed                                   |
| `+0x8E0`         | float   | TakeoffHeight (ODF property)                   |
| `+0x8F4`         | float   | mLandedHeight = -(model_bbox_Y_min), NOT ODF   |
| `+0x1118`        | byte    | Flags; bit 1 = is carrier class                |

### `FUN_004fc830` / `SetStateTakeOff` field reference

| Offset (struct)  | Value set       | Notes                                        |
|------------------|-----------------|----------------------------------------------|
| `+0x5B8..C`      | `gVectorZero`   | clears stored velocity                       |
| `+0x2C0`         | `-4.0f`         | initial Y-velocity / hover offset            |
| `+0x580..8`      | `speed × fwd`   | initial flight velocity vector               |
| `+0x598`         | `speed`         | flight speed magnitude                       |
| `+0x59C..0`      | zeros           | clears extra velocity state                  |
| `+0x5A4`         | `2`             | flight mode state = FLYING                   |
| `+0x604`         | `15.0f`         | internal timer (`0x41700000`)                |
| `+0x5A8`         | `1.0f`          | takeoff progress = complete                  |

Speed source: `FUN_004f0a10(entity)` reads `mClass + 0x88c` (EntityFlyerClass flight speed).
