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

| Finding                                              | Status        |
|------------------------------------------------------|---------------|
| charIndex → charSlot → intermediate → Controllable   | ✅ Confirmed  |
| Weapon slot array in Controllable (`+0x4D8`)         | ✅ Confirmed  |
| WeaponClass pointer in Weapon (`+0x060`)             | ✅ Confirmed  |
| ODF name location in WeaponClass (`+0x30`)           | ✅ Confirmed  |
| Active/in-hand weapon index tracking                 | ❓ Unknown    |

---

## Full Resolution Chain

```
mCharacterStructArray + charIndex * 0x1B0
         ↓  charSlot
*(charSlot + 0x148)
         ↓  intermediate
intermediate + 0x18
         ↓  Controllable*  (vtable 0x00A403A0)
*(Controllable + 0x4D8 + slotIndex * 4)
         ↓  Weapon*        (vtable 0x00A52468 or 0x00A53510)
*(Weapon + 0x060)
         ↓  WeaponClass*   (vtable 0x00A525F4)
WeaponClass + 0x30
         ↓  char[]  ← ODF name, inline, null-terminated
```

**Runtime-confirmed example**: `"rep_weap_dc-15s_blaster_carbine"`
(Republic DC-15S Blaster Carbine — default clone trooper primary weapon)

---

## C++ Implementation

```cpp
// res(addr) = unrelocated addr → runtime addr
uintptr_t arrayBase = *(uintptr_t*)res(0xB93A08);
char* charSlot      = (char*)arrayBase + charIndex * 0x1B0;
char* intermediate  = *(char**)(charSlot + 0x148);
char* ctrl          = intermediate + 0x18;                       // Controllable*
char* weapon        = *(char**)(ctrl + 0x4D8 + slotIndex * 4);  // Weapon*
char* wepClass      = *(char**)(weapon + 0x060);                 // WeaponClass*
const char* odfName = wepClass + 0x30;                           // ODF name string
```

---

## Global Variables

| Address    | Type          | Description                                         |
|------------|---------------|-----------------------------------------------------|
| `0xB93A08` | `void**`      | `mCharacterStructArray` — pointer to slot array base |
| `0xB939F4` | `int`         | `MaxCharacterCount` — validate charIndex before use  |

---

## Controllable Layout (Confirmed Fields)

```
Controllable vtable: 0x00A403A0
Reachable as: intermediate + 0x18  ≡  entity + 0x258
```

| Offset  | Type       | Description                                                  |
|---------|------------|--------------------------------------------------------------|
| `+0x000` | `void**`  | vtable                                                       |
| `+0x084` | `void*`   | embedded sub-object (vtable `0x00A3B0AC`)                    |
| `+0x09C` | `void*`   | points to `ctrl - 0x18` (= intermediate)                     |
| `+0x0A0` | `void*`   | points near `ctrl - 0x68`                                    |
| `+0x174` | `void*`   | embedded sub-object (vtable `0x00A40664`, near Entity vtable) |
| `+0x188` | `void*`   | self-pointer `= &ctrl+0x188` (circular list head)            |
| `+0x290` | `void*`   | back-pointer to Entity `= entity`                            |
| `+0x4C0` | `Weapon*` | **always = slot[0]** (NOT the active weapon — see below)     |
| `+0x4D8` | `Weapon*` | weapon slot array — slot[0] (primary weapon)                 |
| `+0x4DC` | `Weapon*` | weapon slot array — slot[1] (secondary/alternate)            |
| `+0x4E0` | `Weapon*` | weapon slot array — slot[2]                                  |
| `+0x4F8` | `int`     | `0x100` observed — possibly weapon count or capacity         |
| `+0x740` | `int`     | `6` observed                                                 |
| `+0x744` | `int`     | `8` observed — possibly max weapon slot count                |
| `+0x764` | `int`     | `7` observed                                                 |

**`ctrl+0x4C0`** — confirmed to always equal `ctrl+0x4D8` (slot[0]) regardless of equipped weapon.
It is NOT the active weapon pointer. The field tracking which weapon is currently in-hand has not
yet been identified.

**`ctrl+0x9D0..+0x9FC`** — always zero. The Ghidra `GetAimTurnRate` listing reads
`[ESI + EAX*4 + 0x9D0]` with label "GetWeaponPtr", but `ESI` at that instruction is NOT the
Controllable — it is a sub-object loaded earlier in the function. Do not use this offset on ctrl.

---

## Weapon Object Layout (Confirmed Fields)

Each weapon slot is `0x200` bytes, contiguous in memory.

```
Weapon vtable: 0x00A52468  (primary slot type)
               0x00A53510  (secondary slot type — observed once)
```

| Offset  | Type         | Description                                              |
|---------|--------------|----------------------------------------------------------|
| `+0x000` | `void**`    | vtable                                                   |
| `+0x004` | `void*`     | pointer to list sentinel's `field4` (shared by all weapons in same Controllable) |
| `+0x008` | `void*`     | `&next_weapon.field4` — outer doubly-linked list: next   |
| `+0x00C` | `void*`     | `&prev_weapon.field4` — outer doubly-linked list: prev   |
| `+0x010` | `Weapon*`   | self-pointer (inner list: `next = self` when empty)      |
| `+0x014` | `Weapon*`   | self-pointer (inner list: `prev = self` when empty)      |
| `+0x018..+0x05F` | —  | uninitialized (`0xCDCDCDCD`) in observed test runs      |
| `+0x060` | `WeaponClass*` | **WeaponClass** — same value at `+0x064` and `+0x068`|
| `+0x06C` | `void*`     | back-pointer to `intermediate` (= `ctrl - 0x18`)         |
| `+0x070` | `void*`     | pointer near ctrl                                        |
| `+0x074` | `void*`     | pointer near `ctrl + 0x20`                               |
| `+0x078` | `void*`     | pointer near `ctrl + 0x28`                               |
| `+0x0B8` | `float`     | `1.0f`                                                   |
| `+0x0BC` | `float`     | `2.5f`                                                   |
| `+0x0C0` | `float`     | `2.5f`                                                   |
| `+0x0C8` | `int`       | `5` — possibly ammo count or weapon mode                 |
| `+0x0D8` | `void**`    | vtable `0x00A2B1BC` — embedded intrusive list node #1    |
| `+0x0EC` | `void**`    | vtable `0x00A2B1BC` — embedded intrusive list node #2    |
| `+0x100` | `void**`    | vtable `0x00A2B1BC` — embedded intrusive list node #3    |
| `+0x130` | `int`       | `0xFFFFFFFF` (`-1`)                                      |
| `+0x16C` | `void**`    | vtable `0x00A2B1BC` — embedded intrusive list node #4    |
| `+0x184` | `void**`    | vtable `0x00A2B1BC` — embedded intrusive list node #5    |
| `+0x19C` | `void**`    | vtable `0x00A2B1BC` — embedded intrusive list node #6    |
| `+0x1A0` | `void*`     | inter-weapon list link: → `wep[1]+0x0DC` (chain confirmed) |

The embedded nodes (vtable `0x00A2B1BC`) form an **intrusive doubly-linked list** that spans
across all weapon objects in the slot array.

---

## WeaponClass Object (Confirmed)

Prototype/class-definition object — one per weapon ODF type, shared across all instances.

```
WeaponClass vtable: 0x00A525F4
```

| Offset  | Type         | Description                                              |
|---------|--------------|----------------------------------------------------------|
| `+0x000` | `void**`    | vtable                                                   |
| `+0x004` | `void*`     | pointer to a static/global data table                    |
| `+0x008` | `void*`     | linked list: next                                        |
| `+0x00C` | `void*`     | linked list: prev                                        |
| `+0x010` | `WeaponClass*` | self-pointer (circular list head)                     |
| `+0x014` | `WeaponClass*` | linked list                                           |
| `+0x018` | `uint32`    | hash / checksum                                          |
| `+0x01C` | `int`       | `0x1E` (30) — possibly weapon type enum or capacity      |
| `+0x024` | `float`     | small float                                              |
| `+0x028` | `float`     | small float                                              |
| `+0x02C` | `float`     | small float                                              |
| **`+0x030`** | **`char[]`** | **ODF name — inline, null-terminated string ← READ HERE** |
| `+0x050` | `void*`     | heap sub-object pointer                                  |
| `+0x054` | `uint32`    | hash (same as `+0x018` in observed run)                  |
| `+0x060` | `WeaponClass*` | self-pointer again                                    |
| `+0x064` | `WeaponClass*` | linked list                                           |
| `+0x068` | `WeaponClass*` | linked list                                           |
| `+0x078` | `void*`     | heap pointer                                             |

**Reading the ODF name:**

```cpp
const char* odfName = wepClass + 0x30;  // use strlen/strcmp directly — null-terminated
```

**Confirmed runtime example**: `"rep_weap_dc-15s_blaster_carbine"`

---

## Active Weapon — Open Problem

`ctrl+0x4C0` is always equal to `ctrl+0x4D8` (slot[0]). It does NOT update when the
player switches weapons in-game. The field that tracks the currently equipped weapon
index (or a pointer that changes on weapon switch) has not yet been identified.

**Known hints from Ghidra:**
- Label "GetWeaponIndex" appears near the `[ESI + EAX*4 + 0x9D0]` instruction in
  `Controllable::GetAimTurnRate` — `EAX` at that point is the result of calling
  GetWeaponIndex somewhere above. Follow the GetWeaponIndex call upward to find what
  it reads to produce the index.
- `DAT_00b7d824` (`0x00B7D824`) — observed = 0 in all tests. NOT the per-character
  current weapon index.

**To find the active weapon index:**
1. In Ghidra, locate `Controllable::GetAimTurnRate` (near `0x004E0340`).
2. Trace backwards from the `MOV ECX, [ESI + EAX*4 + 0x9D0]` instruction to where
   `EAX` is computed — that call is `GetWeaponIndex` or equivalent.
3. What `GetWeaponIndex` reads from `this` is the current weapon index field.
4. Alternatively: xref-search for writes to `ctrl+0x4D8..+0x4E0` range to find the
   weapon-switching code; the same code likely updates an index field.

---

## What Has NOT Worked

| Approach                              | Why it failed                                     |
|---------------------------------------|---------------------------------------------------|
| `ctrl + 0x9D0` as weapon array        | Always zero. `ESI` in GetAimTurnRate ≠ Controllable |
| `intermediate+0x18` vtable = `0x00A5A6E0` | That's **SoldierElement** (rendering/particles), not weapons |
| `SoldierElement + 0x10C..+0x12C`      | Inside particle component, not weapon data         |
| `ctrl + 0x4C0` as "active weapon"     | Always equals slot[0], does not update on switch   |

---

## Ghidra Labels to Apply

| Address      | Suggested Label                                           |
|--------------|-----------------------------------------------------------|
| `0x00A403A0` | `Controllable_vftable`                                    |
| `0x00A40500` | `Entity_vftable`                                          |
| `0x00A52468` | `Weapon_vftable`                                          |
| `0x00A53510` | `Weapon2_vftable` (secondary weapon class)                |
| `0x00A525F4` | `WeaponClass_vftable`                                     |
| `0x00A2B1BC` | `WeaponListNode_vftable` (embedded intrusive list node)   |
| `0x00A5A6E0` | `SoldierElement_vftable` (rendering/particle — NOT weapons) |
| `0x00A58E20` | `WeaponSentinel_vftable` (sentinel sub-object)            |
| `0xB93A08`   | `g_mCharacterStructArray`                                 |
| `0xB939F4`   | `g_MaxCharacterCount`                                     |
