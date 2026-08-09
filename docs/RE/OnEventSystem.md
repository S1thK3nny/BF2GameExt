# BF2 Lua Event System — Technical Reference

> Target: `BF2_modtools.exe` (BF2 2005), x86, imagebase `0x400000`
> All addresses are unrelocated. Runtime resolution:
> ```cpp
> void* res(uintptr_t a) { return (void*)((a - 0x400000u) + (uintptr_t)GetModuleHandleW(nullptr)); }
> ```

---

## Overview

BF2 exposes named Lua events (`OnCharacterDeath`, `OnCharacterEnterVehicle`, etc.) through a
uniform event descriptor infrastructure. Each event is represented by a 24-byte **EventDescriptor**
in `.data`, registered at startup by `FUN_0079d750`. Five Lua globals are registered per event:

```
OnXxx(callback)           → registers a plain callback
OnXxxName(callback, str)  → registers a name-filtered callback
OnXxxTeam(callback, idx)  → registers a team-filtered callback
OnXxxClass(callback, str) → registers a class-filtered callback
ReleaseXxx(handle)        → unregisters a callback handle
```

All five variants share the same C handler functions, differentiated only by an upvalue carrying
the event's descriptor pointer.

---

## EventDescriptor Layout (24 bytes / 0x18)

Memory layout of the `CharacterEnterVehicle` descriptor at `0x00ADF414`:
```
70 AF A6 00  D0 AC A6 00  07 00 00 00  00 00 00 00  [?? ?? ?? ??  ?? ?? ?? ??]
```

| Offset  | Type         | Value (CEV example) | Description                                                         |
|---------|--------------|---------------------|---------------------------------------------------------------------|
| `+0x00` | `const char*`| `0x00A6AF70`        | Pointer to event base-name string                                   |
| `+0x04` | `void*`      | `0x00A6ACD0`        | Static type-handler vtable ptr (shared by all vehicle-class events) |
| `+0x08` | `uint32_t`   | `7`                 | Event ID integer (0 for custom events)                              |
| `+0x0C` | `uint32_t`   | `0`                 | Padding / unknown                                                   |
| `+0x10` | `void*`      | `nullptr` → filled  | **listHead** — head of registered callback nodes                    |
| `+0x14` | `void*`      | `0`                 | Unknown                                                             |

Descriptors are spaced **0x18 bytes** apart in `.data`.
Example: `CharacterLandedFlyer` at `0x00ADF3FC`, `CharacterEnterVehicle` at `0x00ADF414`.

The type-handler vtable at `0x00A6ACD0`:
```
vtbl[0] = 0x00411a68   AND byte ptr [ECX+4], 0xfd  (clear bit 1 of node flags)
vtbl[1] = 0x00415f0a → FUN_00798780  (node constructor/state transition)
vtbl[2] = 0x00407dbf
vtbl[3] = 0x0040b4ec
```

This vtable is shared by at least 4 events. It is a type identifier, not the fire/dispatch function.

---

## Registrar Thunks (unrelocated addresses)

Each thunk takes `(const char** keyNamePtr, void* cHandler)` as `__cdecl` args.
`keyNamePtr = &descriptor.namePtr` (= descriptor base address, since `namePtr` is at offset 0).
Each thunk constructs a prefixed name string and calls `thunk_FUN_004869d0` to install the global.

| Thunk address | Builds Lua global name          |
|---------------|---------------------------------|
| `0x413043`    | `"Release"` + `*param_1`        |
| `0x41402E`    | `"On"` + `*param_1`             |
| `0x40CE8C`    | `"On"` + `*param_1` + `"Name"`  |
| `0x401B4A`    | `"On"` + `*param_1` + `"Team"`  |
| `0x416FBD`    | `"On"` + `*param_1` + `"Class"` |

---

## Shared C Handler Addresses (unrelocated)

These C functions are registered as Lua closures with `descriptor.namePtr` as upvalue.
The same function address is reused for every event — the upvalue routes it to the correct list.

| Address    | Registered for                   |
|------------|----------------------------------|
| `0x4135D9` | `OnXxx(callback)`                |
| `0x41253A` | `OnXxxName(callback, nameStr)`   |
| `0x4061B8` | `OnXxxTeam(callback, teamIdx)`   |
| `0x40685C` | `OnXxxClass(callback, classStr)` |
| `0x4109D3` | `ReleaseXxx(handle)`             |

---

## Callback Registration Flow

When a script calls `OnCharacterXxx(callback)` (handler `0x4135D9`):

1. Reads upvalue 1 (`lua_upvalueindex(1)` = −10002) → descriptor pointer (`descPtr`)
2. Verifies arg 1 is a Lua function (type 6)
3. `lua_pushvalue(L, 1)` — copies the callback onto the stack
4. `luaL_ref(L, LUA_REGISTRYINDEX)` → stores the callback in the Lua registry, returns `int ref`
5. `malloc(0x18)` → allocates a 24-byte callback node
6. `thunk_FUN_00798cc0(ref)` → constructs the node (sets vtable, embeds ref)
7. `thunk_FUN_00798120(node + 4)` → links the node into `descriptor.listHead` (`+0x10`)
8. Returns the node pointer as a Lua light userdata (the **release handle**)

---

## Callback Node Layout (24 bytes)

```
node @ 0x09d18540:
  +0x00 = 0x00a6ad44   vtable ptr
  +0x04 = 0x00000007   flags (bits 0–1 cleared by vtbl[0] / vtbl[1])
  +0x08 = 0x6149f008   back-pointer to &descriptor+0x04 (owning descriptor)
  +0x0C = 0x00000000   next node pointer (null = end of list)
  +0x10 = 0x00000000   (null)
  +0x14 = 0x0000001a   luaL_ref = 26  (Lua registry index of the stored callback)
  +0x18 = 0x00000000
  +0x1C = 0x00000000
```

Key fields:
- `node+0x0C` — **next pointer** (null-terminated singly-linked list)
- `node+0x14` — **luaL_ref** (`−1` = `LUA_NOREF`; skip the node)
- `node+0x04` — **flags** (do not use as ref)

---

## Walking the List and Firing Callbacks

```cpp
void* node = descriptor.listHead;   // descriptor+0x10
while (node) {
    const int ref  = *(int*)((char*)node + 0x14);
    void*     next = *(void**)((char*)node + 0x0C);
    if (ref != -1) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);  // push callback
        // push event-specific args here
        lua_pcall(L, nargs, 0, 0);
    }
    node = next;
}
```

`lua_rawgeti`: `0x7B8810`, `__cdecl(lua_State* L, int idx, int n)`.

> **Note:** `0x7B87C0` is `lua_rawget(L, idx)` — a 2-argument function that pops a key from
> the stack top. It is **not** `lua_rawgeti` and must not be called with 3 arguments.

`LUA_REGISTRYINDEX` = `−10000`.

---

## Adding a New Event

1. **Declare the descriptor** in DLL `.data`:
   ```cpp
   static const char         s_myEventName[] = "CharacterMyEvent";
   static BF2EventDescriptor g_myEventDesc   = { s_myEventName, nullptr, 0, 0, nullptr, nullptr };
   ```

2. **Patch typeHandler** at runtime (required for `thunk_FUN_00798120` to link nodes correctly):
   ```cpp
   g_myEventDesc.typeHandler = res(0xA6ACD0);   // shared vehicle-class type handler
   ```

3. **Register from `hooked_init_state`:**
   ```cpp
   const char** key = &g_myEventDesc.namePtr;
   reg_on     (key, res(0x4135D9));
   reg_name   (key, res(0x41253A));
   reg_team   (key, res(0x4061B8));
   reg_class  (key, res(0x40685C));
   reg_release(key, res(0x4109D3));
   ```

4. **Hook the C++ event source** and walk `g_myEventDesc.listHead` to fire callbacks.

---

## OnCharacterExitVehicle

### Hook

`FUN_0052FC70` (unrelocated) is hooked via Microsoft Detours. The game function is `__thiscall`;
the hook is declared `__fastcall` to match the ABI: `(void* ecx, void* edx, int arg1, int arg2)`.

The hook scans the character array **before** calling the original because the original clears
vehicle state. `ecx` is the character's controllable (`EntitySoldier + 0x240`), not the
character struct itself; the character index is resolved by array scan, not read from args.

### Dispatch Sequence

1. Read `mCharacterStructArray` base from `0xB93A08`; slot count from `0xB939F4`.
2. Scan for the slot where `slot+0x148 == ecx`.
3. From the matched slot, resolve:
   - `charIndex`      = loop index `i`
   - `vehicleCtrl`    = `*(slot + 0x14C)`
   - `charTeam`       = `*(int*)(slot + 0x134)`
   - `entityNameHash` = `*(uint32_t*)(entitySoldier + 0x004)` (EntityEx::mId)
   - `entityClassPtr` = `*(void**)(entitySoldier + 0x008)` (EntityEx::mEntityClass)
4. Fire all matching callbacks from `g_cevCallbacks[]` (see filter types below).
5. Call `original_char_exit_vehicle(ecx, nullptr, arg1, arg2)`.

### Filter Types

| Type        | Match condition                               |
|-------------|-----------------------------------------------|
| `CEV_PLAIN` | Always fires                                  |
| `CEV_NAME`  | `entityNameHash == callback.nameHash`         |
| `CEV_TEAM`  | `charTeam == callback.teamFilter`             |
| `CEV_CLASS` | `entityClassPtr == callback.classPtr`         |

### Lua API

```lua
-- Plain: fires for every character vehicle exit
local h = OnCharacterExitVehicle(function(charIndex, vehicle) ... end)

-- Name-filtered: fires only when the exiting character's instance name matches
local h = OnCharacterExitVehicleName(function(charIndex, vehicle) ... end, "unit_instance_name")

-- Team-filtered: fires only for characters on the given team (0-based index)
local h = OnCharacterExitVehicleTeam(function(charIndex, vehicle) ... end, 0)

-- Class-filtered: fires only when the character's entity class matches
local h = OnCharacterExitVehicleClass(function(charIndex, vehicle) ... end, "imp_inf_trooper")

-- Release a callback
ReleaseCharacterExitVehicle(h)
h = nil
```

Callback arguments:
- `charIndex` (`number`) — character unit index, 0-based integer, compatible with all native `GetCharacter*` functions
- `vehicle` (`userdata`) — light userdata pointer to the vehicle entity (`vehicleCtrl − 0x240`), or `nil` if the vehicle pointer was not resolved

### Callback Storage

Callbacks are stored in `g_cevCallbacks[CEV_MAX_CBS]` (64 slots) in DLL `.data`. The Lua function
is stored in the Lua globals table (`LUA_GLOBALSINDEX`, index `−10001`) under a negative integer
key (`g_cevNextKey`, starting at `−1000` and decrementing per registration). The handle returned
to Lua is a light userdata pointing to the `CEVCallback` slot. `ReleaseCharacterExitVehicle(h)`
nils the globals entry and zeroes the slot, making it available for reuse.

---

## Lua API Address Reference

| Function             | Address    | Signature                                                 | Notes                                                                   |
|----------------------|------------|-----------------------------------------------------------|-------------------------------------------------------------------------|
| `lua_rawgeti`        | `0x7B8810` | `void(lua_State* L, int idx, int n)`                      | Confirmed `__cdecl`; verified from `FUN_007b73e0` + `FUN_00486a30`      |
| `lua_rawget`         | `0x7B87C0` | `void(lua_State* L, int idx)`                             | 2-arg; pops key from top. **Not** `lua_rawgeti`                         |
| `lua_pcall`          | `0x7B8B60` | `int(lua_State* L, int nargs, int nresults, int errfunc)` |                                                                         |
| `lua_pushcclosure`   | `0x7B86A0` | `void(lua_State* L, lua_CFunction fn, int n)`             |                                                                         |
| `lua_pushlstring`    | `0x7B8580` | `void(lua_State* L, const char* s, size_t len)`           |                                                                         |
| `lua_settable`       | `0x7B8960` | `void(lua_State* L, int idx)`                             |                                                                         |
| `lua_insert`         | `0x7B7F20` | `void(lua_State* L, int idx)`                             | Confirmed from `thunk_FUN_004869d0` (lua_setglobal macro expansion)     |
| `luaL_ref`           | `0x7B73E0` | `int(lua_State* L, int t)`                                | Standard freelist implementation; calls `FUN_007b8810` internally       |
| `LUA_REGISTRYINDEX`  | —          | `−10000`                                                  |                                                                         |
| `LUA_GLOBALSINDEX`   | —          | `−10001`                                                  |                                                                         |
| `lua_upvalueindex(1)`| —          | `−10002`                                                  | Used to fetch the descriptor upvalue in shared C handlers               |
| `LUA_NOREF`          | —          | `−1`                                                      | `node[+0x14] == −1` → skip node                                         |

---

## Appendix: Locating Hook Points with x32dbg

To capture a call stack at the vehicle exit event:

1. Attach x32dbg to the running game.
2. Navigate to address `0x0052FC70` (Ctrl+G → `52FC70`).
3. Set a software breakpoint (F2).
4. Trigger a vehicle exit in-game.
5. When the breakpoint fires, open **View > Call Stack** (Alt+K) to inspect the full call chain.
6. If the breakpoint does not fire, set a hardware **write** breakpoint on `*(charSlot + 0x140)`
   (the vehicle pointer field). It will break the instant that field is cleared during the exit
   transition, revealing the call chain regardless of which code path triggered it.
