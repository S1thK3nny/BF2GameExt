# Command post vehicle list (BF1 spawn screen feature)

The BF1 spawn map showed, for the highlighted command post, a text line listing the
vehicles that spawn there. BF2 removed it. This documents what was removed, what was
kept, and what it would take to restore.

## Verdict

**Restoring this is an exe/DLL change only. No `.hud` data changes are needed.**

Implemented on modtools, Steam and GOG; see [Implementation](#implementation).

LucasArts cut exactly one function and left everything else standing:

- the `VehicleSpawn` -> `CommandPost` association is fully live (it drives normal vehicle spawning)
- the per-team vehicle class arrays are fully live
- the HUD event `player%d.spawnDisplay.vehicle` is still registered as a string event
- **the stock shipped `.hud` files still contain the text element bound to it**, positioned
  and wired to the spawn screen's show/hide events

The only missing piece is the code that produces the string and sends the event.

---

## The BF1 original

`SpawnDisplay::UpdateSpawnpointText(CommandPost*)`, BF1 `0x001D03C0`.

```c
// paraphrased
void SpawnDisplay::UpdateSpawnpointText(CommandPost* cp) {
    if (!cp) return;
    SpawnVehicleList list;                        // stack object
    int team = cp->entity->mTeam;                 // resolved through the CP's weak handle
    RedTextElement::SetText(mSpawnPointNameText, cp->mName);

    for (node = gVehicleSpawnList; node != end; node = node->next) {
        VehicleSpawn* vs = node->obj;
        if (NetGame::ConvertCP(vs->mCommandPost) != cp) continue;
        list.AddEntity(vs->mSpawnClass[team]);    // BF1 +0x88
        list.AddEntity(vs->mFlyerClass[team]);    // BF1 +0xA8
    }
    if (list.mShownCount == 0)
        ustrncat(list.mVehicleListStr, LocalizeDB::Find(0xF9028BE4));  // "none"
    RedTextElement::SetText(mVehicleListText, list.mVehicleListStr);
    if (mSpawnPointNameText->IsChanged())
        FlashyTextElement::StartFlashiness(mSpawnPointNameText);
}
```

`SpawnVehicleList` is a small stack helper that de-duplicates and joins names.
BF1 addresses: `~SpawnVehicleList` `0x003B7E40` / `0x003B7E70`, vtable `0x00449C60`,
`AddEntity` `0x001D0D20`.

---

## What BF2 kept

### `VehicleSpawn` (368 bytes, identical in Phantom / modtools / Steam)

| Offset | Field | Notes |
| --- | --- | --- |
| `+0x1C` | `m_ListNode` | `PblList` node |
| `+0x30` | `mMatrix` | |
| `+0x70` | `mClass` | `VehicleSpawnClass*` |
| `+0x74` | `mCommandPost` | `ConstCommandPostHandle`, **live** |
| `+0x7C` | `mSpawnCount` | |
| `+0x80` | `mSpawnTime` | |
| `+0x90` | `mSpawnClass[8]` | `EntityClass*` per team, **live** |
| `+0xB0` | `mFlyerClass[8]` | `EntityFlyerClass*` per team, **live** |
| `+0xD0` | `mUseCarrier[8]` | |
| `+0xF4` | `mVehicleTeam` | |
| `+0xF8` | `mSpawnTeam` | |

The BF1 -> BF2 offset shift is `mSpawnClass` `0x88` -> `0x90` and `mFlyerClass`
`0xA8` -> `0xB0`, caused by `mDecayTime` being inserted.

### `VehicleSpawn::SetProperty` (modtools `0x00664E60`)

Ghidra labels the `this` pointer as `VehicleSpawnClass*`; it is the world entity.
`VehicleSpawnClass` is `0x84` bytes, so decompiler expressions of the form
`this[1].field_0xNN` are `0x84 + NN` into `VehicleSpawn`.

Parses `CommandPost`, `SpawnTime`, `DecayTime`, `SpawnCount`, `Team`, plus the
per-team class properties (loop over the 8 team slots against the shared
per-team property-name table at `0x00AD5F28`, 3 name aliases per team).

The `CommandPost` branch resolves through `CommandPostManager::FindPost` and then
validates, with three distinct messages from
`C:\Battlefront2\main\Battlefront2\Source\VehicleSpawn.cpp`:

| Line | Severity | Message |
| --- | --- | --- |
| `0x87` | error | `Vehicle spawn missing command post "%s"` |
| `0x8B` | warning | `Vehicle spawn command post "%s" has no control region` |
| `0x8C` | warning | `Vehicle spawn outside command post "%s" control region` |

This path is load-bearing for ordinary vehicle spawning, so `mCommandPost` can be
relied on.

---

## What BF2 cut

### `SpawnDisplay::UpdateSpawnpointText`

Phantom `0x007D8AD0`, in full:

```
007d8ad0: C20400      RET 0x4
```

Zero callers. The thunk at `0x0040BA1E` is itself unreferenced. On modtools and Steam
the linker dropped the function entirely, along with all of `SpawnVehicleList`.

### `SpawnVehicleList` survives in Phantom only

Still complete in the dev build, which is useful as a reference implementation.
`SpawnVehicleList::AddEntity` (thunk `0x0041702B`) de-duplicates against
`mShownList`, appends `", "`, prefers `EntityClass::mLabel` (already localized) and
falls back to `mFilename`, caps at 31 entries and logs
`"Too many vehicles at a spawnpoint"` from `pcSpawnDisplay.cpp:0xFB`.

Struct (648 bytes):

| Offset | Field |
| --- | --- |
| `+0x00` | vtable |
| `+0x04` | `mShownCount` |
| `+0x08` | `mShownList[32]` (`EntityClass*`) |
| `+0x88` | `mVehicleListStr[256]` (`wchar_t`) |

---

## The HUD side is already complete

`HUD::GameEvents::Open` registers five spawn-display events. Types are from the PDB
enum `HUD/EventClass/Type` (see `HUDEventBindings.md`).

| Event | Type | Producer |
| --- | --- | --- |
| `player%d.spawnDisplay.enable` | `Bool(1)` | `SpawnDisplay::Show`, `EnableViewport` |
| `player%d.spawnDisplay.disable` | `Bool(1)` | `SpawnDisplay::Hide`, `EnableViewport` |
| `player%d.spawnDisplay.message` | `Uint(3)` | `HUD::GameEvents::DisplaySpawnMessage` |
| `player%d.spawnDisplay.vehicle` | `String(8)` | **nothing** |
| `player%d.spawnDisplay.spawninfo` | `String(8)` | **nothing** |

The two string events are written once in `Open` and read only by `Close`. Nothing
ever sends them a value, in any build.

### Event class globals

Derived by walking the `EventClass::Create` call chain in `Open`. The pattern is
`PUSH count; PUSH name; PUSH type; MOV [global], EAX; CALL`, so the `MOV` that follows
a given `CALL` stores that call's result.

| Build | `Open` | `.message` | `.vehicle` | `.spawninfo` |
| --- | --- | --- | --- | --- |
| Phantom | `0x00611F90` | `0x00B27128` | `0x00B2712C` | `0x00B27130` |
| modtools | `0x006AEF00` | `0x00BA3CF0` | `0x00BA3CF4` | `0x00BA3CF8` |
| Steam | `0x0055E3A0` | `0x01E56DC8` | `0x01E56DCC` | `0x01E56DD0` |
| GOG | `0x0055F120` | not derived | not derived | not derived |

modtools `.enable` is `0x00BA3CE8`, `.disable` is `0x00BA3CEC`.

Ghidra resolves the indexed reads on Phantom and modtools but not on the Steam LTCG
build, so the Steam "no producer" result is inference from the other two plus the
absent function, not a direct xref proof.

### Stock `.hud` data already has the element

`data/Common/hud/PC/1playerhud.hud` (and the non-PC `1playerhud.hud`, and the
console `2/3/4playerhud.hud` files) ship this verbatim:

```
Text("player1spawnvehicle")
{
    TextBox(0.384766, 0.500000)
    TextBreak("Word")
    TextFont("gamefont_tiny")
    TextStyle("Shadow")
    EventText("player1.spawnDisplay.vehicle")
    Position(0.497674, 0.338058, 0.000000, "Viewport")
    ZOrder(0)
    EventEnable("player1.spawnDisplay.enable")
    EventDisable("player1.spawnDisplay.disable")
}
```

There is a matching `player1spawninfo` at `Position(0.498295, 0.192691)`.

Both `1playerhud.hud` and `PC/1playerhud.hud` declare `Viewports(1)`, so both load on
PC and the later-parsed one wins. Either way the element exists, is positioned, and
already appears and disappears with the spawn screen. It renders blank because the
event never carries a value.

**This is why no data change is required.** The element is not something a modder has
to author; it is already shipped and already correct.

---

## Implementation

Implemented in `PatcherDLL/src/render/spawn_vehicle_list.cpp`, INI
`[Features] SpawnVehicleList` (default on). Entirely DLL-side; no data changes.
modtools, Steam and GOG.

Hook: `SpawnDisplay::Update`, `bool __thiscall(this, float dt)`, vtable slot 1.
After the original runs we read `mMode` and `mCommandPost` off `this`, and when
either changed since the last send we rebuild and send the string.

### Why polling Update rather than the post-change edge

The obvious hook is `SpawnDisplay::SetCommandPost`, the sole writer of
`mCommandPost`, reached from `SelectPost`, which `Update` calls on exactly the
"highlighted post changed" edge:

```c
cp = GetCommandPost(this);
if (cp != this->mCommandPost) SelectPost(this, cp);
```

That was the first implementation and it is the wrong edge on its own, in two
ways that cancel out into a visible bug:

- the post is already selected while the player is still on the **side-select**
  screen (`Show` picks a random spawn point if there is none), so the list
  appears there, which is not where BF1 put it;
- once the side is accepted the post usually has *not* changed, so no further
  `SetCommandPost` arrives and the list would never appear on the unit screen
  where it belongs.

Polling `Update` covers the post changing, the mode changing, and the post being
captured out from under the player, in one place.

### The side-select gate

`mMode == 0` is side select. The engine draws the same line itself: `SelectPost`
plays its spawn-point-change sound only `if (mMode != 0)`, so it already treats
"not side select" as the condition under which a post change is user-visible
feedback. We reuse that gate rather than inventing a mode whitelist.

The element is on-screen during side select (it is enabled by
`spawnDisplay.enable`, which `Show` fires before any of this), so it has to be
actively blanked there rather than merely left alone.

### What gets built

Take the post's owning team, walk `sVehicleSpawnList` keeping the spawns whose
`mCommandPost` matches, collect `mSpawnClass[team]` / `mFlyerClass[team]`,
de-duplicate, join the localized labels with `", "`, send through the existing
HUD event.

BF1 indexed by the *post's* owning team rather than the viewer's. Those agree for
any post you can spawn at, and it keeps the hook self-contained:
`SpawnDisplay::mTeamNumber` is only refreshed later in `Update`, so it is stale on
the first selection of a life. Tracking the team also means a post captured while
the player is looking at it swaps the list, for free.

### Addresses

| | modtools | Steam | GOG |
| --- | --- | --- | --- |
| `SpawnDisplay::Update` | `0x0068CCD0` | `0x0042A6B0` | `0x0042A670` |
| `HUD::Event::Send` | `0x006ADA90` | `0x0055E100` | `0x0055EE80` |
| `.vehicle` EventClass* array | `0x00BA3CF4` | `0x01E56DCC` | `0x01E5827C` |
| `sVehicleSpawnList` | `0x00AD6004` | `0x007EBEBC` | `0x007ECE8C` |
| `SpawnDisplay` vtable | `0x00A5BB7C` | `0x0079663C` | `0x007975DC` |
| `SpawnDisplay::SetCommandPost` | `0x0068A8C0` | `0x0042B9D0` | `0x0042B990` |
| `SpawnDisplay::SelectPost` | `0x0068A950` | `0x0042B960` | `0x0042B920` |
| `SpawnDisplay::Show` | `0x0068A9C0` | `0x00429DD0` | `0x00429D90` |
| `SpawnDisplay::mMode` | `+0x18` | `+0x18` | `+0x18` |
| `SpawnDisplay::mTeamNumber` | `+0x2090` | `+0x2054` | `+0x2054` |
| `SpawnDisplay::mCommandPost` | `+0x2094` | `+0x2058` | `+0x2058` |
| event array index field | `+0x2000` | `+0x2000` | `+0x2000` |
| `EntityClass::mLabel` | `+0x40` | `+0x20` | `+0x20` |
| `EntityClass::mFilename` | `+0x20` | stripped | stripped |
| `EntityClass` name hash | n/a | `+0x18` | `+0x18` |

GOG was derived in its own program (`BattlefrontII_MemExt.exe`), not shifted from
Steam: three of the four addresses are globals, which the piecewise `.text` shift
does not cover. Its `.vehicle` array was confirmed twice over, from `Show`
(`.enable` at `0x01E58270`, stride `0xCA`) and from `Open`'s own store at
`0x0055F94E`.

Build-invariant, verified on all three: `VehicleSpawn` `m_ListNode +0x1C`,
`mCommandPost +0x74`, `mSpawnClass[8] +0x90`, `mFlyerClass[8] +0xB0`;
`CommandPost` `mObject +0x2C`, `mSavedHandleId +0x30`; `GameObject`
handle id `+0x204`, `mTeam` 4-bit signed bitfield `+0x234`; HUD event array
stride `0xCA` dwords per local player.

### Calling conventions

`SpawnDisplay::Update` ends in `RET 4` (Steam epilogue at `0x0042AC89`:
`pop edi / mov al,1 / pop esi / mov esp,ebp / pop ebp / ret 4`), read off the
disassembly rather than trusted from the decompiler's signature. `dt` is a float,
so it goes on the stack, and `__fastcall(ecx, edx, float)` matches
`__thiscall(this, float)` exactly.

`HUD::Event::Send` is byte-identical on all three builds and ends in `RET`, not
`RET 4`, so it takes `this` in ECX with no stack argument:

```
51        PUSH ECX          ; the Event* becomes EventClass::Send's stack arg
8B 09     MOV  ECX,[ECX]    ; ECX = this->mClass
E8 ..     CALL EventClass::Send
C3        RET
```

`HUD::Event` is an 8-byte POD and its constructor only stores the two words
(modtools `0x006AD4A0` is literally two `mov`s), so the DLL builds one inline
rather than calling the engine.

The install byte-guards `Update`'s prologue and declines with the bytes it found
on a mismatch. modtools is `A1 80 02 B3 00 83 EC 2C 85 C0`; Steam and GOG share
`55 8B EC 83 E4 F0 83 EC 28 83` and diverge only at the 11th byte.

### Why the string reaches the screen

`HUD::ElementText::EventText` (Phantom `0x0060C360`) handles `type_String` by
casting the event data straight to `wchar_t*` and calling
`RedTextElement::SetText`, which copies. A null pointer is handled too and
simply clears the element, so an empty list is safe.

### Resolved since the original survey

- The spawn-point-change site is `SpawnDisplay::SetCommandPost`, reached from
  `SelectPost`. Superseded as the hook by the `Update` poll, above, but it is
  still what pins `mCommandPost`'s offset on each build.
- `sVehicleSpawnList` exists and is the engine's own list, so no shadow list off
  the constructor is needed. Its address falls out of the destructor's `_iCount`
  decrement minus `0x10`.
- GOG derived.

### Still open

- `.spawninfo` is dead in exactly the same way, its element is likewise already
  in the stock `.hud`, and it would come back through the same hook.
- The BF1 "none" string `0xF9028BE4` has no confirmed BF2 counterpart. An empty
  command post leaves the line blank rather than inventing one.
- Play-tested on modtools only. Steam and GOG are built but unverified in-game.
- Splitscreen is coded for (per-player state, `[playerIdx * 0xCA]`) but PC only
  ever runs one viewport, so index 0 is the only path actually exercised.

## Cross-references

- `HUDEventBindings.md` - event type enum, binding rules, why misspelled bindings are
  silent on retail
- `HUDSystem.md` - `.hud` load gating, `FileInfo` viewport rules, parse order
- `EntityCarrierSystem.md` - `VehicleSpawn` / `vehiclepad` integration, the per-team
  class table, carrier spawn sequence
