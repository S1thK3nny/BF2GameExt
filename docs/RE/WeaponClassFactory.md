# Weapon Class Factory and the `dualcannon` ClassLabel

How BF2 turns `ClassLabel = "cannon"` in a weapon ODF into a live C++ class, how
GameExt registers a new label from the DLL, and how the first one, `dualcannon`, is
built on top of that. User documentation: [docs/user/classlabels/](../user/classlabels/README.md).

Addresses are unrelocated (imagebase `0x400000`) and modtools unless a column says
otherwise. The full per-build list is in [Address reference](#address-reference).
Code: `PatcherDLL/src/weapon/dual_cannon.cpp`.

---

## How a ClassLabel becomes a class

### Registration

`GameState::CreateBaseWeaponClasses` (`0x0044C960`, `__cdecl`, plain `RET`) is a flat
list of twenty allocations, one per stock label:

```cpp
new WeaponCannonClass(PblHash("cannon"));
new WeaponLauncherClass(PblHash("launcher"));
new WeaponMeleeClass(PblHash("melee"));
...
```

The base `WeaponClass` constructor registers the object itself. `WeaponClass` derives
from `Factory<Weapon,WeaponClass,WeaponDesc>`, and that sub-object carries the identity:

| Offset | Field | Notes |
|--------|-------|-------|
| +0x00 | vptr | |
| +0x04 | `Node mNode` | intrusive list node, linked into `sList` (`0x00AD43BC`) |
| +0x18 | `uint mId` | **the ClassLabel hash** |
| +0x1C | `uint mNetIndex` | post-incremented from `sCounter` (`0x00B91BE8`) |

Both the hash constructor and the copy constructor register and take an index, so
**every ODF-derived weapon class consumes an index too**, not just the twenty base
classes. `sCounter` warns above 254.

### Lifetime: per game state

`CreateBaseWeaponClasses` has one caller, `GameState::PreStateInit` (call site
`0x0044F1B7`), so the base classes are rebuilt on every mission load.
`GameState::PostStateCleanup` unlinks `sList` and resets `sCounter`; on modtools it
does not free the class objects. **Registration has to happen every time**, not once
at DLL load. The hook point is `CreateBaseWeaponClasses` itself: call the original,
then register. That re-runs per state and puts the new class at a fixed position.

Class objects must come from the engine's `operator new` (`0x007E34A0`), never the
DLL's CRT allocator.

### The ODF load path

`WeaponClass::Read(PblFileChunk*)` is the munged-ODF reader and the only consumer of
the registry:

| Chunk | ASCII | What it does |
|-------|-------|--------------|
| `0x45534142` | `BASE` | reads the ClassLabel string, hashes it, **linear scan of `sList` for a matching `mId`**. Miss: `RedWarning "Weapon base class \"%s\" not found"` |
| `0x45505954` | `TYPE` | reads the weapon's own name, scans `sList` to reject a duplicate (two ODFs with one name keep the first), then calls **`base->Derive(nameHash)`** (vtable `+0x04`) to mint the per-ODF class |
| `0x504f5250` | `PROP` | reads a **pre-hashed** property id plus its value string and calls **`derived->SetProperty(id, value)`** (vtable `+0x18`) |

Consequences:

- **Registering one base object with the right `PblHash` is all a new label needs**
  to resolve. Nothing is baked into a static table.
- **Every per-ODF class comes from the base's `Derive`**, so it inherits whatever
  vtable the base carries. There is no second registration step.
- OdfMunge passes unknown ClassLabels and unknown property names straight through,
  hashed. PblHash is FNV-1a over `(c | 0x20)`; `ToolsFL\bin\Hash.exe` is ground truth.
- OdfMunge adds any property ending in `GeometryName` to the `.req`, so a new
  `*GeometryName` property gets its model packed for free.

### Multiplayer: `mNetIndex` is on the wire

`mNetIndex` is sent in 8 bits in `NET_EVENT_CREATE_ORDNANCE` and resolved back by
`ReadWeaponClass` (damage owner weapon, tow cable and melee throw type checks). One
extra registration shifts the index of every class after it, so a peer without it
would resolve the wrong class.

The fix: a base class object is never itself a fire weapon, so after registering it
takes `mNetIndex = 0xFF` (the "no class" value, which lookups reject before scanning)
and gives its counter slot back (`sCounter--`). Maps that do not use the label then
index exactly as stock. Classes derived from it take normal indices.

**A stock game crashes** loading a level that contains an ODF with a GameExt label:
without our registration the `BASE` lookup finds no class for it. Tested with
`dualcannon`, also when only a `WeaponName@GameExt` line references it, since the
munge packs the weapon either way. Nothing on the DLL side can help, because stock
never runs our code, so a mod that uses a GameExt label requires the extension.

### Class objects and instances

| Class | modtools | Steam / GOG |
|-------|----------|-------------|
| `WeaponCannonClass` object | `0x3DC` | `0x2E0` |

The size differs because the debug build carries extra `WeaponClass` members, so every
**class-side** field offset differs between modtools and retail. `Weapon` and `Aimer`
offsets match on all three builds, except `WeaponCannon::mSalvoCount`.

Instances come from `Weapon::sMemoryPool` (`0x00B91C10`), a fixed-stride pool that only
**warns** when a request exceeds its stride. A larger weapon object would mean raising
the pool size before it is first used. GameExt avoids that entirely: extra state lives
in side tables (below), so objects keep their stock size.

---

## Registering a new label from the DLL

What `dual_cannon.cpp` does, and what the next label should copy.

1. **Hook `CreateBaseWeaponClasses`.** After the original returns, engine-`new` a
   `WeaponCannonClass`-sized object and run the engine's own hash constructor
   (`0x00625A10`, `__thiscall`, `RET 4`) with the new label's PblHash. Apply the
   `mNetIndex = 0xFF` / `sCounter--` fix.
2. **Re-point it at a DLL-owned copy of the class vtable** (13 slots, `0x00A525F4`).
   Copy it lazily at the first registration, which is long after every install-time
   vtable patch has landed (`barrel_fire_origin`, `held_ordnance_effect`), so the copy
   inherits those hooks.
3. **Override `Derive` and `Build` by chaining**, not allocating. Each calls the slot
   it replaced, then re-points the new object's vptr at our class or instance vtable
   (a DLL copy of the 61-slot `WeaponCannon` table, `0x00A52468`).
   `held_ordnance_effect` hooks `Derive`, `SetProperty` and the destructor to keep its
   own side tables in step with class inheritance; allocating in our `Derive` would
   silently skip that.
4. **Extra state goes in side tables**, keyed by `WeaponClass*` and `Weapon*`, never
   in a larger object. `Derive` copies the parent's entry so properties inherit through
   `ClassParent`. Both destructors erase their entry, and both tables are cleared on
   every mission load, because the next state reuses the addresses.
5. **`SetProperty` handles the new hashes** and forwards everything else to the slot
   it replaced.
6. **Instance behaviour** through instance vtable slots where the engine calls them
   virtually, and detours (gated on our vptr, so every other weapon passes straight
   through) where it does not.

Class vtable slots used: `+0x00` destructor, `+0x04` `Derive`, `+0x08` `Build`,
`+0x18` `SetProperty`. Instance slots used: `+0x00` destructor, `+0x8C` `Render`.

---

## `dualcannon`

One weapon instance (one ammo pool) that draws a second model and alternates fire
origin, muzzle flash and shoot animation between the two. A design with two real child
weapons was rejected: a weapon outside the soldier's `Weapon*[8]` array gets no update,
HUD, net or animation handling.

### Properties

| Property | Hash | Stored as |
|----------|------|-----------|
| `OffhandGeometryName` | `pbl_hash` | `RedModel*` from `FindModel` (`0x00448670`, `__cdecl(PblHash)`) |
| `OffhandHardPoint` | `pbl_hash` | `PblTEMPHash` of the name: a key into the soldier's `RedPose` |
| `OffhandFirePointName` | `pbl_hash` | `PblTEMPHash`, default `hp_fire`, resolved to an offset with `RedModel::GetParentBoneAndOffset` (`0x007F9E50`, `__thiscall(crc, out)`, `RET 8`) |
| `AlternateMode` | `pbl_hash` | `shot` (default) or `salvo` |
| `FireAnim` | `pbl_hash` | swallowed with a warning: the gun that fires picks the state |

The fire point is re-resolved whenever the model or the name changes, so the property
order in the ODF does not matter. `Offhand*` names are safe on other classes: only
`WeaponMelee::SetProperty` parses names like these, and a cannon falls through.

### Drawing the second gun

`Weapon::Render` (instance `+0x8C`) draws the main gun at `hp_weapons`, the muzzle
flash, and bakes `mFirePointMatrix` (`+0x20`, translation at `+0x50`). After it, our
override looks `OffhandHardPoint` up in the pose with
`pbl_hash_table_find(pose + 4, 0x100, crc)`, multiplies that matrix by the world
matrix and calls the offhand model's own `Render` (model vtable `+0x04`). It then bakes
the offhand fire position into the weapon's side table under the same conditions
`Weapon::Render` uses for the main one: not for an invisible draw (`color.a == 0`), and
not for a mirrored matrix (negative determinant), so a reflection region's duplicate
cannot overwrite the real position. A hidden weapon (`+0xAC` bit 0) skips the offhand.

The engine's own dual wield (`OffhandWeapon` prop, `WeaponClass` flag, draws at
`bone_l_hand`) is a two-channel system and is not used.

**Spawn screen.** The preview soldier has no `Weapon` instance:
`SoldierElement::RenderUsingContext` draws the selected weapon with the non-virtual
`WeaponClass::Render` (`0x0061D170`, `__thiscall`, `RET 0x14`, sole caller
`0x00674F0E`). A detour on it draws the offhand model with the same helper. Stock
classes are not in the side table and pass straight through.

### Alternating fire

`WeaponCannon::Fire` (`0x00626490`) is non-virtual, has one caller
(`WeaponCannon::UpdateFire`, `0x006276F5`), and builds the `OrdnanceDesc` from
`Aimer::mFirePos` (`+0x88`) and `mDirection` (`+0x48`) on every call. `UpdateFire`
calls it once per `ShotsPerShot` pellet and can call it several times a frame, so a
per-shot origin needs a detour on `Fire`. (`UpdateFire` also calls `Weapon::EnterFire`
directly between continuous salvos, so the `EnterFire` slot would miss salvos.)

The detour decides the gun per call:

- A **new shot** differs from the last call in mission time, in `mSalvoCount`, or has
  already had all its `ShotsPerShot` pellets. Pellets of one shot keep the gun.
- A **new salvo** is `mSalvoCount == ShotsPerSalvo`: the engine resets the count when
  a salvo starts and counts it down.
- `shot` mode switches gun on a new salvo; `salvo` mode on every new shot.

For gun 2 it swaps `mFirePos` and `mDirection` for the offhand fire position and a
direction from `barrel_fire_origin_aim_from` (the barrel fix's per-aimer impact point,
so both guns converge on the crosshair), calls the original, and restores them,
because the next call in the same frame may be gun 1.

### Shoot animations

Around the call the detour sets `Weapon::mState` (`+0xB0`) to `FIRE` (1) for gun 1
and `FIRE2` (2) for gun 2, only ever swapping between those two, which every consumer
treats alike. `SoldierAnimatorClass::WeaponStateToAnimation` maps them to `SHOOT` /
`SHOOT2`, or `SHOOT_SECONDARY` / `SHOOT_SECONDARY2` in the secondary slot. The engine
normally enters fire as `(FireAnim & 3) + FIRE`, which is why `FireAnim` is ignored.

A bank with no `shoot2` clip falls back up the bank and weapon chain like any missing
animation. An animation `.msh` exported without its animation chunks is dropped from
the `.anims` list by the munge with no message.

### Muzzle flash

The flash belongs to the gun that fired last. `Weapon::Render` draws it at the main
fire point via `WeaponClass::RenderFlash` (`0x0061CA80`), timed by
`mMuzzleFlashStartTime` (`+0xC4`). For gun 2 the override zeroes that timer around the
base `Render`, then calls `RenderFlash` itself at the offhand fire position with
`t = (start - now) / mFlashLength`. `FlashLightColor` / `FlashLightRadius` /
`FlashLightDuration` have no consumer in any build, so there is no flash light to move.

### First person: stale `shoot2` slots

`FIRE2` makes a latent engine bug reachable. First person picks its animation from
`FirstPerson::mAnim[weaponClass * 11 + state]` (48 slots), filled by
`FirstPerson::Init` (`0x004AB590`) on every `ingame.lvl` load: named slots are looked
up, and slots still null afterwards get `humanfp_tool_idle`. The `shoot2` slot (state
3) has no name for the rifle, bazooka and tool classes, so `Init` never touches it and
it keeps the previous level's pointer, freed by then. Stock never reaches those slots
because only a grenade enters `FIRE2`. The crash was an access violation in
`ZephyrAnimInst<32>::SetAnim` from `FirstPersonRenderable::SetAnimation`. A detour on
`Init` clears all 48 slots first, so every unnamed slot is refilled from the level
being loaded.

First person draws only `HighResGeometry`; the offhand model is third person only.

### Multiplayer

The host creates every bolt and sends its **position** in the create ordnance event,
so the host's choice of gun replicates for free. Three client-side problems needed
work, all without new data on the wire.

**1. A client fires its own weapon in several net turns.** `WeaponCannon::Fire` only
creates ordnance in the local turn: with `netEnabled && netOnClient && !netIsLocalTurn`
(`0x00BDA95D`, test at `0x006269CD`) it returns "fired" and creates nothing. Those calls
must not advance the gun.

**2. Client prediction replays turns.** `NetGame::Predict` (`0x006E8970`) rewinds to
the host snapshot every frame and re-simulates every unacknowledged turn.
`WeaponCannon::Read` restores `mSalvoCount`, so the salvo-start turn replays with
`mSalvoCount == ShotsPerSalvo` and a gun switch would run once per replay. With a
steady ping that is the same number of flips per shot, so the same gun fires every
time. Gate 1 covers this too, because only the newest turn is the local turn.

**3. Other players' guns on a client.** A weapon is remote when
`NetGame::GetJoystickIndex(owner->mPlayerId)` (`0x006E3C80`, owner `+0xD4`) is
negative, the same test `Fire` uses. The detour leaves remote weapons alone. The
client's replay re-enters plain `FIRE`, so the replicated `mState` never survives to
render and cannot pick the gun. Instead:

- **Gun and flash** come from the event. The ordnance factory `Build` call in
  `ReadNetEvent`'s create ordnance case (`0x006EC534`, 15 bytes, byte-guarded) is
  rewritten to `net_build_ordnance`, which calls the factory, then picks the gun whose
  fire point is nearer the event's position (`OrdnanceDesc +0x00`, firing weapon at
  `+0x44`) and starts the remote flash.
- **Shoot animation**: `EntitySoldier::Render` plays shoot / shoot2 from `mState` when
  the weapon's fired flag (`+0xAC` bit 1) is set, and with networking on it substitutes
  `FIRE` / `FIRE2` from the weapon slot when `mState` is neither. That choice comes
  before the weapon renders, so our `Render` is too late. Frame order is events, then
  `Predict`, then render, so a detour on `Predict` runs after the replay: for remote
  dual weapons it clears the fired flag unless a host event is pending, and otherwise
  sets the flag and `mState = FIRE / FIRE2`. Our `Render` restores `mState` afterwards.

Tested with a modded host and client: both guns, flash and shoot animations match the
host on every machine (the remote animation can be off for the first few seconds).

**Open:** other players' bolts are white on a client. Laser colour is the ordnance
class `mGlowColor` times `Ordnance::mRenderColor` (white unless a team bonus applies),
and `CreateOrdnance` is the same `mOrdnanceClass->Build` call on both sides. Suspect:
the event's 8-bit ordnance index resolving to another class on the client. Not yet
known whether stock weapons do it. Tracked in `ROADMAP.md`.

### Retail differences

Steam and GOG are not a table fill. On top of the per-build addresses:

- **`WeaponClass::RenderFlash` takes `t` in XMM3** (call site Steam `0x006794D4`),
  `RET 8` with only pos and dir on the stack. Modtools is `RET 0xC` with three stack
  args. Bridged by a naked thunk (`render_flash_retail`).
- **`GameLoop::GetMissionTime` returns in XMM0** on retail (`MOVSS XMM0,[EBP-4]` at
  Steam `0x00530EBC`), ST(0) on modtools. A `float(__cdecl*)()` typedef reads ST(0), so
  a second thunk (`mission_time_xmm`) moves the value. Any float-returning engine
  function needs this check on retail.
- **`WeaponClass::Render` substitutes a constant `0x04000000`** for the model's render
  flags on retail (Steam `0x0067BFA6`, GOG `0x0067D046`) instead of forwarding its own.
  The spawn screen offhand draw must match it. `Weapon::Render` forwards on every build.
- **Class-side offsets differ** (table below). `ShotsPerShot` has no key of its own
  (`PblHash("ShotsPerShot")` has zero sites, the field has two alias keys) and was read
  from `UpdateFire`'s `CMP EAX,[ECX+0x27C]` (Steam `0x0067ED96`).

Each pair of raw function pointers (`*St0` / `*Xmm`, `*St` / `*Retail`) is reachable
only through its `call_*` wrapper, so a missed call site cannot jump through a null.

---

## Address reference

Unrelocated. GOG came from `tools/port_gog.py` off Steam (every entry score 1.00) and
the offsets were re-read out of the GOG image.

| Item | Modtools | Steam | GOG |
|------|----------|-------|-----|
| `GameState::CreateBaseWeaponClasses` | `0x0044C960` | `0x00539AA0` | `0x0053A810` |
| engine `operator new` | `0x007E34A0` | `0x006C3540` | `0x006C45D0` |
| `WeaponCannonClass(uint hash)` | `0x00625A10` | `0x00680050` | `0x006810D0` |
| `WeaponCannonClass` vtable (13 slots) | `0x00A525F4` | `0x007B0674` | `0x007B15EC` |
| `WeaponCannon` vtable (61 slots) | `0x00A52468` | `0x007B057C` | `0x007B14F4` |
| `Factory<Weapon,...>::sCounter` | `0x00B91BE8` | `0x01FAA758` | `0x01FABC08` |
| `FindModel(PblHash)` | `0x00448670` | `0x00411C50` | `0x00411C50` |
| `RedModel::GetParentBoneAndOffset` | `0x007F9E50` | `0x006C41E0` | `0x006C5270` |
| `WeaponCannon::Fire` | `0x00626490` | `0x0067F320` | `0x006803A0` |
| `WeaponClass::RenderFlash` | `0x0061CA80` | `0x0067BD30` | `0x0067CDD0` |
| `GameLoop::GetMissionTime` | `0x00732E60` | `0x00530EA0` | `0x00531BF0` |
| `FirstPerson::Init` | `0x004AB590` | `0x00521000` | `0x00521000` |
| `WeaponClass::Render` | `0x0061D170` | `0x0067BF00` | `0x0067CFA0` |
| `netIsLocalTurn` | `0x00BDA95D` | `0x01E62F10` | `0x01E643C0` |
| `NetGame::GetJoystickIndex` | `0x006E3C80` | `0x005B73C0` | `0x005B8370` |
| `ReadNetEvent` ordnance `Build` site | `0x006EC534` (15 B) | `0x005BF327` (20 B) | `0x005C02B7` (20 B) |
| `NetGame::Predict` | `0x006E8970` | `0x005BA810` | `0x005BB7C0` |

| Offset | Modtools | Steam / GOG |
|--------|----------|-------------|
| `WeaponClass` `mFlashLength` | `0x290` | `0x1B8` |
| `WeaponClass` `ShotsPerSalvo` | `0x354` | `0x280` |
| `WeaponClass` `ShotsPerShot` | `0x358` | `0x27C` |
| `WeaponCannon` `mSalvoCount` | `0x144` | `0x114` |
| `WeaponCannonClass` size | `0x3DC` | `0x2E0` |

Shared `Weapon` fields (all builds): `mOwner +0x6C`, `mClass +0x64`,
`mRenderClass +0x68`, `mAimer +0x70`, flags `+0xAC` (bit 0 hide, bit 1 fired),
`mState +0xB0`, `mMuzzleFlashStartTime +0xC4`, `mFirePointMatrix +0x20`.
`Aimer`: `mDirection +0x48`, `mFirePos +0x88`.

---

## Related

- [barrel-fire-origin.md](barrel-fire-origin.md): vtable patching of `WeaponCannon` /
  `WeaponLauncher`, and the per-aimer impact point the second gun aims with.
- [FirstPersonAnimationSystem.md](FirstPersonAnimationSystem.md): the first person
  animation table.
