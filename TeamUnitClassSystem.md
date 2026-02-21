# Team Unit Class System — Reverse Engineering Notes

Everything documented here was reverse-engineered from `BF2_modtools.exe` using Ghidra and x64dbg.
All addresses are **unrelocated** (imagebase = `0x400000`). At runtime, resolve via:

```cpp
uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
void* resolved = (void*)((unrelocated_addr - 0x400000u) + base);
```

---

## Data Structure Overview

Each team stores its unit classes in a **Structure-of-Arrays (SoA)** layout — three separate
heap-allocated arrays that run in parallel, all indexed by slot number:

```
Team object layout (confirmed offsets):
  team + 0x44  →  int   classCapacity    (max slots — pre-allocated)
  team + 0x48  →  int   classCount       (live entries; 0..classCapacity)
  team + 0x50  →  void** classDefArr     (pointer to array of ClassDef*)
  team + 0x54  →  int*   minUnitsArr     (pointer to array of min counts)
  team + 0x58  →  int*   maxUnitsArr     (pointer to array of max counts)
```

All three arrays (`classDefArr`, `minUnitsArr`, `maxUnitsArr`) are pre-allocated to
`classCapacity` slots. They are **not** resized at runtime. The game hard-codes a
capacity of **10** in 5 patch sites (see `class_limit.cpp`); BF2GameExt raises this to 20.

**Key insight**: This is NOT a linked list and NOT a Java-style ArrayList. The backing
storage is fixed. `classCount` is simply a cursor. Removing a class left-shifts the
remaining entries and decrements `classCount`, freeing the last slot immediately. Adding
a class appends at `classDefArr[classCount]` and increments `classCount`. You can freely
add and remove as long as `classCount` never exceeds `classCapacity` at any single moment.

---

## Global Data

### `g_ppTeams` — Team Array Pointer Variable

```
Address: 0xAD5D64  (pointer variable — value = base of team pointer array)
```

**Two dereferences** are required to get a team object:

```cpp
uintptr_t teamArrayBase = *(uintptr_t*)res(0xAD5D64);   // dereference 1: get array base
void*     teamPtr       = *(void**)(teamArrayBase + teamIndex * 4);  // dereference 2: get Team*
```

There are 8 team slots (indices 0–7). A null `teamPtr` means the team is inactive.

### `g_ClassDefList` — Global Class Definition Registry

```
Address: 0xACD2C8  (pointer to head node of linked list)
```

This is a **singly-linked list** of all `ClassDef` objects loaded from `.req` files.
A class must appear in this global registry before it can be assigned to any team —
this is why the vanilla error says *"check the side's .req file"*.

Node layout:
```
node + 0x00  →  (unused / vtable or other)
node + 0x04  →  node*   next           (nullptr = end of list)
node + 0x0c  →  void*   classDef       (nullptr = end of list sentinel)
```

ClassDef layout (relevant fields only):
```
classDef + 0x18  →  int   nameHash     (NOT a char* — integer hash produced by HashString)
```

**Critical**: `classDef + 0x18` is an **integer hash**, not a string pointer. Treating it
as `char*` and calling `strcmp` will crash immediately (access violation in strcmp).
Always use `HashString` to compare names (see below).

---

## Key Functions

### `HashString` — FUN_007e1bd0

Hashes a class name string to an integer. Uses `__thiscall` with an 8-byte stack buffer
as `this` (ECX). The first `int` of the output buffer is the hash.

```cpp
typedef void* (__thiscall* HashString_t)(void* buf, const char* name);
HashString_t fn_HashString = (HashString_t)res(0x7E1BD0);

alignas(4) int hashBuf[2] = {};
fn_HashString(hashBuf, "imp_inf_trooper");
int hash = hashBuf[0];  // use this to compare against classDef+0x18
```

### `Team::AddClassByName` — thunk at 0x662DF0

Searches `g_ClassDefList` by hash, then calls `AddClassByDef`. Emits vanilla error if
not found: *"Team missing class X (check the side's .req file)"*.
Does **not** create new ClassDef objects.

### `Team::AddClassByDef` — thunk at 0x662DA0

Appends `classDef` to the team's SoA arrays:
```
classDefArr[classCount] = classDef
minUnitsArr[classCount] = min
maxUnitsArr[classCount] = max
classCount++
```
Enforces `classCapacity` limit (vanilla = 10; BF2GameExt patches to 20).

### `Team::SetUnitClassMinMax` — thunk at 0x662C20

Writes `min`/`max` into the parallel arrays at the given slot and fires
`OnUnitClassChanged` (thunk at 0x661E00) to notify the game.

```cpp
typedef void (__thiscall* SetUnitClassMinMax_t)(void* team, int slot, int min, int max);
```

Called by both `AddClassByDef` (on insertion) and `RemoveUnitClass` (on each shifted slot).

### `Team::GetClassMinUnits` — thunk at 0x6617B0
### `Team::GetClassMaxUnits` — thunk at 0x6617A0

Read the min/max values from the parallel arrays for a given slot.

### `Team::SetClassUnitCount` — FUN_00663020 (thunk at 0x663020)

Updates min/max for an already-assigned class. Called by `AddUnitClass` when the class
is already on the team (just update the counts, don't add a new slot).
Vanilla error: *"Team::SetClassUnitCount(): team_m..."*

### `Team::CopyAllFromTemplate` — FUN_006633C0

Iterates all 8 teams (step 4 bytes each) and copies class lists from a template team.
Uses `GetClassMinUnits` / `GetClassMaxUnits` / `AddClassByName` internally.

### `GetClassDefByName` — FUN_00662FC0

Searches the team's own `classDefArr` (not the global list) by hash.
Returns the `classDef` pointer if found in the team, null otherwise.
Called by `AddUnitClass` to decide which path to take (add new vs. update existing).

### Game Logger — FUN_007e3d50

Printf-style debug logger. Same function the vanilla game uses for its own error messages.

```cpp
typedef void (__cdecl* GameLog_t)(const char* fmt, ...);
GameLog_t fn_GameLog = (GameLog_t)res(0x7E3D50);
fn_GameLog("MyFunc(): something went wrong: %s\n", className);
```

---

## `AddUnitClass` — FUN_0046bad0

The vanilla Lua function exposed to scripts. Two code paths:

1. **Class not yet on team** (`GetClassDefByName` returns null):
   Calls `Team::AddClassByName` → finds in `g_ClassDefList` → calls `AddClassByDef`.

2. **Class already on team** (`GetClassDefByName` returns non-null):
   Calls `Team::SetClassUnitCount` to update min/max without adding a new slot.

Enforces max 12 classes (vanilla; BF2GameExt patches this to 20).

---

## `RemoveUnitClass` — BF2GameExt addition

Inverse of `AddUnitClass`. Algorithm:

1. **Resolve team pointer** — double-dereference `g_ppTeams` as shown above.
2. **Hash the class name** — call `HashString` to get the integer hash.
3. **Walk `g_ClassDefList`** — find the `ClassDef*` whose `nameHash` matches.
4. **Search team's `classDefArr`** — find the slot index by pointer comparison.
5. **Left-shift removal** — for each slot `i` from `foundSlot` to `classCount-2`:
   ```
   classDefArr[i] = classDefArr[i+1]
   SetUnitClassMinMax(team, i, minArr[i+1], maxArr[i+1])
   ```
   `SetUnitClassMinMax` must be called (not a raw write) so `OnUnitClassChanged` fires.
6. **Clear last slot** — zero `classDefArr[lastSlot]`, `minArr[lastSlot]`, `maxArr[lastSlot]`.
7. **Decrement count** — `*(int*)(teamPtr + 0x48) = classCount - 1`.

**Why left-shift, not swap-and-pop?**
Swap-and-pop moves the last element into the removed slot, changing the visible order of
unit classes in the spawn menu. Left-shift preserves order exactly as the player sees it.

---

## Class Capacity Patch — `class_limit.cpp` ⚠️ NOT CURRENTLY ACTIVE

> **Status: incomplete / disabled.** The 5 instruction-level patch sites below only raise
> the guard limit used by `AddClassByDef`. The three parallel arrays (`classDefArr`,
> `minUnitsArr`, `maxUnitsArr`) are also allocated to a **fixed compile-time size** at
> startup — those allocations must be found and patched (or intercepted at allocation time)
> before the limit can safely be raised above 10. Until that is done, calling `AddUnitClass`
> beyond slot 9 will write out of bounds into adjacent memory.

The hardcoded limit of 10 classes per team appears in 5 instruction sites:

| Address    | Instruction                              |
|------------|------------------------------------------|
| 0x0068A5CF | `83 FB 0A` — `cmp ebx, 0A`              |
| 0x0068A5EF | `6A 0A`    — `push 0A`                  |
| 0x0068A5FC | `C7 44 24 30 0A 00 00 00` — `mov [esp+30], 0A` |
| 0x0068A6C9 | `83 FB 0A` — `cmp ebx, 0A`              |
| 0x0068A6CE | `BF 0A 00 00 00` — `mov edi, 0A`        |

`patch_class_limit()` overwrites the single `0A` byte at each site with `new_limit`.
Patching these alone is not sufficient — the backing arrays must also be enlarged.
Still needs: locate the two fixed array allocations (`classDefArr` and the min/max arrays)
and either patch their sizes or intercept `malloc`/`new` at startup to enlarge them.

---

## `GetCharacterWeapon` — ⚠️ NOT CURRENTLY WORKING

> **Status: broken / not maintained.** `lua_GetCharacterWeapon` in `lua_funcs.cpp` reads
> `weapon + 0x18` as a `util::BaseArray<char>` whose first field is a `char*` ODF name.
> The offset and/or the Weapon layout assumption have not been confirmed against a live
> game session. The function compiles and registers fine, but returns garbage or nil in
> practice. Do not rely on it until the Weapon struct layout is verified in Ghidra.

---

## Debugging Notes

- **Access violation in `strcmp`/`stricmp`**: You read `classDef + 0x18` as a `char*`.
  It's an `int`. Use `HashString` instead.

- **Access violation reading `teamPtr + 0x48`**: You single-dereferenced `g_ppTeams`.
  It's a pointer variable, not the array base. Double-dereference as shown above.

- **Hero appears in trooper's slot after remove**: You used swap-and-pop.
  Use left-shift to preserve order.

- **Class not found in global registry**: The ODF name must be loaded via a `.req` file
  for the side. The global `g_ClassDefList` is populated at load time, not at script time.
