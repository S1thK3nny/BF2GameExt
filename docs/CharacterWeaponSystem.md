# Character Weapon System — Reverse Engineering Notes

Everything documented here was reverse-engineered from `BF2_modtools.exe` using Ghidra, x64dbg,
and runtime memory scanning via BF2GameExt PatcherDLL diagnostic hooks.
All addresses are **unrelocated** (imagebase = `0x400000`). At runtime, resolve via:

```cpp
uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
void* resolved = (void*)((unrelocated_addr - 0x400000u) + base);
```

---

## Status

| Finding                                              | Status            |
|------------------------------------------------------|-------------------|
| charIndex → charSlot → intermediate → Controllable   | ✅ Confirmed      |
| Weapon slot array at `ctrl+0x4D8` (8 entries max)    | ✅ Confirmed      |
| WeaponClass pointer in Weapon (`+0x060`)             | ✅ Confirmed      |
| ODF name in WeaponClass (`+0x30`, inline string)     | ✅ Confirmed      |
| Channel→slot index array at `ctrl+0x4F8`             | ✅ Confirmed      |
| `GetCharacterWeapon(char, 0)` returns held primary   | ✅ Working        |
| `GetCharacterWeapon(char, 1)` returns held secondary  | ✅ Working        |
| Weapon channel classification by vtable              | ❌ Unreliable     |

---

## Quick Reference — How `GetCharacterWeapon` Works

```cpp
// Get ODF name of weapon in channel N (0=primary, 1=secondary, ...)
uintptr_t arrayBase = *(uintptr_t*)res(0xB93A08);   // global character array
char* charSlot      = (char*)arrayBase + charIndex * 0x1B0;
char* intermediate  = *(char**)(charSlot + 0x148);
char* ctrl          = intermediate + 0x18;            // Controllable*

uint8_t slotIdx     = *(uint8_t*)(ctrl + 0x4F8 + channel);  // channel → slot index
char* weapon        = *(char**)(ctrl + 0x4D8 + slotIdx * 4); // Weapon*
char* wepClass      = *(char**)(weapon + 0x060);              // WeaponClass*
const char* odfName = wepClass + 0x30;                        // ODF name string
```

---

## Full Resolution Chain (Diagram)

```
                        Global Variables
                        ================
  0xB93A08  ──►  mCharacterStructArray*
  0xB939F4  ──►  MaxCharacterCount (int, validate charIndex < this)

                        Character Slot
                        ==============
  mCharacterStructArray + charIndex * 0x1B0
           │
           ▼  charSlot (0x1B0 bytes per character)
  *(charSlot + 0x148)
           │
           ▼  intermediate*

                        Controllable
                        ============
  intermediate + 0x18
           │
           ▼  ctrl  (Controllable*, runtime vtable varies by derived class)
           │
           ├── ctrl+0x4D8  ──►  Weapon* slot[0]  ──►  *(wpn+0x060) = WeaponClass*
           ├── ctrl+0x4DC  ──►  Weapon* slot[1]       WeaponClass+0x30 = "odf_name"
           ├── ctrl+0x4E0  ──►  Weapon* slot[2]
           ├── ctrl+0x4E4  ──►  Weapon* slot[3]
           ├── ctrl+0x4E8  ──►  Weapon* slot[4]       (up to 8 slots, NULL-terminated)
           ├── ctrl+0x4EC  ──►  Weapon* slot[5]
           ├── ctrl+0x4F0  ──►  Weapon* slot[6]
           ├── ctrl+0x4F4  ──►  Weapon* slot[7]
           │
           └── ctrl+0x4F8  ──►  uint8[] channelSlotIndex
                                  [0] = primary channel's current slot index
                                  [1] = secondary channel's current slot index
                                  ...
```

---

## The Key Discovery: `ctrl+0x4F8` — Channel-to-Slot Index Array

This byte array sits **immediately after** the 8-entry weapon pointer array
(`0x4D8 + 8×4 = 0x4F8`). Each byte is a **direct index** into the weapon slot
array at `ctrl+0x4D8`.

**Formula:** `slotIndex = *(uint8_t*)(ctrl + 0x4F8 + channel)`

Then: `weapon = *(Weapon**)(ctrl + 0x4D8 + slotIndex * 4)`

| Byte Offset | Meaning                                      |
|-------------|----------------------------------------------|
| `+0x4F8`    | Channel 0 (primary) → current weapon slot index   |
| `+0x4F9`    | Channel 1 (secondary) → current weapon slot index |
| `+0x4FA`    | Channel 2 (if exists)                             |
| `+0x4FB`    | Channel 3 (if exists)                             |

### Runtime Proof (rep_inf_coruscant_guard_shock_rocketeer)

This character has 5 weapons in its slot array:

| Slot | ODF Name                          | Channel |
|------|-----------------------------------|---------|
| 0    | rep_weap_rps-6_rocket_launcher    | Primary |
| 1    | rep_weap_dc-15a_blaster_rifle     | Primary |
| 2    | rep_weap_mine_dispenser           | Secondary |
| 3    | rep_weap_thermal_detonator        | Secondary |
| 4    | rep_weap_melee                    | Secondary |

Test results:

| Held primary | Held secondary | byte[0] | byte[1] | ch0 result | ch1 result |
|-------------|----------------|---------|---------|------------|------------|
| RPS-6       | mines          | 0       | 2       | RPS-6 ✅   | mines ✅  |
| DC-15A      | thermals       | 1       | 3       | DC-15A ✅  | thermals ✅ |
| DC-15A      | melee          | 1       | 4       | DC-15A ✅  | melee ✅  |

### Memory dump at each state

```
Holding RPS-6 + mines:       ctrl+0x4F8 = 0x00000200  → byte[0]=0, byte[1]=2
Holding DC-15A + thermals:   ctrl+0x4F8 = 0x00010301  → byte[0]=1, byte[1]=3
Holding DC-15A + melee:      ctrl+0x4F8 = 0x00010401  → byte[0]=1, byte[1]=4
```

---

## Global Variables

| Address    | Type          | Description                                         |
|------------|---------------|-----------------------------------------------------|
| `0xB93A08` | `void**`      | `mCharacterStructArray` — pointer to slot array base |
| `0xB939F4` | `int`         | `MaxCharacterCount` — validate charIndex before use  |
| `0x7E3D50` | `function`    | `GameLog(const char* fmt, ...)` — printf to BF2 debug log |

---

## Character Slot Layout

Each character slot is `0x1B0` bytes.

| Offset   | Type     | Description                                           |
|----------|----------|-------------------------------------------------------|
| `+0x00C` | `void*`  | Changes on weapon switch (different pointer each time)|
| `+0x148` | `void*`  | Pointer to "intermediate" object                      |

---

## Controllable Layout (Confirmed Fields)

```
Controllable base vtable: 0x00A403A0  (but derived classes override ALL entries)
Reachable as: intermediate + 0x18  ≡  entity + 0x258
```

| Offset   | Type       | Description                                                  |
|----------|------------|--------------------------------------------------------------|
| `+0x000` | `void**`   | vtable (derived class, NOT 0x00A403A0 at runtime)            |
| `+0x084` | `void*`    | embedded sub-object (vtable `0x00A3B0AC`)                    |
| `+0x09C` | `void*`    | points to `ctrl - 0x18` (= intermediate)                     |
| `+0x0A0` | `void*`    | points near `ctrl - 0x68`                                    |
| `+0x174` | `void*`    | embedded sub-object (vtable `0x00A40664`, near Entity vtable)|
| `+0x188` | `void*`    | self-pointer `= &ctrl+0x188` (circular list head)            |
| `+0x290` | `void*`    | back-pointer to Entity `= entity`                            |
| `+0x4C0` | `Weapon*`  | **always = slot[0]** — NOT the active weapon (misleading)    |
| `+0x4D8` | `Weapon*[8]` | **Weapon slot array** — up to 8 weapon pointers            |
| `+0x4F8` | `uint8[]`  | **Channel→slot index array** — byte[N] = slot index for channel N |
| `+0x4FC` | `uint32`   | Observed values: 3 (decreases on weapon switches), likely weapon-count related |
| `+0x740` | `int`      | `6` observed                                                 |
| `+0x744` | `int`      | `8` observed — likely max weapon slot capacity               |
| `+0x764` | `int`      | `7` observed                                                 |

### `ctrl+0x4C0` — NOT the active weapon

Despite appearing to be a "current weapon" pointer, `ctrl+0x4C0` always equals `slot[0]`.
It does NOT update on weapon switch. **Do not use it.** Use `ctrl+0x4F8` instead.

### `ctrl+0x9D0..+0x9FC` — NOT weapons on Controllable

The Ghidra disassembly of `GetAimTurnRate` shows `[ESI + EAX*4 + 0x9D0]` labeled "GetWeaponPtr",
but `ESI` is a sub-object loaded earlier in that function, NOT the Controllable pointer. These
offsets on Controllable are always zero. **Do not use.**

---

## Weapon Object Layout (Confirmed via Steam-build PDB structs)

**Source of truth**: the Steam release `BattlefrontII.exe` (Ghidra port 8193) carries PDB-derived
struct types `Weapon` (284 bytes) and `Weapon_vftable`. Offsets are 1:1 with the modtools build
(verified against the well-known `+0x60`, `+0x88`, `+0xC8` fields). When in doubt, look it up
in the Steam binary first.

Weapon inherits from `Thread` (24 bytes at +0x00).

```
Weapon vtables (Weapon..WeaponD, alphabetized):
   0x00A51C08  — Weapon (base)
   0x00A520D8  — WeaponAreaEffect
   0x00A52288  — WeaponBinoculars
   0x00A52468  — WeaponCannon          (DC-15A blaster, melee, etc.)
   0x00A52740  — WeaponCatapult        (turret arm)
   0x00A52960  — WeaponDestruct
   0x00A52B80  — WeaponDetonator       (extends WeaponDispenser)
   0x00A52D68  — WeaponDisguise
   0x00A53020  — WeaponDispenser       (mine dispenser)
   0x00A53510  — (thermal detonator subclass)
   0x00A53AE8  — WeaponGrapplingHook   (extends WeaponCannon)
```

**WARNING**: You **cannot** determine a weapon's channel from its vtable. The game uses
multiple weapon subclasses, and weapons of the same channel may have different vtables.
Channel assignment comes from the ODF `WeaponChannel` property and is tracked at
`ctrl+0x4F8`.

| Offset   | Field                         | Type                  | Notes |
|----------|-------------------------------|-----------------------|-------|
| `+0x000` | vftable                       | `void**`              | varies by subclass |
| `+0x000..+0x017` | `Thread` base         | (24 bytes)            | |
| `+0x020` | `mFirePointMatrix`            | `PblMatrix` (64 b)    | |
| **`+0x060`** | **`mStart`**              | **`WeaponClass*`**    | initial/template class |
| **`+0x064`** | **`mClass`**              | **`WeaponClass*`**    | **current/effective class — mutable** (swaps for ammo modes) |
| **`+0x068`** | **`mRenderClass`**        | **`WeaponClass*`**    | rendering-side class |
| `+0x06C` | `mOwner`                      | `Controllable*`       | direct owner ptr (no chain needed) |
| `+0x070` | `mAimer`                      | `Aimer*`              | |
| `+0x074` | `mTrigger`                    | `Trigger*`            | fire trigger |
| `+0x078` | `mReload`                     | `Trigger*`            | reload trigger (separate from fire) |
| `+0x07C` | `mFirePos`                    | `PblVector3`          | 12 bytes |
| **`+0x088`** | **`m_pAmmoCounter`**      | **`AmmoCounter*`**    | **24-byte heap object; NOT OrdnanceFactory** |
| `+0x08C` | `m_pEnergyBar`                | `EnergyBar*`          | |
| `+0x090` | `mCurChargeStrengthHeavy`     | `float`               | |
| `+0x094` | `mCurChargeStrengthLight`     | `float`               | |
| `+0x098` | `mCurChargeDelayHeavy`        | `float`               | |
| `+0x09C` | `mCurChargeDelayLight`        | `float`               | |
| `+0x0A0` | `mCurChargeRateLight`         | `float`               | |
| `+0x0A4` | `mCurChargeRateHeavy`         | `float`               | |
| `+0x0A8` | `mCurTimeAtMaxCharge`         | `float`               | |
| `+0x0AC` | bitfield                      | `uint`                | `mHideWeapon:1, mFiredFlag:1, mSelectedFlag:1, m_iSoldierState:6` |
| `+0x0B0` | `mState`                      | `WeaponState`         | 0=idle, 1=fire, 2=charge, 3=reload, 4=empty |
| `+0x0B4` | `mStateTimer`                 | `float`               | incremented by dt |
| `+0x0B8` | `mStateLimit`                 | `float`               | state duration cap |
| `+0x0BC` | `mZoom`                       | `float`               | |
| `+0x0C0` | `mZoomTurnScale`              | `float`               | |
| `+0x0C4` | `mMuzzleFlashStartTime`       | `float`               | (NOT mLastFireTime — see +0xF8) |
| **`+0x0C8`** | **`mSoldierAnimationMap`** | **`MAP` (int)**       | -1 = invalid; per-frame UpdateIndirect reads this |
| `+0x0CC` | `mSoundControl`               | `GameSoundControllable` | |
| `+0x0D0` | `mSoundControlFire`           | `GameSoundControllable` | |
| `+0x0D4` | `mFoleyFXControl`             | `GameSoundControllable` | |
| `+0x0D8` | `mSoundProps`                 | `GameSound*` (8 b)    | |
| `+0x0E0` | `mSoundPropsFire`             | `GameSound*`          | |
| `+0x0E8` | `mFoleyFXProps`               | `GameSound*`          | |
| `+0x0F0` | `mChargeUpEmitter`            | `PblHandle<ParticleEmitterObject>` | |
| `+0x0F8` | `mLastFireTime`               | `float`               | real last-fire timestamp |
| `+0x0FC` | `mSkipTime`                   | `float`               | |
| `+0x100` | bitfield                      | `bool`                | `mSkip:1, mCharged:1` |
| `+0x101` | `mDeactivateScheduled`        | `uchar`               | |
| `+0x104` | `mTarget`                     | `PblHandle<GameObject>` | lock-on target |
| `+0x10C` | `mTargetBodyID`               | `int`                 | |

Total size: 284 bytes (Steam build; modtools may differ slightly past +0x10C).

### Weapon vftable layout (slot index × 4 bytes)

| Slot | Method | Notes |
|------|--------|-------|
| 0 | `~Weapon` (scalar deleting dtor) | |
| 1 | `Update(dt)` | per-frame state pump |
| 2-4 | `ActivateThread` / `Deactivate` / `IsThreadActive` | from `Thread` base |
| 5-7 | RTTI helpers | |
| 9 | `GetOrdnanceVelocity` | |
| 10 | `GetOrdnanceGravity` | |
| 11-13 | `IsLocked` / `GetLocked` / `GetLockedTargetBodyID` | missile lock |
| 16-17 | `GetYawSpread` / `GetPitchSpread` | recoil |
| 18 | `Deflect` | lightsaber-deflect hook |
| 19 | `SignalFire` | network/event fire signal |
| 20 | `ShouldShowReticule` | HUD |
| 21-22 | `IsMelee` / `IsMeleeThrow` | type queries |
| 23 | `NotifySoldierState` | |
| 24 | `SoldierCanOperate` | |
| 25-28 | `OverrideSoldierVelocity` / `Controls` / `EnergyRestore` / `Aimer` | |
| **29** | **`Select(param_1, silent)`** | weapon-select state init + sound |
| 30 | `Deselect` | |
| 31 | `IsBusy` | |
| 33-34 | `Write` / `Read` | netcode |
| 35 | `Render` | |
| 36 | `GetNameHash` | |
| 37-39 | `EnterIdle` / `UpdateIdle` / `ExitIdle` | |
| 40 | `EnterFire` | |
| 42-43 | `StopFireSound` x2 | |
| 51 | `ExitEmpty` | |
| 52 | `CheckCharge` | |
| 53 | `CheckFire` | predicate |
| 54 | `FireDone` | |
| 55 | `CheckEmpty` | |
| 57 | `CheckReload` | |
| 58 | `ReloadDone` | |

**No `ForceReload` virtual exists** — reload is driven by `mReload` Trigger + state machine.

---

## WeaponClass Object (Confirmed)

Prototype / class-definition object — one per weapon ODF type, shared across all instances
of that weapon. Multiple Weapon objects can point to the same WeaponClass.

```
WeaponClass vtable: 0x00A525F4
```

| Offset     | Type            | Description                                     |
|------------|-----------------|-------------------------------------------------|
| `+0x000`   | `void**`        | vtable                                          |
| `+0x004`   | `void*`         | pointer to a static/global data table           |
| `+0x008`   | `void*`         | linked list: next                               |
| `+0x00C`   | `void*`         | linked list: prev                               |
| `+0x010`   | `WeaponClass*`  | self-pointer (circular list head)               |
| `+0x014`   | `WeaponClass*`  | linked list                                     |
| `+0x018`   | `uint32`        | hash / checksum                                 |
| `+0x01C`   | `int`           | `0x1E` (30) — possibly weapon type enum         |
| `+0x024`   | `float`         | small float                                     |
| `+0x028`   | `float`         | small float                                     |
| `+0x02C`   | `float`         | small float                                     |
| **`+0x030`** | **`char[]`**  | **ODF name — inline, null-terminated ← READ HERE** |
| `+0x050`   | `void*`         | heap sub-object pointer                         |
| `+0x054`   | `uint32`        | hash (same as `+0x018`)                         |
| `+0x060`   | `WeaponClass*`  | self-pointer again                              |
| `+0x064`   | `WeaponClass*`  | linked list                                     |
| `+0x068`   | `WeaponClass*`  | linked list                                     |
| `+0x078`   | `void*`         | heap pointer                                    |

**Confirmed ODF names observed**: `"rep_weap_dc-15s_blaster_carbine"`,
`"rep_weap_rps-6_rocket_launcher"`, `"rep_weap_dc-15a_blaster_rifle"`,
`"rep_weap_mine_dispenser"`, `"rep_weap_thermal_detonator"`, `"rep_weap_melee"`

---

## PDB-Known Controllable Methods

These addresses come from a PDB but the **Controllable vtable entries do NOT match them** at
runtime — the derived class overrides every slot. These are useful for setting breakpoints
or finding cross-references in Ghidra, but cannot be found via vtable scanning.

| Method                     | Address      | Notes                                                   |
|----------------------------|--------------|---------------------------------------------------------|
| `SetWeaponIndex(int, int)` | `0x005E6F70` | Sets weapon for (slotIdx, channel). `RET 0xC`. Calls `PlayAnimation`. |
| `GetCurWpn()`              | `0x005E7090` | Traverses float-priority sorted linked list via FPU `FCOMP`. |
| `GetActiveWeaponChannel()` | `0x004DBCF0` | Stub: `XOR EAX,EAX; RET` — always returns 0.           |
| `GetWpnChannel()`          | `0x005E7100` | All `INT3` — pure virtual, not implemented.             |
| `GetCurAimer()`            | `0x005E7070` | Initializes an intrusive list head sub-struct.          |
| `SetCharacter(Character*)` | `0x005E6FA0` | Links Controllable to its Character object.             |
| `Update(float)`            | `0x005E6FE0` | Per-frame update function.                              |

---

## Per-Frame Tick Broadcaster

Originally suspected to be a weapon-switch event system — **it is not**. It is a generic
per-frame tick broadcaster. `g_deltaTime` (a float, ~0.0125s) is passed as `param_1` to all
subscribers, not a weapon/entity pointer.

```
GameLoop (0x007363e4)
  → TickBroadcaster_Dispatch([g_deltaTime], 0, 0)       // 0x0048fda0
    → TickBroadcaster_BroadcastList(primaryList, dt)    // 0x0048fcd0
      → TickBroadcaster_NotifyChain(node, dt)           // 0x0048fc70
        → node->vtable[1](dt)                           // virtual Update(float dt)
          if returns false → node->vtable[0](1)         // fallback/reset
```

### Subscriber lists
| Global | Address | Used when |
|--------|---------|-----------|
| `g_tickSubscribers_primary` | `0xb6a704` | Always (param2=0 path) |
| `g_tickSubscribers_secondary` | `0xb6a774` | param3=1 AND multiple game-state flags pass |
| `g_tickSubscribers_alt` | `0xb6a728` | param2=1 path |

### `TickBroadcaster_Dispatch` — parameter semantics
| Param | Type | Effect |
|-------|------|--------|
| `param_1` | `float` | Delta time — forwarded to every subscriber's vtable[1] |
| `param_2` | `byte` | If non-zero: use alt list instead of primary |
| `param_3` | `byte` | If non-zero AND flags pass: also notify secondary list |

### Animation subscriber
`AnimSubscriber_Update` at `0x0055f080` is registered in `g_tickSubscribers_primary`. It is
called every frame with the current delta time. This is what drives the per-character animation
state. **This is the key function to decompile** — it determines what data it reads to select
the animation stance, which will tell us what SetCharacterWeapon needs to write.

---

## What Has NOT Worked (Dead Ends)

These approaches were tested and confirmed to fail. Documented here to save future effort.

| Approach                              | Why it failed                                      |
|---------------------------------------|----------------------------------------------------|
| `ctrl + 0x9D0` as weapon array        | Always zero. `ESI` in GetAimTurnRate ≠ Controllable |
| `intermediate+0x18` vtable `0xA5A6E0` | That's **SoldierElement** (rendering/particles)    |
| `SoldierElement + 0x10C..+0x12C`      | Inside particle component, not weapon data         |
| `ctrl + 0x4C0` as "active weapon"     | Always equals slot[0], never changes on switch     |
| Vtable-based channel classification   | 3+ different vtables across weapons in same channel|
| PDB addresses in Controllable vtable  | Derived class overrides ALL vtable entries; 0 PDB matches |
| `GetActiveWeaponChannel()` at `0x4DBCF0` | Stub function, always returns 0 (xor eax,eax; ret) |
| `GetWpnChannel()` at `0x5E7100`       | All INT3 — pure virtual, never implemented         |

---

## Ghidra Labels to Apply

### Vtables
| Address      | Label                           | Notes                                              |
|--------------|---------------------------------|----------------------------------------------------|
| `0x00A403A0` | `Controllable::vftable`         | Base class vtable; derived classes override all entries |
| `0x00A40500` | `Entity::vftable`               |                                                    |
| `0x00A3B0AC` | `Controllable_SubObj084::vftable` | Embedded sub-object at ctrl+0x084                |
| `0x00A40664` | `Controllable_SubObj174::vftable` | Embedded sub-object at ctrl+0x174, near Entity   |
| `0x00A52468` | `WeaponA::vftable`              | Weapon type A (blaster rifle, melee)               |
| `0x00A53510` | `WeaponB::vftable`              | Weapon type B (thermal detonator)                  |
| `0x00A53AE8` | `WeaponC::vftable`              | Weapon type C (rocket launcher)                    |
| `0x00A53020` | `WeaponD::vftable`              | Weapon type D (mine dispenser)                     |
| `0x00A525F4` | `WeaponClass::vftable`          | Weapon prototype/definition                        |
| `0x00A2B1BC` | `IntrusiveListNode::vftable`    | Embedded in Weapon objects, forms linked list       |
| `0x00A5A6E0` | `SoldierElement::vftable`       | Rendering/particle — NOT weapons                   |
| `0x00A58E20` | `WeaponSentinel::vftable`       | Sentinel sub-object in weapon list                  |

### Global Data
| Address    | Label                        | Type     | Notes                                    |
|------------|------------------------------|----------|------------------------------------------|
| `0xB93A08` | `g_mCharacterStructArray`    | `void**` | Pointer to character slot array base     |
| `0xB939F4` | `g_MaxCharacterCount`        | `int`    | Max valid charIndex (exclusive)          |
| `0xb6a704` | `g_tickSubscribers_primary`  | list head | Primary per-frame tick subscriber list  |
| `0xb6a774` | `g_tickSubscribers_secondary`| list head | Secondary tick subscribers (conditional)|
| `0xb6a728` | `g_tickSubscribers_alt`      | list head | Alternate list (used when param2 != 0)  |
| `0xc6a9b0` | `g_deltaTime`                | `float`  | Current frame delta time (~0.0125s). Passed as param to all tick subscribers. NOT an entity pointer. |

### Functions
| Address      | Label                                  | Signature / Notes                                |
|--------------|----------------------------------------|--------------------------------------------------|
| `0x007E3D50` | `GameLog`                              | `void GameLog(const char* fmt, ...)` — debug log |
| `0x005E6F70` | `Controllable::SetWeaponIndex`         | `void (int slotIdx, int channel)` — RET 0xC      |
| `0x005E7090` | `Controllable::GetCurWpn`              | Float-priority list traversal                    |
| `0x004DBCF0` | `Controllable::GetActiveWeaponChannel` | Stub: `xor eax,eax; ret` — always 0             |
| `0x005E7100` | `Controllable::GetWpnChannel`          | Pure virtual (all INT3)                          |
| `0x005E7070` | `Controllable::GetCurAimer`            | Initializes intrusive list head sub-struct       |
| `0x005E6FA0` | `Controllable::SetCharacter`           | Links Controllable ↔ Character                   |
| `0x005E6FE0` | `Controllable::Update`                 | Per-frame update, takes float dt                 |
| `0x0048fda0` | `TickBroadcaster_Dispatch`             | Selects subscriber list(s) and broadcasts delta-time to each |
| `0x0048fcd0` | `TickBroadcaster_BroadcastList`        | Iterates doubly-linked subscriber list, calls `_NotifyChain` on each |
| `0x0048fc70` | `TickBroadcaster_NotifyChain`          | Per-node: calls `vtable[1](param)`, on false calls `vtable[0](1)` |
| `0x00449d10` | `TickBroadcaster_Lock`                 | Critical-section lock/unlock around each broadcast |
| `0x0055f080` | `AnimSubscriber_Update`                | Subscriber vtable[1] — per-frame animation update, receives delta time |

### Struct Field Labels (for Ghidra struct definitions)

**Controllable struct** (apply to the struct used at `0x00A403A0`):
| Offset    | Field Name                | Type              |
|-----------|---------------------------|--------------------|
| `+0x09C`  | `m_pIntermediate`         | `void*`            |
| `+0x290`  | `m_pEntity`               | `Entity*`          |
| `+0x4C0`  | `m_pDefaultWeapon`        | `Weapon*`          |
| `+0x4D8`  | `m_aWeaponSlots`          | `Weapon*[8]`       |
| `+0x4F8`  | `m_aChannelSlotIndex`     | `uint8_t[8]`       |
| `+0x744`  | `m_nMaxWeaponSlots`       | `int` (value: 8)   |

**Weapon struct** (use Steam-build `Weapon` struct as canonical reference):
| Offset    | Field Name                | Type              |
|-----------|---------------------------|--------------------|
| `+0x060`  | `mStart`                  | `WeaponClass*`     |
| `+0x064`  | `mClass`                  | `WeaponClass*`     |
| `+0x068`  | `mRenderClass`            | `WeaponClass*`     |
| `+0x06C`  | `mOwner`                  | `Controllable*`    |
| `+0x088`  | `m_pAmmoCounter`          | `AmmoCounter*`     |
| `+0x08C`  | `m_pEnergyBar`            | `EnergyBar*`       |
| `+0x0B0`  | `mState`                  | `WeaponState`      |
| `+0x0B4`  | `mStateTimer`             | `float`            |
| `+0x0B8`  | `mStateLimit`             | `float`            |
| `+0x0C8`  | `mSoldierAnimationMap`    | `MAP` (int)        |
| `+0x0F8`  | `mLastFireTime`           | `float`            |

**WeaponClass struct** (at `0x00A525F4`):
| Offset    | Field Name                | Type              |
|-----------|---------------------------|--------------------|
| `+0x018`  | `m_nHash`                 | `uint32_t`         |
| `+0x030`  | `m_szOdfName`             | `char[32]`         |
