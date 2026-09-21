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

> **Modtools column corrected 2026-08-26.** The first version of this table was
> derived from `E:\BF2_Modtools\BF2_modtools.exe`, which is a **different build**
> from the one this project targets (`GameData\BattlefrontII.Debug.FullScreen.1080.exe`,
> the image the MemExt Ghidra program matches). Every modtools address below was
> re-derived against the correct image; entries marked "not re-derived" came from
> the wrong-image set and have been cleared rather than left as landmines.

| Symbol | Phantom | Modtools |
|---|---|---|
| `HUD::EventClass::Create` | `0x0060F400` | `0x006AD8A0` (thunk `0x0040A394`) |
| `HUD::EventClass::EventClass` | `0x0060F250` | `0x006AD740` |
| `HUD::EventClass::FindByHashID` | `0x0060F4D0` | not re-derived |
| `HUD::EventClass::AddRef` | `0x0060F3F0` | |
| `HUD::EventClass::RemoveRef` | `0x0060F7C0` | |
| `HUD::EventClass::DestroyAll` | `0x0060F480` | |
| `HUD::EventClass::RegisterEventHandler` | `0x0060F760` | |
| `HUD::EventClass::UnregisterEventHandler` | `0x0060F870` | |
| `HUD::EventClass::Send` | `0x0060F800` | inlined into `Event::Send` |
| `HUD::EventClass::sList` | `0x009D9EE0` | not re-derived |
| `HUD::Event::Send` | `0x0060F7F0` | `0x006ADA90` |
| `HUD::Event::SendDelayed` | `0x0060F840` | |
| `HUD::EventHandler::Init` | `0x0060F740` | |
| `HUD::EventHandler::HandleEvent` | `0x0060F720` | |
| `HUD::EventQueue::AddEvent` | `0x0060F940` | |
| `HUD::EventQueue::Update` | `0x0060FA40` (thunk `0x004064F6`) | not re-derived |
| `HUD::gEventList` | | not re-derived |
| `HUD::RemoveAllEvents` | `0x0060FA20` | |
| `HUD::Item::CreateEventA` | `0x00617370` | not re-derived |
| `HUD::Item::ReadEvent` | `0x00617E30` | not re-derived |
| `HUD::Item::GetFilteredEventName` | `0x00617540` | not re-derived |
| `HUD::Item::SetEventFilter` | `0x00618080` | |
| `HUD::GameEvents::Open` | `0x00611F90` | `0x006AEF00` |
| `HUD::GameEvents::Update` | `0x00613390` (thunk `0x0040DEE0`) | |
| `HUD::Manager::Open` | `0x00619970` | not re-derived |
| `HUD::Manager::Load` | `0x00619510` (thunk `0x004180BB`) | not re-derived |
| `HUD::Manager::Update` | `0x0061A270` | not re-derived |
| `HUD::Manager::Close` | `0x00619070` | |
| `HUD::Alloc` | `0x00619030` | |
| `HUD::Event::GetClass` | | `0x006AD590` (`mov eax,[ecx]`) |
| `HUD::Event::GetData` | | `0x006AD5A0` (`mov ecx,[ecx+4]`) |
| `HUD::EventClass::GetType` | | `0x006AD410` (`mov eax,[ecx+4]`) |

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

`Editor::KeyboardEvent` toggles `mMode` between `mode_Disabled` and
`mode_SelectElement` on a `KEYCHAR` of `0x12` with modifier bit 0 set, and toggles the
backdrop on `'0'`. Verified on Phantom, on modtools (ctor `0x0068F310` writes vtable
`0x00A5C290` and `mMode = 1`) and on both retail builds, whose two bodies are
byte-for-byte identical to each other. Community documentation for the modtools debug build
(`data_HUD/HUD Tutorial.txt` by Anakin) gives the toggle as Ctrl+E, so the exact
binding on the shipped modtools build should be re-checked rather than taken from
Phantom.

The editor writes its output to `GameData\Data\` (or the VirtualStore redirect).

### It is still live on retail

The whole editor is present and reachable on Steam and GOG. `Manager::Open` builds
`gEditor` unconditionally there too, and `Editor::Update`'s `mMode - 1` jump table
still has all four arms including the mode-2 navigation code. The navigation input
path is intact as well: the button-to-key mapper (steam `FUN_00546D30`) maps its 14
button indices to the same DirectInput scancodes (`0xCB` left, `0xCD` right, `0xC8` up,
`0xD0` down, numpad `0x4B/0x4D/0x48/0x50`, `HOME/END/PGUP/PGDN/DEL/INS`) and reads
them out of `joystick + 0x2608`, which `FLInputManager` populates on retail
(`FUN_0052AA20`: `if (GetNumKeyboards()) kbd = GetKeyboard(0)`).

So nothing statically explains why the editor cannot be driven on retail - the
reported behaviour is that it opens and then does not respond. The untested
candidate is `SetMode`'s own bail-out: entering mode 2 does

```
mFile = ConfigFile::GetFirst();
if (mFile == 0 || mFile->[0x0C] == 0) mMode = 1;   // silent revert
if (mFile == 0) goto resume;
mHighlight(); GameLoop::Pause();                    // runs anyway
... Enable(this, true);
```

which, on an empty config list, pauses the game and shows the editor while leaving
`mMode` at 1, so `Editor::Update` falls through to its no-op arm. That would need a
runtime check to confirm, and it does not explain a second toggle press failing.

Either way the retail-facing consequence is the same: `SetMode(2)` calls
`GameLoop::Pause()`, so the player freezes the game behind a tool they cannot use.

### Removal on retail (`hud_editor_disable.cpp`)

`Editor::SetMode` has exactly two callers - `Editor::Update` (its own bookkeeping,
which only ever re-applies the static `sMode`) and `Editor::KeyboardEvent` - so
`KeyboardEvent` is the single door in. BF2GameExt overwrites its entry with its own
epilogue (`C2 14 00`, `RET 0x14`) on Steam and GOG:

| Build | `Editor::Editor` | vtable (from the ctor's own store) | slot 2 = `KeyboardEvent` |
|---|---|---|---|
| Modtools | `0x0068F310` | `0x00A5C290` | `0x00690E70` (via ILT `0x00404066`) - **not patched** |
| Steam | `0x00544C90` | `0x007A0578` | `0x00546E20` |
| GOG | `0x005459E0` | `0x007A13D4` | `0x00547B70` |

Each has exactly one vtable xref, so none is a COMDAT-folded body shared with
another class. With the key dead, `sMode` never leaves its BSS `0`, and the first
`Editor::Update` after each `Manager::Open` drives `mMode` 1 -> 0 through
`SetMode(0)` exactly as it already did.

**Do not disable the editor by skipping its construction in `Manager::Open`.**
`Manager::Close` calls `gEditor`'s vtable slot 0 with no null check
(`(**(code **)*gEditor)(0)` at modtools `0x006B8250`), so a null `gEditor` turns a
dead feature into a crash on map teardown.

---

---

## Collections: how the HUD does "N of something"

There is no repeater or list primitive, and there is no `post%d.*` / `marker%d.*` event
family. The engine's idiom for a variable-length collection is different, and it is
worth understanding before designing anything dynamic:

> **one event carrying an index, plus a fixed-size array of pre-built child elements
> owned by a specialised element class.**

`ElementMap` is the clearest case. `player1.map.refreshPost` is `type_Uint` and its
payload is the **command post index**:

```cpp
void __cdecl HUD::ElementMap::EventRefreshPost(Event* e, void* self) {
    if (EventClass::GetType(e->GetClass()) != type_Uint) return;
    uint i = e->GetData().uintValue;
    if (i > 0xF) return;                       // hard cap
    CommandPost* cp = TargetManager::gPost[i].actor;
    ...
}
```

The element then owns parallel 16-entry arrays of icon/text sub-elements, seeded from
the `PostLarge` / `PostSmall` / `PostSpawn`, `PostSelect*` and `PostText*` templates in
the `.hud` file. Same pattern for `EventRefreshTarget` and `EventRefreshMarker`.

`ElementVehicleSeating`, `ElementTarget` and `ObjectiveList` are the other collection
classes; `ElementMultilineText` is the same trick for text lines.

### Command posts specifically

| Fact | Detail |
|---|---|
| Storage | `TargetManager::gPost[16]`, `Post` = 48 bytes (`CommandPost* actor` + `MapParams`) |
| Hard cap | **16**, checked in both `ElementMap::GetPost` (`i < 0x10`) and `EventRefreshPost` (`i > 0xF` rejects) |
| Post team | `GameObject+0x234`, the 4-bit signed `mTeam` bitfield (see `team_count_hard_limit`) |
| Icon colour | `GetPlayerTeam(this)->mColor[postTeam]` - the palette is **relative to the viewing player**, which is what makes `ColorFriendly` work |
| Contested flash | when `post->actor->mHoldTeam != postTeam`, crossfades between the two team colours on `fmod(GetMissionTime(), 0.25)`, 0.125 s each direction |
| Hide all | `player1.map.hideCPs` -> `ElementMap::EventPostHide` -> `HideCommandPosts` |

The only HUD events that mention command posts are the six
`player1.commandPost.{charge,disable,color,disputeColor,disputeEnable,disputeDisable}`,
and they all describe the **single post the player is currently capturing**, not the set.

So a per-CP display already exists and is genuinely dynamic - it is the minimap's post
icons. What does not exist is any way to render that set outside `ElementMap` from
data, because only `ElementMap` knows how to walk `TargetManager::gPost`.

Lua already has the state: `GetCommandPostTeam`, `GetCommandPostCaptureRegion`,
`GetCommandPostBleedValue`. It has no channel to push it at the HUD.

### Building a custom command-post strip

Tier 2 work, and small. Do **not** try to mimic the index-carrying pattern - a generic
`Element` cannot demultiplex an index. Register one event per post per field instead:

```cpp
// from a GameEvents::Open detour, after calling the original
for (int i = 1; i <= 16; ++i) {
    gCP[i].color    = EventClass::Create(type_Color,  "commandpost%d.teamColor", i);
    gCP[i].present  = EventClass::Create(type_Bool,   "commandpost%d.present",   i);
    gCP[i].disputed = EventClass::Create(type_Bool,   "commandpost%d.disputed",  i);
    gCP[i].name     = EventClass::Create(type_String, "commandpost%d.name",      i);
}
```

then pump them from a `GameEvents::Update` detour by walking `TargetManager::gPost`,
sending only on change (the engine's own `GameEvents` helpers all do change-detection
against a cached `gPlayerData` copy; copy that discipline or every element re-renders
every frame). 64 extra `EventClass` objects is 2 KB plus names.

A `.hud` file can then lay out sixteen groups with `EventEnable("commandpost3.present")`
and `EventColor("commandpost3.teamColor")` and get a real capture-status strip.

---

## The floating weapon icon bug (community `extraweapons.hud` fixes)

The community fix for custom-weapon HUD icons is a pair of items per weapon channel:

1. a `TransformNameMesh` mapping each custom weapon mesh name to `com_inv_mesh`, wired
   `EventInput("player1.weaponN.change")` -> `EventOutput("player1.weaponN.mesh")`, which
   blanks the **stock** icon element;
2. the mod's own `Model3D` bound to the raw `EventMesh("player1.weaponN.change")` with
   `Scale(0,0,0)` as the default and a hand-placed `MeshInfo` per supported weapon.

Loading two such fixes breaks both. The cause is the miss path in the transform.

### Root cause

```cpp
void __cdecl HUD::TransformNameMesh::EventInput(Event* e, void* self) {
    uint hash = 0;
    if (EventClass::GetType(e->GetClass()) == type_Uint) hash = e->GetData().uintValue;
    if (self->mEventClassOutput == 0) return;        // +0x30
    if (hash == 0) return;
    if (self->mNumMappings != 0) {                   // +0x3C
        NameMesh* nm = FindNameMesh(self, hash);     // bsearch over mMapping (+0x38)
        if (nm) {
            RedModel* m = NameMesh::GetMesh(nm, Item::GetName(self));
            if (m) goto send;
        }
    }
    // ---- MISS PATH: not silent ----
    RedModel* m = PblHashTableCode::_Find(RedModel::_HashTable, 0x800, hash);
    if (!m) return;
    m->flags |= 1;
send:
    Event ev(self->mEventClassOutput, m);
    ev.Send();
}
```

On a lookup miss the transform does **not** stay quiet. It resolves the incoming hash as
a `RedModel` and sends the **original, unmapped mesh** to its output event.

`EventClass::RegisterEventHandler` appends at the tail and `EventClass::Send` walks head
to tail, so handlers fire in registration order and **the last-parsed `.hud` file wins**.
With two fixes bound to the same input and output events:

| Weapon | Mod A (has mapping) | Mod B (no mapping) | Result today |
|---|---|---|---|
| A's weapon | sends `com_inv_mesh` | falls through, sends the real mesh | B wins - stock icon reappears **and** A's own `Model3D` draws it: the double icon |
| B's weapon | falls through | sends `com_inv_mesh` | A fires first, B second, correct by luck |
| neither | falls through | falls through | same value twice, harmless |

The same defect breaks **stock** remaps, which is independently testable: stock
`hudtransforms.hud` maps `cis_weap_inf_wrist_trishot` -> `hud_cis_trishot`. Any
extraweapons fix loaded after it has no entry for the trishot, falls through, and
re-sends the world mesh. Load one of these fixes and the CIS wrist trishot icon should
show the world model instead of its HUD icon.

`NameMesh::GetMesh` returning null (mapping present, mesh not in the `.req`) takes the
same fallback path, so a broken mapping produces the identical symptom. On Phantom it
first logs `"TransformNameMesh %s : unable to find model %s associated with name %s"`,
gated once per entry by `mDisplayedWarning`.

### The fix: mapped beats unmapped

**Shipped, and confirmed working on modtools.** One detour on `TransformNameMesh::EventInput`.
Before running the original: if this transform has no mapping for the hash, but some
other transform sharing the same `mEventClassOutput` does, return without sending.

Implemented in `PatcherDLL/src/render/hud_weapon_icon_fix.cpp`, INI `[Fixes] WeaponIconFix`.

```cpp
static void __cdecl hooked_TNM_EventInput(HudEvent* e, void* self)
{
   if (EventClass_GetType(e->cls) == type_Uint) {
      unsigned hash  = e->d.u;
      void*    myOut = *(void**)((char*)self + 0x30);
      if (hash && myOut && !FindNameMesh(self, hash)) {
         // walk sTransformNameMeshList; the node sits at item + 0x40
         for (Node* n = *(Node**)kList; n != (Node*)kList; n = n->next) {
            void* t = (char*)n - 0x40;
            if (t == self) continue;
            if (*(void**)((char*)t + 0x30) != myOut) continue;
            NameMesh* nm = FindNameMesh(t, hash);
            if (nm && NameMesh_GetMesh(nm, Item_GetName(t)))
               return;                    // someone real will answer; stay quiet
         }
      }
   }
   orig_TNM_EventInput(e, self);
}
```

Properties:

- **Order independent.** Whichever file loads last no longer decides.
- **No data changes.** Existing mod `.hud` files are fixed as shipped, with no
  cooperation between mod authors.
- **Backward compatible.** One transform behaves identically. No transform having the
  mapping still yields the passthrough. Two transforms both mapping the same weapon is a
  genuine conflict and stays last-wins.
- **Repairs the stock remaps** as a side effect.
- Cost is an O(N) walk only on the miss path, only on weapon change, with N = the number
  of `TransformNameMesh` items (single digits in practice).

`FindNameMesh` builds a stack `NameMesh`, `Init`s it with `(hash, 0, false)` and
`bsearch`es `mMapping`; it mutates nothing, so calling it speculatively on other
transforms is safe.

The alternative - merging duplicate transforms at load time so only one handler ever
registers - gives a cleaner runtime but is more invasive and makes the 256-entry cap
shared across all mods.

### Addresses

Modtools came from the MemExt Ghidra program. Steam and GOG were located through
RTTI, which survives on retail even though the RedWarning strings do not: the type
descriptor `.?AVTransformNameMesh@HUD@@` leads to the COL, the COL to the vtable,
and the two stores of that vtable are the constructor and the deleting destructor.
The constructor then hands over everything else, since it pushes `EventInput` into
`mEventInput` and links `+0x40` into the list:

```
005675F4  mov [esi],0x7A35D8      <- vtable            (Steam)
005675FA  lea ecx,[esi+0x40]      <- mTransformNameMeshNode
0056761B  mov eax,[0x7EBABC]      }  push-front link into
00567623  mov [0x7EBABC],ecx      }  sTransformNameMeshList
00567629  lea ecx,[esi+0x1C]      <- mEventInput
0056762C  push 0x567AB0           <- EventInput
```

| Symbol | Phantom | Modtools | Steam | GOG |
|---|---|---|---|---|
| `TransformNameMesh::EventInput` | `0x0061C8E0` | `0x006BB610` | `0x00567AB0` | `0x00568830` |
| `TransformNameMesh::FindNameMesh` | `0x0061CA00` | `0x006BB230` | `0x00567A60` | `0x005687E0` |
| `sTransformNameMeshList` | `0x009D9F90` | `0x00AD8A10` | `0x007EBABC` | `0x007ECA8C` |
| `TransformNameMesh::TransformNameMesh` | | | `0x005675E0` | `0x00568360` |
| `TransformNameMesh` vtable | | | `0x007A35D8` | `0x007A4418` |
| `TransformNameMesh::FindByHashID` | `0x0061C9B0` | `0x006BB6F0` | | |
| `TransformNameMesh::ReadData` | `0x0061CD50` | contains `0x006BBA06` | | |
| `NameMesh::GetMesh` | `0x0061CA60` | `0x006BB350` (thunk `0x0040AB46`) | `0x005674A0` | |
| `NameMesh::Init(name, meshName)` | `0x0061CB80` | `0x006BB740` | | |
| `Item::GetName` | `0x00617850` | `0x006B6190` (unverified) | | |
| `RedModel::_HashTable` | not derived | `0x00D4D964` | `0x0093EBDC` | |

Conventions were read off the disassembly on every build rather than assumed, which
matters because retail is LTCG: `EventInput` ends in a **bare `RET`** (so `__cdecl`,
two stack args) and `FindNameMesh` ends in **`RET 4`** (`__thiscall(this, uint)`) on
all three. The accessor offsets check out identically everywhere:

| | Phantom | Modtools | Steam |
|---|---|---|---|
| `Event::GetClass` -> `mClass +0x00` | | `0x006AD590` | `0x0055E120` |
| `EventClass::GetType` -> `mType +0x04` | | `0x006AD410` | `0x0055DDE0` |
| `Event::GetData` -> `mData +0x04` | | `0x006AD5A0` | `0x0055E130` |

The install byte-guards ten bytes of both prologues. There are two codegens: the
modtools debug build frames on ESP, the retail builds frame on EBP, and Steam and
GOG are byte-identical across those ten bytes.

```
modtools  EventInput    83 EC 08 53 56 8B 74 24 14 57
modtools  FindNameMesh  83 EC 14 56 8B F1 8D 4C 24 04
retail    EventInput    55 8B EC 8B 55 08 83 EC 08 8B
retail    FindNameMesh  55 8B EC 83 EC 14 56 8B F1 8D
```

A mismatch declines the install and writes the bytes it found to `BF2GameExt.log`.

`TransformNameMesh` layout: `mMapping` `+0x38`, `mNumMappings` `+0x3C`,
`mTransformNameMeshNode` `+0x40`, `mInheritNames[16]` `+0x44`, `mNumInheritNames`
`+0x84`. Inherited from `Transform`: `mEventInput` `+0x1C`, `mEventClassOutput` `+0x30`.
`NameMesh` is 20 bytes: `mHashID +0x00`, `mMesh +0x04`, `mName +0x08`, `mMeshName +0x0C`,
bitfield `+0x10` (bit0 `mDisplayedWarning`, bit1 `mInherited`, bit2 `mMeshIsID`).

### Two undocumented things found on the way

**`TransformNameMesh` has an inheritance keyword.** Inside a `TransformNameMesh` block,
a nested `TransformNameMesh("otherItemName")` line (hash `0x67C3C715`, the same hash as
the type keyword) resolves that name through `FindByHashID` and **copies every one of its
`NameMesh` entries into this one**, flagged `mInherited`. Up to 16 inherit names are
recorded in `mInheritNames` so `WriteData` round-trips them.

This is a real composition mechanism, but it does not fix the bug on its own: both
transforms still register and both still send, so it only helps if the last-loaded one
inherits all the others. It is also blunted in practice because every shipped
extraweapons fix names its transform `player1weapon1` / `player1weapon2`, exactly like
stock `hudtransforms.hud`, and `FindByHashID` returns the first match - so mods would
have to rename their transforms before they could inherit each other.

**`NameMesh` entries are capped at 256 per transform.** `TransformNameMesh::Read` builds
the table in a 256-entry stack scratch array, and `ReadData` warns
`"HUD TransformNameMesh can only store %d NameMeshs"` and silently drops anything past
255. Inherited entries count against the same budget. For reference the largest shipped
community table is `995_extraweapons.hud` at 70 entries.

## Floating elements: `EventPosition`, target events and the hit signal

Read 2026-09-19 for a floating, latching target health bar. **Addresses in this
section are Phantom**, not modtools, and are not ported.

### `EventPosition` takes HUD-space coordinates

`HUD::ElementGroupBase::EventPosition` (`0x005FB930`) requires `type_Vector3`, copies
the vector out of the payload immediately, and runs x and y through the same
conversion a static `Position()` line gets:

```c
mode = *(byte*)(self + 0x144);                          // the group's RelativeMode
x = ConvertRelativeToPixels(mode, v.x, ContainerFrameWidth,  ContainerViewWidth,  screenW);
y = ConvertRelativeToPixels(mode, v.y, ContainerFrameHeight, ContainerViewHeight, screenH);
RedInterfaceElement::SetPosition(self->mElement /* +0xB0 */, &v);   // z passes through
SetAlignment(self, hAlign /* +0x145 */, vAlign /* +0x146 */, false, false, true);
```

So the vector is in the units of the group's own `Position(x, y, z, "Viewport")` -
0..1 viewport fractions for a `"Viewport"` group. It **replaces** the static position
rather than offsetting it, and nothing is projected here: a world position has to be
projected by whoever sends the event. The payload pointer only has to outlive `Send`.
`EventScale` (`0x005FBDE0`) and `EventRotation` (`0x005FBAC0`) are its siblings.

### The stock projection, to copy exactly

`HUD::GameEvents::UpdateWeaponEvents` (`0x00615BC0`) produces `weaponN.lockOnPosition`:

```c
pt  = locked->GetTargetPoint(out, &ctrl->mTargetInfo.mAimStart, &ctrl->mEyeDir, bodyId);
pt += smoothedOrInterpolatedMatrix.trans - locked->GetMatrix()->trans;   // follow the RENDERED pose
cam = D3DXVec3TransformCoord(pt, camera->_MatrixInverse);
if (cam.z < 0) {                                      // in front of the camera
    camera->TransformCameraPointToProjectionSpace(&ndc, &cam);
    ndc.x = ndc.x * 0.5f + 0.5f;  ndc.y = ndc.y * 0.5f + 0.5f;  ndc.z = 0;
    Send(lockOnPosition, &ndc);
} else Send(lockOnDisable, true);                     // behind the camera: hide
```

The offset term uses `GetSmoothedMatrix`, replaced by
`GameObjectInterpolator::GetMatrix(mNetUniqueId)` when networking is live and
`netFrameLock` is clear. Without it a marker tracks the simulated position and jitters
on a multiplayer client.

### Why the stock target bar fades

The same function picks the target as `weapon->mTarget`, falling back to
`controllable->mReticuleTarget[channel]`, both handle-checked. Buildings pass; anything
else must be alive (`Damageable+0xB8` bit 3), and an entity-class flag (`+0x36C & 0x20`)
opts a class out. It compares the result with the cached `WeaponData.target` every tick
and on any change sends `targetDisable` + `targetDisableShieldName` and resets
`targetHealth` to -1. Pure aim tracking with no memory: look away and the bar is told
to disable on the next tick.

### The hit signal is anonymous

`target.hit` / `target.hitCritical` / `target.hitColor` come from three per-affiliation
timers on the player's `Character`:

```c
void Character::RegisterHit(int victimTeam, float mult) {          // 0x0049E810, thiscall
    a = mTeamPtr->mAffiliation[victimTeam];
    if (mObjectHitTimer[a+1] == 0 || mObjectHitMultiplier[a+1] < mult) {
        mObjectHitTimer[a+1] = 0.5f;  mObjectHitMultiplier[a+1] = mult;
    }
}
float Character::GetObjectHitValue(int a) { return min(mObjectHitTimer[a+1] * 10.0f, 1.0f); }  // 0x0049DE10
```

It records that *something* of an affiliation was hit, never *what*. Its only caller is
`Damageable::ApplyDamageCommon` (`0x004F6200`) at `0x004F6867`, reached only after two
local "this counted" flags pass and the damage owner has a `Character`. There the
attacker `Character*` is in `ECX` and the victim is still in `EDI` (its team is read from
`[EDI+0x234]` for the argument) - but the victim is not passed on. That call site is
therefore the one place a "who did I just hit" latch can be taken, and taking it there
makes the latch fire exactly when the stock hit marker does.

### The latched floating target bar (built 2026-09-19, `render/target_bar_latch.cpp`)

An earlier draft published a parallel `player1.focus.*` family of six events. That
was dropped: five of the six duplicated what `weaponN.target.*` already carries,
with the engine's own shield handling, name lookup, team colour, change detection
and dead/stale-handle checks. The agreed design makes the ENGINE's target sticky
and adds exactly one event.

**One new event:** `player1.weaponN.target.position` (`type_Vector3`, viewport
fractions). The existing bar keeps all its bindings and gains one line on its parent
group: `EventPosition("player1.weapon1.target.position")`.

Its anchor intentionally differs from the stock `lockOnPosition` point.
The first attempt took the top of a *projected* collision rectangle. It had two
problems: camera-dependent corner switching, and enormous projected extents near
the camera. The subsequent animated-joint envelope followed poses but made infantry
bars wobble with their animations. Both approaches were discarded.

**Current positioning (2026-09-21):** follow the healthbar-only derivation supplied
by the user from SWBFIII: choose the **top centre of a world bounding box first**,
with zero world-Y offset, then project that single point. No head bones, projected
silhouette extrema, POI icons or perspective scaling are involved.

- **Units:** retain the non-animated, stance-dependent collision-box size the user
  preferred. Re-centre it on the live collision centre and add the rendered-root
  translation delta. Native jump/flail states 4/8 update that centre but leave AABB
  stale (Steam `0x004E1210`, branch `0x004E1419`); re-centring preserves the prior
  stance dimensions without freezing the marker at the previous world position.
- **Vehicles/props:** transform the authored model box with the full smoothed
  matrix and form its world AABB. Do not use the sphere-derived collision cube
  (`UpdateAABB`, Steam `0x00463C70`), which exaggerates long vehicles' height.
- **Projection:** `anchor = ((minX+maxX)/2, maxY, (minZ+maxZ)/2)`. Camera rotation
  changes its projection, not which point on the box is chosen.
- **Up close/offscreen:** clamp the projected anchor to a built-in safe area.
  A small positive depth floor avoids division by zero/flips when the anchor
  crosses the eye plane but some of the bounds remain in front. Wholly
  behind-camera bounds and invalid data move the position offscreen.
- **Pixel alignment:** snap the anchor to framebuffer pixels using live screen
  dimensions, with the safe-area limits rounded inward. This does not rewrite
  bitmap sizes/child offsets, so it does not guarantee every child edge is an
  integer pixel when the HUD author uses fractional dimensions.
- **Death fade:** `tick_after` only projects a valid, living focus. Death or an
  expired handle retains that channel's last published screen anchor, including
  an offscreen anchor if it was already hidden. This avoids death-pose bounds
  pulling the bar down into the body. A new pointer/handle-ID pair or a
  mission/listener reset clears the cache; live projection failures still hide.
  Native enable/disable/alpha and latch duration are unchanged. The cache policy
  is isolated in `target_bar_fade.hpp` with standalone death/despawn/fade tests.

The bounds reads use these layouts:

| Data | Modtools | Steam / GOG |
|---|---|---|
| GameObject live collision centre mirror | `+0x18` | same |
| GameObject collision AABB, min/max XYZ | `+0x60` | same |
| GameObject simulation matrix / translation | `+0xF0` / `+0x120` | same |
| GameObject GameModel pointer | `+0x130` | same |
| GameModel primary RedModel pointer | `+0x20` | same |
| RedModel min/max XYZ (six floats) | `+0x98` | `+0x88` |

**HUD ownership:** this revision leaves the existing `.hud` file, bar sizes, scales,
unit-name/shield-name labels, and manual child offsets unchanged. It publishes
position only, not a size/scale event. The supplied SWBFIII derivation describes
rightward/upward artwork from the anchor; that alignment is a HUD-authoring choice,
not a DLL-imposed shift. There is deliberately no POI implementation.

Screen reservations are built in: left/top/right/bottom `0.10, 0.10, 0.107, 0`,
in viewport fractions. These retain the tested placement and allow for the
existing centred BF3 bar and labels; they do not define or resize the bar.
Label lengths/font metrics are not measured by the DLL.

Support is inherently on for supported builds, still opt-in by the HUD binding.
The enable toggle and inset INI settings have been removed; old entries are ignored.
Only `[Features] TargetBarLatchSeconds` remains configurable, with the same default
of 2.5 seconds and 0 meaning no timeout. Layout stays in the `.hud` file.

Model bounds remain authored, not exact animated-vertex bounds; rotating vehicles
can change their world AABB, and attachments outside it may need asset-specific
adjustments. There is no new per-object `poiYOffset` or centre-selection ODF flag:
the current healthbar path uses top-centre with zero world offset, while the
user's cosmetic offsets stay in the HUD.

**Lending.** `UpdateWeaponEvents` takes `weapon->mTarget` and falls back to
`controllable->mReticuleTarget[channel]`. While a latch is live and that slot is
empty, the DLL writes the latched handle into the slot immediately before
`HUD::GameEvents::Update` and restores it immediately after. The engine then sees a
target, never sends `targetDisable`, and keeps every `target.*` event flowing. All
show/hide decisions are expressed by lending or not lending - there is no disable
event of ours.

**State, per HUD tick, before the engine update:**

- a registered hit by the LOCAL player on an ENEMY (aim assist's `ApplyDamage` hook
  already recovers shooter, victim and handle id on all three builds) sets
  `latch = victim`, `expiry = now + hold`. Hitting someone else transfers it.
- natural target present and `!= latch` -> **cancel the latch.** Any different unit,
  friendlies included. The new unit is plain aim-only; nothing ever snaps back to a
  unit that may be off screen.
- natural target `== latch` -> `expiry = now + hold`. The hold therefore always
  measures time since the unit was last hit OR last under the reticle.
- no natural target -> lend the latch if it has not expired and line of sight holds.
  Line of sight lost: do not lend, but KEEP the latch, so the bar returns if the unit
  reappears inside the hold. Expired: clear it.
- after the engine update, read the engine's cached `WeaponData.target`: if it is
  neither empty nor the latch, the weapon's own lock picked someone else - cancel.
  One tick late, which is harmless because the engine showed that unit anyway.

`hold` defaults to 2.5 s, one INI value, 0 = no timeout. It is NOT a fade timer:
it bounds how long a hit keeps the bar attached. The fade belongs to the `.hud`.

**Position never stops for the last focus unit.** It is sent every tick for the
current target and, when there is none, for the most recent one while its handle is
valid; unprojectable/stale targets receive an offscreen position. When lending stops, the engine sends
`targetDisable` and the author's elements begin their own fade-out; if position
stopped at the same instant the bar would freeze in place and fade while the unit
walked away. One projection per tick, no second timer, no knowledge of the fade
length, and no ray on the tail.

Consequence for authors: bind position ONLY through `EventPosition`, never
`EventEnable` - the trailing updates would re-enable the bar mid-fade. Enable and
disable stay on the stock `target.*` events.

Accepted consequence: everything bound to `target.*` becomes sticky for the hold,
not just the health bar (a reticle tint bound to `target.teamColor`, for instance).

**The user confirmed the revised positioning works better in play.** That is not
confirmation across every build or an online session. The always-on revision passed
the standalone positioning tests and a C++ syntax check; no DLL build was run for
that revision. Every original hook address,
offset and convention below was derived per build and then independently re-read by
a second pass told to refute it (42 of 42 items confirmed), and the one write target
was checked from raw bytes on all three. None of that is a substitute for a match.

Two things the derivation changed from the design above:

- **The latch is taken in `Damageable::ApplyDamage`, not `Character::RegisterHit`.**
  On a multiplayer client `ApplyDamageCommon` diverts into the cosmetic
  `ApplyNetClientDamage`, so `RegisterHit` never runs there and the latch would
  silently never take online. `ApplyDamage` has no client early-out, and
  `DamageDesc+0x00` is the attacker's `Character*`, compared against
  `NetGame::GetLocalPlayer(0)`. Aim assist already detours the same function; both
  chain, and this module's prologue guard reads past a sibling's `JMP` (Detours pads
  the retail 6-byte prologue with `0xCC`, so it fingerprints bytes 8-11 instead).
- **No engine projection call.** `RedCamera` is laid out identically on every build
  (`_Matrix +0x30`, tan-half-FOV `+0x144`/`+0x148`) and `_MatrixInverse` is a rigid
  inverse, so camera space is three dot products and the projection is done in C.

It is **opt-in by data**: `EventClass+0x08` is a self-linked handler list when nobody
is bound, so with no `.hud` using `target.position` there is no lending, no sticky
target and no per-tick work at all.

| | modtools | Steam | GOG |
|---|---|---|---|
| `HUD::EventClass::Create` | `0x006AD8A0` | `0x0055DE40` | `0x0055EBC0` |
| `HUD::EventClass::FindByHashID` | `0x006AD940` | `0x0055DEE0` | `0x0055EC60` |
| `EventClass::sList` | `0x00AD866C` | `0x007EBA5C` | `0x007ECA2C` |
| `HUD::GameEvents::Open` | `0x006AEF00` | `0x0055E3A0` | `0x0055F120` |
| `HUD::GameEvents::Update` | `0x006B50A0` | `0x00562BE0` | `0x00563960` |
| `gPlayerData[0]` | `0x00BA3EA0` | `0x01EC6290` | `0x01EC7740` |
| `NetGame::GetLocalPlayer` | `0x006E3D20` | `0x005B7440` | `0x005B83F0` |
| `CameraManager::sInstance` | `0x00B70BD4` | `0x01E30324` | `0x01E317C4` |
| `GameObject::IsMyEnemy` | `0x0055F940` | `0x00535B30` | `0x005368A0` |
| `HUD::GameEvents::UpdateWeaponEvents` (not hooked) | `0x006B2EF0` | `0x00560AB0` | `0x00561830` |

**Conventions that differ by build** - each is part of the contract:

- `GameEvents::Update`: modtools `cdecl(float dt)`, pushed and never read. Steam and
  GOG: LTCG **dropped the parameter**; `void(void)`, nothing pushed, no `ADD ESP`.
- `EventClass::FindByHashID`: modtools `cdecl`, hash on the stack. Steam and GOG: hash
  in **ECX**, no stack arguments.
- `UpdateWeaponEvents` on Steam/GOG takes the camera in ECX and the `Character*` in
  EDX with eight stack arguments - the reason it is wrapped from outside via
  `Update` rather than detoured.

Same on all three: `EventClass::Create` cdecl varargs (it **never** checks for an
existing name - find first, or the second class is an orphan); `GameEvents::Open`
`void(void)`, and a hook at its return is still inside the `RunTimeHeap` window;
`GetLocalPlayer` cdecl(uint); `IsMyEnemy` thiscall `RET 4`; `ApplyDamage` thiscall,
five stack arguments, `RET 0x14`.

Layout identical on all three shipping builds, and **different from Phantom** where
marked: `Controllable::mReticuleTarget` **`+0x164`** + channel*8 (Phantom `+0x160`);
`mEyeDir +0xE8`, `mTargetInfo.mAimStart +0x148` (Phantom `+0xE4` / `+0x144`);
`Weapon::mTarget` `+0x128` modtools, `+0x104` Steam/GOG; `WeaponData` `0x28` bytes
with `target` at `+0x14`/`+0x18`; `PlayerData` `0xDC` (Phantom `0xD4`); `WeaponEvents`
`0x6C` with `reticule.position +0x60`, `lockOnPosition +0x64` (one slot more than
Phantom, `target.teamColorBright`); `Character` `mUnit +0x148`, `mVehicle +0x14C`,
`mRemote +0x150`; `GameObject` alive `+0x1FC` bit 3, handle id `+0x204`, `_bActive
+0xDC`, `mMatrix +0xF0`. Virtuals: `Trackable::GetGameObject` vptr at
`Controllable+0x18` slot `+0x20`; `GetTargetPoint` primary vptr slot `+0x50`, `RET
0x10`, safe body id `-1`; `GetSmoothedMatrix` slot `+0x110`.

Left for later: `GameObjectInterpolator::GetMatrix`, which the stock lock-on bracket
also applies when networking is live. Its convention differs (thiscall on modtools;
on Steam/GOG the instance is folded in and it is effectively `stdcall(id, out)`), and
`GetSmoothedMatrix` alone already carries the soldier's net smoothing, so it is only
worth adding if a marker is seen to jitter on a client.

---

## Camera horizon rotation event (2026-09-21)

`player1.reticule.horizonRotation` is a separate `type_Vector3` event containing
`(0, 0, zDegrees)`. It shares the existing `GameEvents::Open/Update` hooks in
`render/target_bar_latch.cpp` instead of detouring those guarded entry points
twice. Registration precedes `.hud` loading; publication follows the stock update
and is outside all target-bar/player-character gates. A rotation listener does
not enable the latch. Its event pointer and angle state are discarded on mission
open/list destruction; a changed/missing camera resets the angle as well.

### Native `ElementGroupBase::EventRotation` contract

Checked directly in the three local executables, not inferred from Phantom:

| Build | Rotation callback | Binding in instance constructor |
|---|---|---|
| Modtools | `0x0069A740` (ILT `0x0040215D`) | `0x0069AAA2`, handler at `self+0x144` |
| Steam | `0x0054F410` | `0x0054E474`, handler at `self+0x144` |
| GOG | `0x00550160` | `0x0054F1C4`, handler at `self+0x144` |

All three require event type **9**, dereference its vector payload, multiply each
component by pi/180, then build X/Y/Z rotation matrices with D3DX. Steam's import
slots `0x0076B564/548/55C` are `D3DXMatrixRotationX/Y/Z`; the degree multiplier at
`0x007B1EF0` is `0.01745329238474369`. Modtools uses the same multiplier at
`0x00A2E9E4`; GOG at `0x007B2E68`. No angle interpolation is applied, so equivalent
angles either side of +/-180 do not cause a full-turn animation.

The callback preserves the translation from the render element's matrix at
`self->mElement (+0xB0) +0x30` and recovers scale from the current basis lengths,
then installs `Rx * Ry * Rz * Scale`. Repeatedly doing this on a nonuniformly
scaled group changes those recovered lengths: the author must use a unit-scale
rotation pivot and put artwork sizing in a child. It also replaces, rather than
adds to, the pivot's authored rotation. The Steam `SetPosition` implementation
at `0x006C0800` copies position directly to matrix translation; HUD coordinates
are Y-down. A positive D3DX Z rotation sends HUD up `(0,-1)` toward positive X.

### Angle calculation and pole behaviour

Use the active rendered camera already read for target projection (manager
camera 0, matrix `+0x30`, tangent half-FOV `+0x144/+0x148`). For world-up `(0,1,0)`,
camera right/up dot products are `matrix[1]` and `matrix[5]`. The Z rotation is:

```text
atan2(right.y * screenWidth / tanHalfFovW,
      up.y    * screenHeight / tanHalfFovH) * 180/pi
```

This follows the projected world-up direction, not vehicle simulation roll and
not a bone. Translation has no effect. Equal pixel focal scales cancel, giving
the same bank angle at any normal resolution/FOV. The final result still depends
on the HUD author's parent transforms: do not put a nonuniformly scaled/rotated
ancestor above the pivot and expect an undistorted screen-space angle.

The squared length `right.y^2 + up.y^2` gates the undefined vertical case: below
`0.0001` retain the last valid angle and stay held until at least `0.0004`.
Starting at a pole uses zero. Invalid orientation/projection data resets to zero.
Crossing a pole can reverse projected world-up by 180 degrees; this is not a
continuous-flight-roll instrument. `render/hud_horizon_math.hpp` is independent
of game memory and tested in `tests/hud_horizon_tests.cpp`, including 20,000
random world-up segment projections. In-game behaviour still needs testing.

## Open questions

- Steam and GOG addresses are not derived. Anchors for porting: the strings
  `"HUD Element unable to find event %s"`, `"Too many events added to the HUD::EventQueue! Max is %d"`,
  `"objectivetimer"`, `"HUDEditHeap"`, and the `hud_` FourCC `0x5F647568` in
  `LoadUtil::ReadDataFileChunk`.
- Reader-key hashes `0xCA0B9CCB`, `0x0DD385E2`, `0x3455B0A8`, `0x6242102C`,
  `0xBDBFD84F`, `0x5D23BD7D`, `0x04ABB610` in `Element::ReadData` / `ViewPort::ReadData`
  are unresolved. The two `ViewPort` ones are almost certainly `Viewport1Enable` /
  `Viewport4Position`-shaped names; brute-force against the formula above.
- Why the editor cannot be navigated on retail, given that the code, the input
  mapping and the keyboard device are all present. See
  [It is still live on retail](#it-is-still-live-on-retail); needs a runtime check
  of `ConfigFile::GetFirst()->[0x0C]` at the moment `SetMode(2)` runs.
