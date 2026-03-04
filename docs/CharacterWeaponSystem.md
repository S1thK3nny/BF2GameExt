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

## Weapon Object Layout (Confirmed Fields)

Each weapon in the slot array is `~0x200` bytes, allocated contiguously.

```
Weapon vtables (multiple types — NOT consistent per channel):
   0x00A52468  — used by: DC-15A blaster rifle, melee
   0x00A53510  — used by: thermal detonator
   0x00A53AE8  — used by: RPS-6 rocket launcher
   0x00A53020  — used by: mine dispenser
```

**WARNING**: You **cannot** determine a weapon's channel from its vtable. The game uses
multiple weapon subclasses, and weapons of the same channel may have different vtables.
Channel assignment comes from the ODF `WeaponChannel` property and is tracked at
`ctrl+0x4F8`.

| Offset          | Type          | Description                                        |
|-----------------|---------------|----------------------------------------------------|
| `+0x000`        | `void**`      | vtable (varies by weapon type — see above)         |
| `+0x004`        | `void*`       | linked list sentinel (shared by weapons in same ctrl)|
| `+0x008`        | `void*`       | outer linked list: next                            |
| `+0x00C`        | `void*`       | outer linked list: prev                            |
| `+0x010`        | `Weapon*`     | inner list: next (= self when single)              |
| `+0x014`        | `Weapon*`     | inner list: prev (= self when single)              |
| `+0x018..+0x05F`| —             | uninitialized (`0xCDCDCDCD`)                       |
| **`+0x060`**    | **`WeaponClass*`** | **WeaponClass pointer** (also at `+0x064`, `+0x068`) |
| `+0x06C`        | `void*`       | back-pointer to intermediate                       |
| `+0x070`        | `void*`       | pointer near ctrl                                  |
| `+0x074`        | `void*`       | pointer near `ctrl + 0x20`                         |
| `+0x078`        | `void*`       | pointer near `ctrl + 0x28`                         |
| `+0x0B8`        | `float`       | `1.0f`                                             |
| `+0x0BC`        | `float`       | `2.5f`                                             |
| `+0x0C0`        | `float`       | `2.5f`                                             |
| `+0x0C4`        | `float`       | changes when weapon is used (float, not int)       |
| `+0x0C8`        | `int`         | 6 or 5 for slotted weapons, -1 for special weapons |
| `+0x0D8`        | `void**`      | vtable `0x00A2B1BC` — intrusive list node #1       |
| `+0x0EC`        | `void**`      | vtable `0x00A2B1BC` — intrusive list node #2       |
| `+0x100`        | `void**`      | vtable `0x00A2B1BC` — intrusive list node #3       |
| `+0x130`        | `int`         | `0xFFFFFFFF` (`-1`)                                |
| `+0x16C`        | `void**`      | vtable `0x00A2B1BC` — intrusive list node #4       |
| `+0x184`        | `void**`      | vtable `0x00A2B1BC` — intrusive list node #5       |
| `+0x19C`        | `void**`      | vtable `0x00A2B1BC` — intrusive list node #6       |
| `+0x1A0`        | `void*`       | inter-weapon list link: → `wep[1]+0x0DC`           |

The embedded nodes (vtable `0x00A2B1BC`) form an **intrusive doubly-linked list** spanning
all weapon objects in the same Controllable.

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

**Weapon struct** (apply to all Weapon vtable types):
| Offset    | Field Name                | Type              |
|-----------|---------------------------|--------------------|
| `+0x004`  | `m_pListSentinel`         | `void*`            |
| `+0x008`  | `m_pListNext`             | `void*`            |
| `+0x00C`  | `m_pListPrev`             | `void*`            |
| `+0x060`  | `m_pWeaponClass`          | `WeaponClass*`     |
| `+0x06C`  | `m_pIntermediate`         | `void*`            |
| `+0x0C8`  | `m_nAmmoOrMode`           | `int`              |

**WeaponClass struct** (at `0x00A525F4`):
| Offset    | Field Name                | Type              |
|-----------|---------------------------|--------------------|
| `+0x018`  | `m_nHash`                 | `uint32_t`         |
| `+0x030`  | `m_szOdfName`             | `char[32]`         |
