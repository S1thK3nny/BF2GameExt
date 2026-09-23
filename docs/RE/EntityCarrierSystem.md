# EntityCarrier System

How the engine runs a carrier (a flyer that brings a vehicle to a `vehiclepad` and drops
it), and what `PatcherDLL/src/entity/flyer_carrier_fixes.cpp` changes. Addresses are
unrelocated (imagebase `0x400000`). "modtools" is the MemExt debug build; "release" means
Steam and GOG, which share one layout and, for this subsystem, the same code VAs.

---

## 1. Overview

```
VehicleSpawn::UpdateSpawn   spawn cargo + carrier, AttachCargo(0), InitAsFlying, Land
          │                 → carrier is born in LANDING, behind and above the pad
          ▼
EntityFlyer::Update         LANDING → LANDED when the ground check passes
          ▼
VehicleSpawn::UpdateLive    LANDED → DetachCargo(0) + TakeOff
          ▼
EntityFlyer::Update         TAKEOFF → FLYING when the climb completes
          ▼
VehicleSpawn::UpdateLive    FLYING → vtable[3](1): the carrier is DELETED
```

- The carrier never flies a path to the pad. It is spawned already descending.
- The pad handles one carrier at a time (`VehicleSpawn::mCarrier`).
- Only cargo slot 0 is ever dropped by the engine.
- A finished carrier is deleted through its scalar deleting destructor, not killed.
  `~EntityCarrier` does not detach cargo.

---

## 2. Pointer layout

EntityCarrier is multiply inherited, so different methods receive different `this`
pointers into the same object. All instance offsets in this document are from the
**object base** (`[base]` = EntityCarrier vtable) unless stated otherwise.

| Sub-object      | `this` =       | Methods |
|-----------------|----------------|---------|
| object base     | `base`         | `AttachCargo`, `DetachCargo`, `UpdateLandedHeight`, `EntityFlyer::TakeOff`, `EntityFlyer::Land` |
| RedSceneObject  | `base + 0x94`  | `EntityFlyer::Render` |
| Damageable      | `base + 0x140` | `EntityCarrier::Kill` (calls `DetachCargo(this - 0x140, slot)`) |
| Controllable    | `base + 0x240` | `EntityCarrier::Update`, `EntityFlyer::Update` |

Proof: cargo slot 0's object pointer is read at `ECX+0x1DDC` in AttachCargo,
`ECX+0x1C9C` in Kill and `ECX+0x1B9C` in Update (modtools).

Class hierarchy: `EntityEx` → `EntityFlyer` → `EntityCarrier`. The class side is
`EntityFlyerClass` → `EntityCarrierClass`.

---

## 3. Functions

| modtools   | Steam      | Function | Notes |
|------------|------------|----------|-------|
| `004D7210` | `004976B0` | `EntityCarrierClass::SetProperty` | cargo nodes + sounds, see 6 |
| `004D81F0` | `00497300` | `EntityCarrier::AttachCargo(int slot, GameObject*)` | `bool`, RET 8. Release copy ignores `slot` (always 0) |
| `004D8350` | `00497410` | `EntityCarrier::DetachCargo(int slot)` | `bool`, RET 4 |
| `004D7FE0` | `004971D0` | `EntityCarrier::Update(float)` | `this` = base+0x240, RET 4 |
| `004D8400` | `00497110` | `EntityCarrier::Kill()` | `this` = base+0x140 |
| `004D8130` | `004974B0` | `EntityCarrier::UpdateLandedHeight()` | |
| `004F8B70` | `004B3C60` | `EntityFlyer::TakeOff()` | LANDED/LANDING → TAKEOFF |
| `004F1380` | `004B3D50` | `EntityFlyer::Land()` | FLYING/TAKEOFF/LANDING → LANDING |
| `004FC930` | `004AC460` | `EntityFlyer::Update(float)` | flight state machine |
| `004F6970` | `004AB040` | `EntityFlyer::Render` | `this` = base+0x94 |
| `00665A50` | `0066F370` | `VehicleSpawn::UpdateSpawn(float)` | release: dt in **XMM1**, bare RET |
| `00665300` | `0066ECF0` | `VehicleSpawn::UpdateLive(float, VehicleTracker*)` | was mislabelled `EntityFlyer::CalculateDest` |
| `0046C320` |            | Lua `SetCarrierClass` | |

Vtables: EntityCarrier modtools `0x00A3A670`, Steam `0x0079A34C`, GOG `0x0079B2EC`.
Slot 3 = scalar deleting destructor, 5 = activate, 36 = SetTeam, 41 = ActivatePhysics
(identical indices on modtools and Steam).

---

## 4. Layouts

### 4.1 Structs

```cpp
struct CargoSlot {            // EntityCarrier::mCargoArray[4], stride 0x14
   PblVector3 mOffset;        // +0x00 offset from the carrier origin
   void*      mObjectPtr;     // +0x0C PblHandle pointer
   int        mObjectGen;     // +0x10 PblHandle id, valid while == cargo+0x204
};
struct CargoInfo {            // EntityCarrierClass::mCargoInfo[4], stride 0x10
   uint32_t   mHash;          // +0x00 CargoNodeName hash, 0 for CargoNodeOffset
   PblVector3 mOffset;        // +0x04
};
```

`EntityCarrierClass::mCargoCount` directly follows `mCargoInfo[3]`, so writing entry 4
overwrites the count itself (see 9.1). The instance copies the class count in its
constructor.

### 4.2 Per-build offsets

Instance fields of EntityFlyer/EntityCarrier in the 0x5xx..0x1Dxx range sit 0x40 lower on
release; EntityFlyerClass fields 0xC8 lower; EntityCarrierClass cargo fields 0xE0 lower.
Fields below ~0x200 are unchanged. Values were read from each build's own disassembly
unless marked *inferred*; the code keeps them in `CarrierLayout`.

| Field | modtools | release |
|-------|----------|---------|
| **EntityCarrierClass** `mCargoInfo[4]` | `0x1180` | `0x10A0` |
| `mCargoCount` | `0x11C0` | `0x10E0` |
| `mSoundCargoPickup` / `mSoundCargoDropoff` | `0x11C4` / `0x11D8` | |
| **EntityFlyerClass** takeoff anim (ZephyrAnim*, nFrames at +8) | `0x87C` | `0x7B4` |
| `MinSpeed` | `0x88C` | |
| `TakeoffHeight` / `TakeoffTime` / `TakeoffSpeed` | `0x8E0` / `0x8E4` / `0x8E8` | `0x818` / `0x81C` / `0x820` |
| `LandingTime` / `LandingSpeed` | `0x8EC` / `0x8F0` | `0x824` / `0x828` *inferred* |
| `mLandedHeight` (= -model bbox min Y, not an ODF key) | `0x8F4` | `0x82C` |
| weapon count (aimers) / passenger count | `0xD48` / `0xE14` | `0xC80` / `0xD4C` |
| **Instance** `mFlightState` | `0x5A4` | `0x564` |
| takeoff/landing `progress` | `0x5A8` | `0x568` |
| landed height (incl. cargo) | `0x600` | `0x5C0` |
| `mClass` | `0x66C` | `0x62C` |
| passengers / turrets / turret count / aimers | `0x670` / `0x680` / `0x6A0` / `0x6A8` | `0x630` / `0x640` / `0x660` / `0x668` |
| ground distance | `0x6D0` | `0x690` |
| anim ref used by the render (nFrames source) | `0x1870` | `0x1830` |
| net anim delta | `0x1D00` | `0x1CC0` |
| post-collision sub-object | `0x1D10` | `0x1CD0` |
| `mCargoArray[4]` | `0x1DD0` | `0x1D90` |

Build-invariant: matrix at `+0xF0` (rows right, up, forward, position at `+0x120`),
scene bounding sphere `+0xC4..+0xD0`, PblHandle id `+0x204`, team bits `+0x234`
(bits 4-7 team, 8-11 perceived team).

### 4.3 VehicleSpawn (build-invariant)

| Offset | Field |
|--------|-------|
| `+0x30`  | `mMatrix` (pad transform) |
| `+0x70`  | `mClass` (VehicleSpawnClass*, `mIsPad` at class `+0x80`) |
| `+0x7C`  | `mSpawnCount` |
| `+0x80`  | `mSpawnTime` |
| `+0x90`  | `mSpawnClass[8]` (cargo class per team) |
| `+0xB0`  | `mFlyerClass[8]` (carrier class per team) |
| `+0xD0`  | `mUseCarrier[8]` |
| `+0xD8`  | `mTrackerList` (count at `+0xE8`) |
| `+0xEC`  | `mCarrier` (PblHandle, id at `+0xF0`) |
| `+0xF4`  | `mVehicleTeam` |
| `+0xF8`  | `mSpawnTeam` |

---

## 5. Lifecycle in detail

### 5.1 Setup

- The cargo spawn must use `ClassLabel = "vehiclepad"`. `VehicleSpawn::SetProperty` only
  sets `mUseCarrier` when `VehicleSpawnClass::mIsPad` is set, so a `"vehiclespawn"`
  bypasses the carrier system entirely.
- `SetCarrierClass(team, "com_fly_vtrans")` must run in `ScriptInit()` **before** the
  `ReadDataFile` that loads the pads, so the class is known when their properties are read.

### 5.2 Spawn (`VehicleSpawn::UpdateSpawn`)

1. Returns immediately while `mCarrier` is a live handle (one carrier per pad).
2. Returns and retries in 1 s if the EntityCarrier memory pool is full.
3. Spawn matrix = the pad matrix, dropped onto the ground with a 50-unit ray.
4. `gCalcBeginLandPos` moves it in the **pad's local frame**:
   `Y += LandingTime * LandingSpeed / 2 + LandedHeight`,
   `Z -= LandingTime * MinSpeed / 2`.
   The carrier starts behind the pad and approaches along the pad's +Z, so **the pad's
   facing direction is the approach corridor**.
5. Cargo is spawned; the carrier is spawned `Y +=` cargo bbox height higher.
6. Carrier: SetTeam, team bits, activate, `AttachCargo(0, cargo)`, `InitAsFlying`
   (FLYING, velocity = MinSpeed forward), then `Land()` immediately (LANDING).
7. Cargo: team, perceived team unless available to any team, activate, then a
   `VehicleTracker` is appended for the **cargo**. `mVehicleTeam = mSpawnTeam`.

After spawning, the carrier's Y is clamped to `AIUtil::gMaxFlyHeight` (default 200, Lua
`SetMaxFlyHeight`), so a spawn height above that is pulled down on the first frame.

### 5.3 Descent and landing (`EntityFlyer::Update`, state 3)

- `progress -= dt / LandingTime` (floor 0).
- Target velocity: down at `LandingSpeed * (0.125 + 0.875 * scale)`, horizontal speed
  damped exponentially.
- A 1024-unit downward RayHit gives the ground distance. LANDING → LANDED when
  ground distance < instance landed height **and** the surface normal dot up > 0.9375
  (slope under ~20 degrees) **and** the surface is not water / `DenyFlyerLand`.
  On a bad surface it goes straight to TAKEOFF (state 1) instead. This write is inside
  Update and does not go through `TakeOff`.
- Holding the flyer's land trigger with progress < 0.05 also aborts to TAKEOFF.

Instance landed height (`UpdateLandedHeight`): class `mLandedHeight`, raised to
`-slot.offset.y - cargoBBoxMinY` for every attached cargo, so the carrier lands with the
cargo touching the ground.

### 5.4 Drop and removal (`VehicleSpawn::UpdateLive`)

`VehicleSpawn::Update` walks the tracker list and calls `UpdateLive` once per **live**
tracked vehicle, then calls `UpdateSpawn` if fewer trackers than `mSpawnCount` remain.
Inside UpdateLive, while `mCarrier` is valid:

- LANDED: `DetachCargo(0)` then `TakeOff()`.
- FLYING: `carrier->vtable[3](1)` (`PUSH 1; CALL [EAX+0xC]` on both builds). Deleted.
- anything else: fall through to the empty-vehicle expiry logic for the tracked vehicle.

TAKEOFF → FLYING happens in Update when `progress += dt / TakeoffTime` reaches 1 while
ground distance > `TakeoffHeight + LandedHeight`.

### 5.5 EntityCarrier methods

- **Update**: after `EntityFlyer::Update` returns true, every live cargo is placed:
  slot offset through the carrier matrix, cargo sub-object `vtable+0xC` (SetMatrix), then
  cargo `vtable+0x48(&carrier velocity)` (SetVelocity). **Cargo placement happens inside
  the original Update.** It also counts down the 1.5 s "recently detached cargo" timer.
- **AttachCargo**: returns false if the slot already holds live cargo. Otherwise
  `offset = node.offset - cargo->GetCenter()` (`vtable+0xA0`, no null check), stores the
  handle, sets `cargo+0x58` (`CollisionObject::mParent`) to the carrier, updates the
  landed height, plays `PickupSound`.
- **DetachCargo**: clears `cargo->mParent`, remembers the cargo for 1.5 s (collisions with
  it are ignored meanwhile), clears the slot, updates the landed height, plays
  `DropoffSound`.
- **Kill**: drops first person for a local pilot, wakes and detaches every live cargo,
  then `EntityFlyer::Kill`.
- **ActivatePhysics**: only activates the Controllable with priority -1. EntityFlyer's
  version also activates the post-collision sub-object (-15), aimers, turrets and
  passenger slots, so carrier turrets are built but never activated.

### 5.6 Flight states

| State | Name |
|-------|------|
| 0 | LANDED |
| 1 | TAKEOFF (ascending) |
| 2 | FLYING |
| 3 | LANDING |
| 4 | DYING |
| 5 | DEAD |

The generic flyer AI goal (`005AF000`, modtools) picks random command posts and calls
`TakeOff` whenever the flyer isn't FLYING, including mid-descent. Unchecked, the carrier
oscillates LANDING → TAKEOFF → LANDING and never lands.

---

## 6. ODF reference

`EntityCarrierClass::SetProperty`:

| Key | Hash | Effect |
|-----|------|--------|
| `CargoNodeName`   | `0x3E2C4DA4` | Looks the bone up in the **already loaded** geometry (up to 0x80 bones) and copies its bind translation into a new cargo node. Bone missing or no geometry yet: no node is created, silently. |
| `CargoNodeOffset` | `0x910A89FC` | `x y z`, creates a **new** cargo node. Does not adjust a `CargoNodeName` node. |
| `PickupSound`     | `0x759C8F54` | played on attach |
| `DropoffSound`    | `0x8897DB28` | played on detach (`DropOffSound` hashes the same) |

- `GeometryName` must come before `CargoNodeName`.
- Maximum 4 cargo nodes (GameExt enforces it, see 9.1).
- Whether the bone translation is model space or parent-relative has not been checked; a
  hardpoint nested under other bones may be misplaced.

EntityFlyerClass keys that matter for carriers:

| Key | Effect for a pad carrier |
|-----|--------------------------|
| `LandingTime` | descent duration; also sets the spawn distance (5.2) |
| `LandingSpeed` | descent speed; also sets the spawn height |
| `MinSpeed` | initial forward speed; sets the spawn distance |
| `TakeoffTime` | climb duration until FLYING (and deletion) |
| `TakeoffSpeed` | climb speed; GameExt also uses it as the post-drop forward speed |
| `TakeoffHeight` | climb must clear `TakeoffHeight + LandedHeight` before FLYING |

Collision: carrier meshes need at least one `p_` collision primitive. With none,
`EntityFlyerClass::SetProperty` (`004FA310`) builds a `main_body` capsule from the model
bounding box (radius = average of the X/Z half extents, height = Y extent), which is
usually far too large.

---

## 7. Rendering

- `EntityFlyer::Render` shows takeoff anim frame `(nFrames - 1) * progress`, using the
  anim ref cached on the instance. During LANDING, progress runs 1 → 0, so vanilla plays
  the takeoff clip backwards on the way down.
- The render's first argument is the LOD. Far-scene objects render at LOD 3, which may
  have no skinning, so an animation can run and still be invisible.
- The render is skipped by a JZ after the frustum test on the scene bounding sphere
  (modtools `0x004F6999`, release `0x004AB082`).

---

## 8. Team bits and carried cargo

`+0x234`: bits 4-7 team (spawning and entering use it), bits 8-11 perceived team.
`SetTeam` is vtable slot 36. VehicleSpawn writes the bit fields with XOR masks after
calling SetTeam.

---

## 9. GameExt fixes and additions

All in `flyer_carrier_fixes.cpp`, installed from `lua_hooks_install()` on modtools, Steam
and GOG through one install path. Per-build offsets live in `CarrierLayout`; per-carrier
state lives in `CarrierTrack`, keyed by object pointer **and** PblHandle id, because the
next carrier from a pad usually reuses the deleted carrier's pool block.

### 9.1 Memory-safety guards

- `SetProperty`: cargo node ignored once 4 exist (entry 4 would overwrite `mCargoCount`,
  entry 5 `mSoundCargoPickup`).
- `AttachCargo`: null cargo rejected; slot >= 4 rejected on modtools (release ignores the
  argument, see 9.3).
- `DetachCargo`: slot >= 4 rejected.

### 9.2 Pad lifecycle gaps

- **Stuck pad.** If the cargo is destroyed while carried, its tracker is freed, UpdateLive
  never runs for the pad, the carrier sits landed forever and UpdateSpawn keeps bailing on
  the live `mCarrier`. `padCarrierUpdate` runs UpdateLive's carrier block (LANDED → drop +
  TakeOff, FLYING → delete) from the UpdateSpawn hook, which the engine calls exactly when
  the pad is below its tracker count. Repeating it is harmless when UpdateLive already ran.
- **Failed landing.** A landing aborted by the surface check climbs to FLYING and is deleted
  with the cargo attached, leaving the cargo with a dangling `mParent` and a parked team.
  `dropStrandedCargo` drops every slot as soon as a carrier is in TAKEOFF or FLYING with
  cargo attached, and slots 1-3 when it is LANDED.
- **TakeOff during LANDING** (flyer AI) is blocked for carriers.

### 9.3 Multi-cargo

Extra cargo for slots 1..N-1 is spawned after the original UpdateSpawn, within the pad's
spawn budget, each with its own VehicleTracker. On release `AttachCargo` always writes slot
0 (LTCG specialised it for its only caller), so `attachCargoToSlot` presents class node N as
node 0 and an empty slot 0, calls the original, then moves the result into slot N and
restores what it borrowed (in a `__finally`, since the class is shared).

### 9.4 Cargo team parking

While carried, cargo is set to team 0 so it can't be used as a spawn point or entered in
the air. The team is saved in the track and restored on every detach. All GameExt-initiated
detaches go through the hooked DetachCargo so the restore always runs.

### 9.5 Flight overrides

- **Descent**: on entering LANDING, smoothstep X/Y/Z from the current position onto the pad
  over `LandingTime`, targeting `padY + instance landed height - 1` so the ground check
  fires directly over the pad.
- **Post-drop ascent**: at TakeOff after a drop, snapshot the rotation rows
  (`+0xF0..+0x11F`) and X/Z; each TAKEOFF frame restore the rotation and move X/Z along the
  saved heading, ramping to `TakeoffSpeed` over 3 s. Vanilla keeps driving Y. A
  post-drop carrier can't re-enter LANDING.
- **Terrain wobble**: the two downward RayHit calls in `EntityFlyer::Update` feed the terrain
  normal into heading and pitch. While a tracked carrier is more than
  `max(2 * landedHeight, 10)` above its pad they report "no hit" (ground distance 1024) for
  the duration of its Update: modtools `FLD1` + NOPs, release a CALL to a stub that sets
  XMM0 = 1.0. Sites: modtools `0x004FE8CD` / `0x004FEAE2`, release `0x004AE246` /
  `0x004AE478`.

### 9.6 Rendering

For tracked carriers the render hook bypasses the frustum-cull JZ, holds progress at 0
(bay closed) during LANDING, and after the slot-0 drop plays the takeoff clip 0 → 1 over
`nFrames / 30` seconds (pause-aware, forced LOD 0, anim ref forced to the takeoff clip,
net delta zeroed). The scene bounding sphere is kept on the pivot with radius at least 80.

### 9.7 Turrets

- **ActivatePhysics**: vtable[41] is replaced with EntityFlyer's version keeping the
  carrier's -1 priority, so turrets, aimers and passenger slots get activated.
- **Fire while airborne**: `MountedTurret::Update` blocks turret fire unless the parent
  flyer is LANDED. A cave skips that test for carriers (modtools `0x565C4C`, 17 bytes;
  release `0x5A64DF`, 18 bytes).
- **PILOT_SELF AI**: `MountedTurret::UpdateIndirect` borrows the carrier's UnitController
  for the call, reads the fire decision it wrote into the carrier's triggers, fires when
  on target and pumps the turret weapon's `Update_` every frame. Release's trigger state
  machine takes no dt (RET 4 vs RET 8).
- **CreateController null check** (modtools only): PlayerController path dereferences
  `[ESI+0xD0]+0xD4` without a check.

### 9.8 Calling-convention traps

- `VehicleSpawn::UpdateSpawn` on release: dt in XMM1, bare RET. Bridged with naked thunks.
- Release `AttachCargo` ignores its slot argument.
- Release trigger state machine: `(trigger, fire)`, no dt.

---

## 10. Open issues

- **Cargo trails the carrier by one frame.** The descent and ascent overrides write the
  transform after the original Update, which already placed the cargo from the physics
  pose. The cargo also inherits the physics velocity at drop time, which may explain a
  lurch on release. Fix: re-run the cargo placement after the overrides and set the
  carrier velocity to match the override.
- **Raw position writes skip `EntityFlyer::SetPosition`**, which normally recomputes the
  bounding-sphere centre and notifies the scene and collision systems. That is why the
  cull bypass and the radius-80 sphere exist. Moving the carrier through SetPosition
  should make both unnecessary.
- **Does the stock landing really miss the pad?** The spawn distance assumes horizontal
  speed falls linearly from MinSpeed to 0 over LandingTime, but LANDING damps horizontal
  speed exponentially. If that mismatch is the whole miss, correcting the spawn
  distance could replace the descent override. A cleaner long-term design is a forward
  velocity term during LANDING and TAKEOFF with the spawn distance computed to match,
  keeping the flight model in charge instead of overwriting positions. Not verified.
- **Turrets share the carrier's AI state** during their borrowed UpdateIndirect call, so
  several turrets don't select targets independently. Needs UnitController RE.
- **Multiplayer clients**: `dropStrandedCargo` runs on every machine; carriers on clients
  were not checked.
- The EntityCarrier memory pool size caps carriers map-wide; GameExt tracks at most 8.
