## Character Array

| Field | Value |
|-------|-------|
| Array base ptr | `*(0xB93A08)` |
| Count | `*(0xB939F4)` |
| Stride | `0x1B0` bytes per slot |
| Access | `charSlot = arrayBase + charIndex * 0x1B0` |

---

## Character Slot Layout (`cs = charSlot`)

| Offset | Type | Description |
|--------|------|-------------|
| `cs+0x000` | `vtable*` | EntitySoldier vtable |
| `cs+0x138` | `AnimMap*` | `GetSoldierAnimationMap()` — weapon-to-stance mapping |
| `cs+0x148` | `Intermediate*` | Animation intermediate object (`im`) |

---

## Intermediate Object (`im = *(cs+0x148)`)

`ctrl = im + 0x018` points into the **same memory** as `im` (the Controllable is embedded, not a separate allocation).

| Offset | Type | Description |
|--------|------|-------------|
| `im+0x004` | `AnimNode*` | Per-character animation node (`imNode`) |
| `im+0x018` | `Controllable` (embedded) | `ctrl` pointer — same heap object |
| `im+0x01c` | `uint32_t` | Stance index (low word). `cdcd0000` = no stance set |
| `im+0x034` | `AnimBank*` | **Static global** `0x00b799d8` — shared by all characters |
| `im+0x038..+0x07F` | `uint32_t[]` | Animation handles. All become `0x0c` after `SetCharacterWeapon` write |

### Confirmed writes that stick (no visual effect alone)
- All handles `im+0x038..` → `0x0c` ✅
- Stance `im+0x01c` → `0` ✅

---

## Controllable (`ctrl = im+0x018`)

| Offset | Description |
|--------|-------------|
| `ctrl+0x000` | vtable (`0x00a403a0`, 16 entries) |
| `ctrl+0x004` | stance index (mirrors `im+0x01c`) |
| `ctrl+0x4D8 + slot*4` | `Weapon*` array (8 slots) |
| `ctrl+0x4F8 + channel` | active slot index per channel (8 bytes) |

### ctrl vtable (rebased, `0x00a403a0`)
| Index | Address | Suspected purpose |
|-------|---------|-------------------|
| 3 | `0x00409935` | Unknown — candidate for ApplyStance/reload |
| 4 | `0x00410901` | Unknown — candidate for SetAnimHandle |

---

## Weapon Instance Layout (`wpn = *(ctrl+0x4D8 + slot*4)`)

| Offset | Description |
|--------|-------------|
| `wpn+0x000` | vtable |
| `wpn+0x060` | `WeaponClass*` (`wc`) |
| `wpn+0x088` | Ordnance factory ptr (confirmed write target for ammo/projectile swap) |

---

## WeaponClass Layout (`wc = *(wpn+0x060)`)

| Offset | Type | Description |
|--------|------|-------------|
| `wc+0x004` | link | Flink (doubly-linked list, stored as `wc+0x004`; next node = `Flink - 4`) |
| `wc+0x008` | link | Blink |
| `wc+0x010` | self | List initialisation self-pointer |
| `wc+0x030` | `char[]` | ODF name string (in-place, ASCII) |
| `wc+0x050` | ptr | Odd-addressed — likely AnimBank name string ptr (`08ab044e` etc.) |
| `wc+0x064` | ptr | Ordnance class pointer |
| `wc+0x074` | `uint32_t` | **Anim stance hash** — unique per weapon type |

### Known anim hashes
| Weapon | Hash |
|--------|------|
| E-5 (blaster rifle) | `f6bcfa8e` |
| RPS-6 (rocket launcher) | `277eb5f4` |

### WeaponClass linked list traversal
```
cur = *(wpn+0x060)          // start node
loop:
  lnext = *(cur+0x008)      // Flink (raw)
  nextWC = lnext - 0x004    // actual node base
  if nextWC == startWC: done (wrapped)
```

---

## Animation Node (`imNode = *(im+0x004)`)

| Offset | Type | Description |
|--------|------|-------------|
| `imNode+0x034` | `BlendNode*` | Active blend tree root (`activeBlend`) — **kept alive, do not null** |
| `imNode+0x038 + n*4` | `ChanNode*[]` | Channel node array (up to 24 entries) |

### ChanNode layout
| Offset | Description |
|--------|-------------|
| `+0x008` | `SeqNode*` (animation sequence) |
| `+0x014` | channel ID (`uint32_t`) |

Active channel ID after `SetCharacterWeapon` write: all become `0x0c`.

---

## BlendNode (`activeBlend = *(imNode+0x034)`)

| Offset | Type | Description |
|--------|------|-------------|
| `+0x008` | `AnimSel*` | `"com_bldig_control_zone"` — **globally shared singleton** |
| `+0x010` | `StateObj*` | Per-animation-class state — **globally shared** (`0x0927fa30` for all droids) |
| `+0x014` | `uint32_t` | Blend zone channel index — **= 5 for both E-5 and RPS-6** (dead end) |
| `+0x044` | `HeapObj*` | Per-character heap pointer — **unexplored at time of writing** |

### Key negative results
- `+0x014` channel = **5 for both weapon types** → no differentiation possible by writing this field
- `+0x010` state ptr = **same global singleton** for all droid units → writing is a no-op
- `+0x008` animSel = **same global singleton** for all droid units → cannot copy from NPC

---

## AnimSel (`animSel = *(activeBlend+0x008)`)

Name string: `"com_bldig_control_zone"`

| Offset | Description |
|--------|-------------|
| `+0x010` | Self-pointer (list/init pattern) |
| `+0x014` | Null-channel leaf selector — **global**, same ptr for all droids |

---

## AnimMap (`animMap = *(cs+0x138)`)

| Offset | Type | Description |
|--------|------|-------------|
| `+0x03c` | `uint32_t` | Session-varying small int (0x0e, 0x10, 0x12) — **locomotion blend state** (walk/idle→run transition). Read breakpoint confirmed: fires on movement state change, NOT on weapon switch. Dead end for weapon animation. |
| `+0x040` | `SubObj*` | Sub-object with array of 32 ptrs (`0x099de9f4...`) |
| `+0x050` | `WC*[]` | WC pointer array — 25 registered WeaponClass pointers |
| `+0x054` | `uint32_t[]` | Parallel blend channel indices for each WC in `+0x050` |
| `+0x058` | ptr | Another sub-object |

### animMap+0x050 channel pattern (sample)
```
wc[00] ch=05  wc[01] ch=05  wc[02] ch=05
wc[03] ch=04  wc[04] ch=01  wc[05] ch=03
wc[06] ch=03  wc[07] ch=01  wc[08] ch=00  ...
```
Both E-5 and RPS-6 register under **channel 5** — confirms why blend channel write is a no-op.

The count of entries is at `*(wcList + 0x040)` (capped at 64 in code).

---

## SetCharacterWeapon — Non-Animation Writes (All Confirmed Working)

| What | How |
|------|-----|
| WeaponClass ptr | Write target `wc` to `wpn+0x060` |
| Ordnance factory | Write new factory ptr to `wpn+0x088` |
| Model / vtable | Derive from new WC, write to weapon and WC fields |
| All anim handles | Write `0x0c` to `im+0x038..+0x07f` |
| Stance index | Write `0` to `im+0x01c` |
| blendNode channel | Copy `*(srcActiveBlend+0x014)` → `*(activeBlend+0x014)` (currently no-op since both = 5) |
| blendNode state | Copy `*(srcActiveBlend+0x010)` → `*(activeBlend+0x010)` (currently no-op since both point to same global) |

---

## Animation Change — RESOLVED ✅

The fix was found by decompiling `EntitySoldier::UpdateIndirect` (0x0053b920) and reading the
disassembly around `0x0053be5c`. The full per-frame animation update path, confirmed from disasm:

```
0053be5c: MOV ECX, [ESI+0x520]        ; SoldierAnimator* from EntitySoldier
0053be66: MOV AL,  [ESI+0x512]        ; active weapon slot index (byte)
0053be79: MOV EAX, [ESI+EDX*4+0x4F0] ; Weapon* from entity's weapon array
0053be84: MOV EAX, [EAX+0xC8]        ; int MAP  ← the missing cached field
0053be8a: CMP EAX, -1                 ; -1 = INVALID_MAP, skip if so
0053be90: CALL 0x004170d5             ; SoldierAnimator::SetWeaponAnimationMap(ECX, MAP)
```

**Root cause:** `wpn+0xC8` is the cached animation MAP integer for a weapon instance.
`SetCharacterWeapon` only wrote `wpn+0x060/064/068` (WeaponClass*) and `wpn+0x088` (factory),
so `UpdateIndirect` kept re-applying the old weapon's MAP every frame.

**Fix implemented in `lua_funcs.cpp`:**
- Copy `newMap = *(sourceWpn + 0xC8)` from the source weapon (same type on another character)
- Write `*(wpn + 0xC8) = newMap` — keeps all future frames correct
- Get `entity = *(ctrl + 0x290)`, `soldierAnimator = *(entity + 0x520)`
- Call `SetWeaponAnimationMap(soldierAnimator, newMap)` at `0x004170D5` for immediate visual switch

### Additional confirmed EntitySoldier fields (ESI = ctrl+0x290, NOT the charSlot)
| Offset | Field |
|--------|-------|
| +0x4F0 | Weapon*[8] array (rendering-side weapon slots) |
| +0x512 | uint8 active slot index (channel 0) |
| +0x520 | SoldierAnimator* |

### Additional confirmed Weapon instance field
| Offset | Field |
|--------|-------|
| +0xC8  | int MAP (animation map ID, -1 = INVALID) |

### Remaining open question
When `sourceWpn` is not found (no other character has the target weapon), the MAP cannot
be copied. `FUN_00570760(BANK, WEAPON_type)` can look it up from the global registration
table at `0xacf558/0xacf55c`, but the WEAPON type field within WeaponClass is not yet
confirmed (possibly `WeaponClass+0x020` based on `FUN_006740d0` decompile).

---

## AnimBank (`*(im+0x034) = 0x00b799d8`)

Static global pointer shared by all active characters. Dumps `+0x000..+0x07F` showed it is a fixed-size table — not per-character.

---

## Per-Frame Animation Subscriber — `AnimSubscriber_Update` (`0x0055f080`)

Discovered via hardware write breakpoint on `ctrl+0x4F8 + 0x510` during weapon switch.

The write to `ctrl+0x4F8` comes from vtable[1] of a subscriber registered in the tick broadcaster
primary list (`g_tickSubscribers_primary` at `0xb6a704`). The subscriber receives **delta time**
(a float, `[0x00c6a9b0]` ≈ `0x3C4CB1F5` ≈ 0.0125s) — NOT an entity/character pointer.

The full call chain each frame:
```
GameLoop → TickBroadcaster_Dispatch(deltaTime, 0, 0)   [0x0048fda0]
  → TickBroadcaster_BroadcastList(primaryList, dt)      [0x0048fcd0]
    → TickBroadcaster_NotifyChain(node, dt)             [0x0048fc70]
      → AnimSubscriber_Update(dt)                       [0x0055f080]  ← writes ctrl+0x4F8
```

**`0x0055f080` is the critical function to decompile.** It reads some state to determine which
weapon slot/animation should be active, then writes `ctrl+0x4F8`. Understanding what it reads
will reveal the missing write in `SetCharacterWeapon`.

### What we know it does NOT read directly
- Breakpoint on `ctrl+0x4F8` fires every frame — the slot index is re-evaluated from internal state
- Swapping `wpn+0x060` (WeaponClass ptr) does not affect the visual output → it does not read this
- `animMap+0x03c` is not written during switch → likely read-only input consumed by this function

---

## GameLog function

`fn_GameLog = res(0x7E3D50)` — standard BF2 log. Output appears in the game console/log file.
