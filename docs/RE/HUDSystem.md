# HUD System

The in-game HUD is a fully data-driven retained-mode widget tree with its own
named pub/sub event bus. Nothing about the layout is hardcoded: the engine
publishes ~173 named events, the `.hud` config file builds elements and
subscribes them to those events by name, and the elements re-render themselves.

Investigated on the Phantom build, then
re-derived by hand on modtools. Steam and GOG addresses are **not** derived here.

| Build | Status |
|---|---|
| Phantom | Fully symbolized, all decompiles below |
| Modtools (`BF2_modtools.exe`) | Core addresses re-derived by disassembly, all strings present |
| Steam / GOG | Not derived. The system is core to the shipped game and the `.hud` data ships on retail, so it is present, but every address below needs porting |

---

## Where the data lives

| Layer | Detail |
|---|---|
| Source | `data/Common/hud/*.hud` (plain text, PblConfig syntax) |
| Munger | `ToolsFL/bin/ConfigMunge.exe`, declared in `ingame.req` under `"config"` |
| Container | chunk FourCC `hud_` (`0x5F647568`) inside `ingame.lvl` |
| Loader | `LoadUtil::ReadDataFileChunk` dispatches `hud_` to `HUD::Manager::Load` |

Stock files: `1playerhud.hud`, `2playerhud.hud`, `3playerhud.hud`, `4playerhud.hud`,
`hudtransforms.hud`, plus a `PC/` override directory.

### FileInfo is a load gate

Every `.hud` file opens with a `FileInfo` block, and `HUD::Manager::Load` uses it to
decide whether the rest of the file applies at all:

```
FileInfo("1playerhud")
{
    Viewports(1)
}
```

`Manager::Load` reads the `FileInfo` item first, then checks three things and
**aborts the entire file** if any fails:

1. `SplitModes` list must contain the current split mode (skipped if empty)
2. `Viewports` list must contain `CameraManager::sInstance->m_iNumCam` (skipped if empty)
3. if `Widescreen` was set, it must equal `gWidescreenEnabled`

Surviving files are appended to `gConfigFiles[gNumConfigFiles++]`.

**Consequence:** on PC `m_iNumCam` is 1, so only `1playerhud` and `hudtransforms`
(`Viewports(1, 2, 3, 4)`) ever load. `2/3/4playerhud.hud` are console-era assets that
are munged but never applied, which is why their `player2.*` / `player3.*` event names
have no counterpart in the engine's event registry (see [Built-in event
catalogue](#built-in-event-catalogue)).

---

## Object model

Everything in a `.hud` file is an `HUD::Item`, created by an `HUD::Item::Factory`
that is looked up by the **PblHash of the type name**.

`HUD::Item` (28 bytes):

| Offset | Field |
|---|---|
| `+0x00` | vptr |
| `+0x04` | `mNameHash` |
| `+0x08` | `mName` |
| `+0x0C` | `mNameDisplay` |
| `+0x10` | `mFactory` |
| `+0x14` | `mItemNode` (link into `Item::sList`) |
| `+0x18` | `mWriteEnabled` bitfield |

Item vtable slots used by the reader: `+0x04` `CreateItem`, `+0x08` `Read(scope)`,
`+0x20` `ReadData(config, data)`, `+0x24` `PostReadSetup()`.

`HUD::Item::Read` walks the config chunk: a `DATA` child (`0x41544144`) goes to
`ReadData`, a `SCOP` child (`0x504F4353`) recurses into `Read`. `PostReadSetup` runs last.

### Registered factories

Registered in `HUD::Manager::Open`. Type-name hashes are plain PblHash, so any
missing one can be computed (formula below).

| `.hud` keyword | Class | Type hash |
|---|---|---|
| `Group` | `ElementGroup` | `0x5FB91E8C` |
| `Text` | `ElementText` | |
| `Bitmap` | `ElementBitmap` | `0x46544626` |
| `BitmapMasked` | `ElementBitmapMasked` | |
| `Model3D` | `ElementModel3D` | |
| `Map` | `ElementMap` | |
| `Target` | `ElementTarget` | |
| (Target sub-items) | `ElementTarget::Target` | |
| `BarBitmap` | `ElementBarBitmap` | |
| `ProceduralBarBitmap` | `ProceduralBarBitmap` | |
| `BarSegmented` | `ElementBarSegmented` | |
| `MultilineText` | `ElementMultilineText` | |
| `VehicleSeating` | `ElementVehicleSeating` | |
| `BorderedBox` | `BorderedBox` | |
| `ObjectiveList` | `ObjectiveList` | |
| `ViewPort` | `ViewPort` | |
| `Sound` | `Sound` | `0x0E0D9594` |
| `TransformNumberColor` | `TransformNumberColor` | `0x0D34920D` |
| `TransformNumberColorBlend` | `TransformNumberColorBlend` | `0xF3C47270` |
| `TransformNameMesh` | `TransformNameMesh` | `0x67C3C715` |
| `TransformNumberVector3` | `TransformNumberVector3` | `0x2EF8F006` |
| `FileInfo` | `Manager::ConfigFile` | `0xD4E0C797` |

Class hierarchy (from the PDB struct set): `Item` is the root. `Element` (256 bytes)
derives from it and is the base of every visible widget; `ElementGroupBase` /
`ElementBitmapBase` / `ElementBar` are intermediate bases. `Transform` (56 bytes)
also derives from `Item` but is invisible: it is a pure event-to-event mapper.

### PblHash

`PblHash` is FNV-1a over `(c | 0x20)`, not over raw bytes:

```python
def pbl(s):
    h = 2166136261
    for c in s.encode():
        h = ((h ^ (c | 0x20)) * 16777619) & 0xFFFFFFFF
    return h
```

Verified against 31 reader hashes lifted out of `Element::ReadData`,
`Transform::ReadData`, `ViewPort::ReadData` and `Manager::Open`. Ground truth is
`ToolsFL/bin/Hash.exe <str>`.

Sample of resolved reader keys:

| Key | Hash | | Key | Hash |
|---|---|---|---|---|
| `EventEnable` | `0xD23096A8` | | `Color` | `0x3D7E6258` |
| `EventDisable` | `0x7B5F76B1` | | `ColorChange` | `0x3C656D30` |
| `EventChanged` | `0x0D1D42CD` | | `ColorChangeRate` | `0xB4660872` |
| `EventColor` | `0x49F429AE` | | `ColorPulseRate` | `0xE459101B` |
| `EventPulseRate` | `0xCCD945CA` | | `Alpha` | `0x5D8B6DAB` |
| `EventFadeOut` | `0xB8E23CBB` | | `ZOrder` | `0xEC9F263F` |
| `EventInput` | `0xCCE718ED` | | `Viewport` | `0xE4ABBAC3` |
| `EventOutput` | `0x6903ED32` | | `BlendMode` | `0xFA784EAB` |
| `EventNameFilter` | `0x4EE68DBE` | | `UseChangeColor` | `0x849604EB` |
| `FadeInTime` | `0x5083ECD9` | | `FadeOutTime` | `0x798DC484` |
| `FadeHoldTime` | `0x8E3E2923` | | `FadeSustainTime` | `0xFF48A8F9` |
| `Viewport0Position` | `0x0FBFC95E` | | `Viewport3Position` | `0x2E5F5873` |

---

## Lifecycle

| Phase | Function | What it does |
|---|---|---|
| Open | `HUD::Manager::Open` | switches to `GameMemory::RunTimeHeap`, allocates the 1 MB `HUDEditHeap` (only if `__RedDebugHeap != -1`), constructs every `Item::Factory`, allocates `gScreenGroup[5]` / `gScreenGroupEdit[5]`, constructs `gEditor`, then calls `GameEvents::Open()` |
| Load | `HUD::Manager::Load(chunk)` | per `hud_` chunk: sets up viewport dimensions, then for each top-level `DATA` looks up the factory by hash, creates the item, and `Read`s its scope |
| Update | `HUD::Manager::Update(dt)` | `Editor::Update` then `Element::UpdateAll(dt)`. While the editor is in edit mode the delta is scaled to `0.0001` so the world effectively freezes |
| Close | `HUD::Manager::Close` | `GameEvents::Close`, then `Factory::DestroyAll`, `Element::DestroyAll`, `Transform::DestroyAll`, `Sound::DestroyAll`, **`EventClass::DestroyAll`**, tears down the screen groups, editor and edit heap |

The game-state pump is `GameLoop::Update`, which calls `HUD::GameEvents::Update(dt)`
(two call sites). `HUD::EventQueue::Update` is registered as a virtual update in a
callback table, not called directly.

`HUD::Alloc(size, bool* useEditHeap)` allocates from `gEditHeap` when the flag is set
and the edit heap exists, otherwise from **`__RedCurrHeap`**. This matters for
anything calling into the HUD from outside the engine's own call sites.

---

## The event system

Four types, all tiny, all in one translation unit (`HUDEvent.cpp` / `HUDEventQueue.cpp`).

### `HUD::EventClass` (32 bytes)

One per named event. Lives in a global singly-linked list `EventClass::sList`.

| Offset | Field |
|---|---|
| `+0x00` | `mHashID` (PblHash of the resolved name) |
| `+0x04` | `mType` (`Type` enum) |
| `+0x08` | `mHandlerList` (`PblListDouble`, terminator `{next, prev}`) |
| `+0x10` | `mNode` (link into `sList`) |
| `+0x14` | `mRefCount` |
| `+0x18` | `mName` |
| `+0x1C` | `mFreeName` |

### `HUD::Event` (8 bytes, POD, empty destructor)

| Offset | Field |
|---|---|
| `+0x00` | `mClass` (`EventClass*`) |
| `+0x04` | `mData` (union: `intValue` / `uintValue` / `floatValue` / `string` (`wchar_t*`) / `color` (`RedColor*`) / `model` / pointer) |

### `HUD::EventHandler` (20 bytes)

| Offset | Field |
|---|---|
| `+0x00` | `mFunc` (`void __cdecl (Event*, void*)`) |
| `+0x04` | `mData` (user pointer, always the owning item) |
| `+0x08` | `mNode` (`{next, prev}` into the class's handler list) |
| `+0x10` | `mClass` (back pointer) |

### `HUD::DelayedEvent` (12 bytes) and the queue

`gEventList` is a fixed array of **32** `DelayedEvent { Event e; float activationTime; }`.
A free slot is marked by `activationTime < 0` (`0xBF800000`, that is `-1.0f`).
`EventQueue::AddEvent` linearly scans for a free slot and warns
`"Too many events added to the HUD::EventQueue! Max is %d"` when full. `EventQueue::Update`
sweeps all 32 slots each frame and sends any whose `activationTime` has passed
`GameLoop::GetMissionTime()`. `HUD::RemoveAllEvents` resets all 32 slots.

### The `Type` enum

Read off the `PUSH imm8` at the `EventClass::Create` call sites.

| Value | Name | Payload |
|---|---|---|
| 1 | `type_Bool` | int (0/1) |
| 2 | `type_Int` | int |
| 3 | `type_Uint` | uint |
| 4 | `type_Float` | float |
| 5 | `type_Model` | model handle |
| 6 | `type_Texture` | texture handle |
| 7 | `type_Color` | `RedColor*` |
| 8 | `type_String` | `wchar_t*` |
| 9 | `type_Vector3` | vector pointer |

`Event::GetDataFloat` coerces `type_Int` / `type_Uint` to float and returns `0.0` for
anything else, so type mismatches are silent, not fatal.

### API

```cpp
// registry
EventClass* EventClass::Create(Type, const char* fmt, ...);   // printf-style name, always creates
EventClass* EventClass::FindByHashID(uint hash);              // linear walk of sList
EventClass* Item::CreateEventA(const char* name, Type);       // filter + find, else Create; AddRef on hit
uint        EventClass::AddRef(EventClass*);
uint        EventClass::RemoveRef(EventClass*);               // deletes at 0
void        EventClass::DestroyAll();

// subscribe
void EventHandler::Init(EventHandler*, void (__cdecl *fn)(Event*, void*), void* data);
void EventClass::RegisterEventHandler(EventClass*, EventHandler*);
void EventClass::UnregisterEventHandler(EventClass*, EventHandler*);
bool Item::ReadEvent(Data*, EventHandler*);                   // config-side bind by name

// fire
void Event::Send(Event*);                                     // -> EventClass::Send -> every handler
void Event::SendDelayed(Event*, float seconds);               // -> EventQueue::AddEvent
```

`EventClass::Send` is a plain forward walk of `mHandlerList`; there is no reentrancy
guard and no priority. `EventHandler::HandleEvent` is literally `mFunc(event, mData)`.

`Item::ReadEvent` resolves the name and, when nothing matches, logs
`"HUD Element unable to find event %s"` at warning severity and returns `true`.
A misspelled event name is therefore silent in release: the element simply never updates.

### Address table

| Symbol | Phantom | Modtools |
|---|---|---|
| `HUD::EventClass::Create` | `0x0060F400` | `0x006BB130` (thunk `0x0040A35D`) |
| `HUD::EventClass::EventClass` | `0x0060F250` | |
| `HUD::EventClass::FindByHashID` | `0x0060F4D0` | `0x006BB1F0` (thunk `0x004077B6`) |
| `HUD::EventClass::AddRef` | `0x0060F3F0` | |
| `HUD::EventClass::RemoveRef` | `0x0060F7C0` | |
| `HUD::EventClass::DestroyAll` | `0x0060F480` | |
| `HUD::EventClass::RegisterEventHandler` | `0x0060F760` | |
| `HUD::EventClass::UnregisterEventHandler` | `0x0060F870` | |
| `HUD::EventClass::Send` | `0x0060F800` | inlined into `Event::Send` |
| `HUD::EventClass::sList` | `0x009D9EE0` | `0x00AF07FC` |
| `HUD::Event::Send` | `0x0060F7F0` | `0x006BB3F0` (thunk `0x00409881`) |
| `HUD::Event::SendDelayed` | `0x0060F840` | |
| `HUD::EventHandler::Init` | `0x0060F740` | |
| `HUD::EventHandler::HandleEvent` | `0x0060F720` | |
| `HUD::EventQueue::AddEvent` | `0x0060F940` | |
| `HUD::EventQueue::Update` | `0x0060FA40` (thunk `0x004064F6`) | `0x006BB680` |
| `HUD::gEventList` | | `0x00BDB110` (32 x 12 bytes) |
| `HUD::RemoveAllEvents` | `0x0060FA20` | |
| `HUD::Item::CreateEventA` | `0x00617370` | `0x006C5210` (thunk `0x00412995`) |
| `HUD::Item::ReadEvent` | `0x00617E30` | `0x006C5100` |
| `HUD::Item::GetFilteredEventName` | `0x00617540` | `0x006C5010` (thunk `0x0040944E`) |
| `HUD::Item::SetEventFilter` | `0x00618080` | |
| `HUD::GameEvents::Open` | `0x00611F90` | `0x006BC950` (thunk `0x004164FF`) |
| `HUD::GameEvents::Update` | `0x00613390` (thunk `0x0040DEE0`) | |
| `HUD::Manager::Open` | `0x00619970` | `0x006C7D50` |
| `HUD::Manager::Load` | `0x00619510` (thunk `0x004180BB`) | `0x006C77D0` |
| `HUD::Manager::Update` | `0x0061A270` | `0x006C6290` |
| `HUD::Manager::Close` | `0x00619070` | |
| `HUD::Alloc` | `0x00619030` | |

---

## Event name filtering (split-screen templating)

`Item::SetEventFilter(const char* filter, uint index)` sets two globals,
`sEventFilter` and `sEventIndex`. Both `Item::ReadEvent` and `Item::CreateEventA`
pass every name through `Item::GetFilteredEventName` first.

The filter walks the name and the filter string in lockstep. If they diverge at a
`%` in the filter, the matched prefix is kept, `sEventIndex` is printed in decimal,
the digits in the source name are skipped, and the remainder is appended. With no
filter set the name passes through verbatim.

```
filter "player%", index 3, name "player1.weapon2.change"
  -> "player3.weapon2.change"
```

`ViewPort::Read` is what drives this: it re-reads the **same** config scope once per
camera, calling `SetEventFilter(mEventFilter, mCurViewPort)` before each pass. A
`ViewPort` block is therefore a template that gets instantiated per player with its
event names renumbered. That is why `hudtransforms.hud` declares `player1.*` once
inside `ViewPort("Transforms") { EventNameFilter("player%") ... }`.

Anything calling `CreateEventA` from outside the config reader inherits whatever
`sEventFilter` was left at. Call `EventClass::Create` directly to bypass it.

---

## Built-in event catalogue

`HUD::GameEvents::Open` is a single 0xEF8-byte function containing **174**
`EventClass::Create` call sites covering **173** distinct format strings. There are
exactly three loops in it: weapons (x2), teams (x2) and statistics (x5). There is no
outer per-player loop, so **only the `player1.*` set is registered on PC**.

`%d` placeholders are filled by varargs at creation time, so the registered names are
literal (`player1.weapon2.heat`, not `player%d.weapon%d.heat`).

Global (17):

```
Float   objectivetimer                Bool  objectivetimer.disable
Float   victorytimer                  Bool  victorytimer.disable
Float   defeattimer                   Bool  defeattimer.disable
Uint    hintPopup                     Bool  hintPopup.disable
String  hintPopup.pageNumber
Uint    objectivePopup                Bool  objectivePopup.disable
Uint    selectionPopup                Bool  selectionPopup.disable
Uint    targetResetCommon
Bool    levelHintText                 Bool  initialize
Float   time
```

Per player (`player1.`):

```
Bool    spawn / die
Uint    health                  Bool  healthDisable
Float   healthFraction / bonusHealthFraction / healthRegenPulseRate
Uint    healthInVehicle         Bool  healthInVehicleDisable
Float   healthInVehicleFraction
Uint    hero.health             Float hero.healthFraction        Bool hero.healthDisable
Uint    vehicle.health          Float vehicle.healthFraction     Bool vehicle.healthDisable
Model   vehicle.seatingMesh
Bool    jetDisable              Float jetFuelFraction / jetFuelWarning / jetFuelThreshold
Float   energy / energyFraction / energyOverburn / energyRegenPulseRate
Bool    energyDisable
Bool    objectivelist.enable / objectivelist.disable
Bool    objectivesUpdated[.disable] / objectiveDetails[.disable]
Bool    hintsAvailable[.disable] / pressSelectToReturn[.disable]
Bool    spaceAssaultStatus[.disable]
Float   vehicle.hackingTime     String vehicle.hackingTimeFraction   Bool vehicle.hackingTimeDisable
Float   vehicle.hackedTime      String vehicle.hackedTimeFraction    Bool vehicle.hackedTimeDisable
Bool    weaponsOverheat / weaponsEnable / weaponsDisable
Bool    map.hideCPs / map.modeToggle / map.enable / map.disable
Bool    map.spawn / map.spawnLarge / map.spawnLargeDisable
Int     map.mode                Int   index
Uint    map.refreshTarget / map.refreshPost / map.refreshMarker / targetResetPlayer
Bool    spawnDisplay.enable / spawnDisplay.disable
Uint    spawnDisplay.message    String spawnDisplay.vehicle / spawnDisplay.spawninfo
Uint    tooltips                Bool  tooltips.disable
String  message                 Color message.color               Bool message.disable
Bool    statistic.changed / statistic.disable
Float   heroSelect.timerFraction / heroSelect.timer
String  heroSelect.message      Bool  heroSelect.disable
Float   commandPost.charge      Bool  commandPost.disable
Color   commandPost.color / commandPost.disputeColor
Bool    commandPost.disputeEnable / commandPost.disputeDisable
String  lockOnName / lockOnClassName / lockOnShieldName
Uint    lockOnHealth            Float lockOnHealthFraction / lockOnDistance
Bool    lockOnDisable / lockOnDisableShieldName / lockOnDirectionDisable
Vector3 lockOnDirection         Color lockOnTeamColor
Bool    lockOnFlagCarrier / lockOnFlagCarrierDisable
Bool    missileLock / missileLockDisable    Uint missileLockDistance
Float   reticule.alpha
Bool    flag.{friend,enemy}.{carried,dropped}[.disable]
Int     flag.{friend,enemy}.{carried,dropped}.number[.disable]
Bool    flag.player.carried / flag.player.dropped / flag.player.disable
```

Per weapon (`player1.weapon1.` and `player1.weapon2.`, 26 each):

```
Uint    change                  Bool  disable
Uint    totalAmmoBullets / totalClipBullets
Float   totalAmmoFraction / totalClipFraction
Bool    ammoInfinite            Float heat / charge / refire
String  name
String  target.name / target.className / target.shieldName
Uint    target.health           Float target.healthFraction
Bool    target.disable / target.disableShieldName
Color   target.teamColor / target.teamColorBright / target.hitColor
Bool    target.hit / target.hitCritical
Bool    reticule.disable        Vector3 reticule.position
Vector3 lockOnPosition          Bool  lockOnDisable
```

Per team (`player1.team1.` and `player1.team2.`, 9 each):

```
Int     points                  Bool  pointsDisable      String pointsText
Int     reinforcements          Float reinforcementsFraction
Bool    reinforcementsDisable
Texture texture                 Bool  textureDisable     Float  bleedRate
```

Per statistic (5 iterations, `%s` filled from a name table):

```
Int     statistic.<name>        String statistic.<name>Delta
```

Two of these are present on modtools but absent from Phantom
(`weaponN.target.teamColorBright` and `reticule.alpha`), which is what the stock
`EventAlpha("player1.reticule.alpha")` binding uses. Phantom is the older dev build.

The engine-side mirror of this list is the `HUD::PlayerEvents` struct (796 bytes,
`EventClass*` per member, `WeaponEvents[2]` at `+104` and `TeamEvents[2]` at `+456`).

---

## Consumers: which element reads which event

| `.hud` key | Class | Handler |
|---|---|---|
| `EventEnable` / `EventDisable` | `Element` | `Element::EventEnable` / `EventDisable` |
| `EventChanged` | `Element` | flips to `ColorChange` colour |
| `EventColor` | `Element` | requires `type_Color` |
| `EventPulseRate` | `Element` | drives `mPulseColorRate` |
| `EventFadeOut` | `Element` | **output**, see below |
| `EventValue` | `ElementBar` | bar fill |
| `EventBitmap` | `ElementBitmapBase` | texture swap |
| `EventText` / `EventNumber` | `ElementText` | string / number |
| `EventText` / `EventColor` | `ElementMultilineText` | scrolling log |
| `EventPosition` / `EventScale` / `EventRotation` | `ElementGroupBase` | transform |
| `EventPlayerIndex` | `ElementGroupPlayer` | re-targets the group |
| `EventMesh` | `ElementModel3D` | mesh swap |
| `EventToggleMapMode` / `EventChangeMapMode` / `EventPostHide` / `EventRefreshTarget` / `EventRefreshPost` / `EventRefreshMarker` | `ElementMap` | minimap |
| `EventResetTargetCommon` / `EventResetTargetPlayer` | `ElementTarget` | 3D target markers |
| `EventTrigger` / `EventStop` | `Sound` | plays a `GameSound` |
| `EventInput` | `Transform` | **input** |
| `EventOutput` | `Transform` | **output** |
| `EventBlend` / `EventAlpha` / `EventInputFactor` | `TransformNumberColorBlend` | extra inputs |

---

## Adding brand new HUD events

Short answer: **easy, and the stock game already does it from data.**

### Tier 1: pure data, no code (already shipped)

`HUD::Transform::ReadData` handles exactly two keys:

```cpp
if (data->m_uiId == 0x6903ED32) {                    // EventOutput
    this->mEventClassOutput = Item::CreateEventA(GetStringArg(data, 0), type);
    return true;
}
if (data->m_uiId == 0xCCE718ED) {                    // EventInput
    return Item::ReadEvent(data, &this->mEventInput);
}
```

`CreateEventA` is get-or-create: `FindByHashID` first, `AddRef` on a hit, otherwise
`EventClass::Create`. So **`EventOutput("anything.you.like")` registers a brand new
named event**, and any element in any `.hud` file can then subscribe to it by name.

This is not theoretical. Every one of these stock event names is created this way and
exists nowhere in the engine:

```
player1.energyColor              player1.healthColor
player1.jetFuelColor             player1.vehicle.healthColor
player1.weapon1.mesh             player1.weapon2.mesh
player1.weapon1.chargeColor      player1.weapon1.chargeRotate
player1.weapon1.chargeScale      player1.weapon1.heatcolor
player1.weapon2.chargeColor      player1.weapon2.chargeRotate
player1.weapon2.chargeScale
```

`Element::ReadData` has a second data-driven creation path: `EventFadeOut(name)` calls
`CreateEventA(name, type_Bool)`, and `Element::Update` **sends** it when the fader
reaches Sustain/Release or Inactive. So any element can emit a new named event when it
finishes fading out.

The limit of tier 1 is where the *value* comes from. A `Transform` output value is a
function of one engine event's value (name to mesh, number to colour, number to
vector3, colour blend). An `EventFadeOut` is a bare `true` pulse. You cannot invent a
value the engine does not already publish.

Practical recipe for a purely data-driven new event:

```
ViewPort("MyStuff")
{
    EventNameFilter("player%")
    TransformNumberColor("player1lowAmmoTint")
    {
        NumberColor(0.00, 255,  32,  32)
        NumberColor(0.25, 255,  32,  32)
        NumberColor(0.26, 255, 255, 255)
        NumberColor(1.00, 255, 255, 255)
        EventInput("player1.weapon1.totalAmmoFraction")
        EventOutput("player1.weapon1.ammoTint")     // new event
    }
}
```

then anywhere else `EventColor("player1.weapon1.ammoTint")`.

### Tier 2: native, from GameExt

Everything needed is a handful of small non-virtual functions with trivial structs.
There is no allocation to manage on the fire path: `HUD::Event` is an 8-byte POD with
an empty destructor.

```cpp
// resolve once per game session
using Create_t   = void* (__cdecl*)(int type, const char* fmt, ...);
using Send_t     = void  (__thiscall*)(void* self);          // naked thunk on release
struct HudEvent { void* cls; union { int i; unsigned u; float f;
                                     const wchar_t* s; void* p; } d; };

void* cls = Create(4 /*type_Float*/, "gameext.myvalue");     // registers the name
HudEvent e{ cls, {} }; e.d.f = 0.75f;
Send(&e);                                                     // every subscriber fires
```

Constraints that actually matter:

1. **Lifetime is per game session.** `Manager::Close` calls `EventClass::DestroyAll`,
   which frees every `EventClass` unconditionally. Create yours after
   `GameEvents::Open` and never cache the pointer across a level change. The right hook
   point is a detour on `HUD::GameEvents::Open` that runs the original and then appends
   your own `Create` calls, exactly where the engine does it. This is the same
   per-game-state lifetime as the weapon class factory (`docs/RE/WeaponClassFactory.md`).
2. **Order matters.** A `.hud` file can only bind to an event that already exists,
   because `Item::ReadEvent` resolves at parse time. `Manager::Open` runs
   `GameEvents::Open` before any `Manager::Load`, so creating from a `GameEvents::Open`
   detour is early enough for config files to subscribe. Creating later means only
   native subscribers can attach.
3. **Heap.** `EventClass::Create` allocates 32 bytes plus a name copy from
   `__RedCurrHeap`. Wrap the call in
   `RedSetCurrentHeap(GameMemory::RunTimeHeap)` / restore, the way `Manager::Open`
   does, or the object lands on whatever heap happens to be current.
4. **Bypass the filter.** Call `EventClass::Create` directly rather than
   `Item::CreateEventA`, so a stale `sEventFilter` cannot rewrite your name.
5. **String payloads are `wchar_t*`** and are not copied. The pointer must outlive the
   `Send`, which in practice means it must outlive the synchronous handler walk only.
6. `SendDelayed` shares one 32-slot global queue with the engine. Do not spam it.

### Tier 3: expose it to Lua

There is currently **no Lua binding that touches the HUD event bus at all.** The 90-odd
HUD-adjacent `ScriptCB_*` / `Show*` / `Map*` callbacks all drive fixed built-in events
through `GameEvents` helpers. Adding two functions would open the whole system to
scripts:

```lua
HudEventCreate("gameext.myvalue", "float")   -- register (idempotent)
HudEventSend("gameext.myvalue", 0.75)        -- fire
```

with `HudEventSend` doing `FindByHashID(PblHash(name))`, a type check against
`EventClass::GetType`, and a stack `HudEvent` + `Send`. That is roughly 60 lines in
`lua_funcs.cpp` plus the address table entries.

Caveat worth stating up front: **HUD events are client-side presentation only.**
`GameEvents::Update` reads the local player's state and fires locally. A Lua-driven
HUD event fires on whichever machine runs the script, so in multiplayer it is
host-only unless separately replicated. See `docs/RE/` notes on MP scripting
constraints.

### What is not feasible

- More than one player's built-in events on PC. `GameEvents::Open` registers the
  `player1.*` set only, and there is exactly one `PlayerEvents` instance. Adding
  `player2.*` would mean synthesising both the registry entries and the per-player
  state feed.
- More than 32 simultaneous delayed events without relocating `gEventList`.

---

## The HUD editor

`HUD::Editor` is a complete in-game layout editor: element list, property browser,
live nudging, and a "generate `.hud` file" writer (`Item::Write` / `Element::WriteData`,
one `WriteData` per class). `gEditor` is constructed unconditionally in
`Manager::Open`; only the 1 MB `HUDEditHeap` is gated on `__RedDebugHeap != -1`.

`Editor::KeyboardEvent` on Phantom toggles `mMode` between `mode_Disabled` and
`mode_SelectElement` on a `KEYCHAR` of `0x12` with modifier bit 0 set, and toggles the
backdrop on `'0'`. Community documentation for the modtools debug build
(`data_HUD/HUD Tutorial.txt` by Anakin) gives the toggle as Ctrl+E, so the exact
binding on the shipped modtools build should be re-checked rather than taken from
Phantom.

The editor writes its output to `GameData\Data\` (or the VirtualStore redirect).

---

## Open questions

- Steam and GOG addresses are not derived. Anchors for porting: the strings
  `"HUD Element unable to find event %s"`, `"Too many events added to the HUD::EventQueue! Max is %d"`,
  `"objectivetimer"`, `"HUDEditHeap"`, and the `hud_` FourCC `0x5F647568` in
  `LoadUtil::ReadDataFileChunk`.
- Reader-key hashes `0xCA0B9CCB`, `0x0DD385E2`, `0x3455B0A8`, `0x6242102C`,
  `0xBDBFD84F`, `0x5D23BD7D`, `0x04ABB610` in `Element::ReadData` / `ViewPort::ReadData`
  are unresolved. The two `ViewPort` ones are almost certainly `Viewport1Enable` /
  `Viewport4Position`-shaped names; brute-force against the formula above.
- Whether the editor's keyboard path is reachable on retail, which would make it a
  shipped-build layout tool rather than a modtools-only one.
