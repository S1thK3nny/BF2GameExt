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

GameExt adds a fourth `FileInfo` line, `TrueWidescreen(1)`, which changes how a file is
laid out and drawn on a wide screen, not whether it loads; see
[Wide screens](#wide-screens-built-2026-10-05-renderhud_true_widescreencpp).

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

Item vtable slots used by the reader: `+0x04` `GetType`, `+0x08` `Read(scope)`,
`+0x20` `ReadData(config, data)`, `+0x24` `PostReadSetup()`.
`CreateItem` is instead slot `+0x04` of the separate **Factory** vtable.

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

Every property each class reads, events included, with its reader's address, is
catalogued in [HUDElementProperties.md](HUDElementProperties.md).

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

Load order, the stock Lua HUD calls, multiplayer and a design are in
[Lua and the HUD](#lua-and-the-hud-researched-2026-09-28).

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

### How it saves a file (read on modtools, 2026-10-07)

`HUD::Manager::Write` (M `006B88D0`) walks every item in `HUD::Item`'s list, in the
order they were made. Each `FileInfo` in `gConfigFiles` it reaches opens
`data\<its name>.hud`, and from then on every item whose write flag is on (+18 bit 0,
vtable +10) is written through `Item::Write` (vtable +0C, M `006B61D0`): the indent,
`Kind("name")` with the kind taken from the item's factory, `{`, the item's own
`WriteData` (vtable +28) one level in, and `}`. `HUD::Item`'s constructor turns the flag
on (M `006B6FDC`); `ViewPort::Read` turns it off for each item it reads (M `006BDDFB`),
and `ViewPort::WriteData` (M `006BD560`) writes those items itself, whatever their
flag. Bars, maps and multiline texts turn it off for the items they make for
themselves. The writers every `WriteData` uses are the indent writer (M `006B5A20`,
thiscall(count) on the file, RET 4) and the format writer (M `006B5A50`,
cdecl(file, format, ...)). A `WriteData` writes an item from its live state, and only
the properties its class reads.

GameExt's own lines go through the same writers:

- **`TrueWidescreen(1)`**: `ConfigFile::WriteData` (M `006B8090`) is followed by the line
  for a `FileInfo` that read it (`render/hud_true_widescreen.cpp`).
- **`TransformNumberMath`, `TransformNumberLerp`, `TransformNumberCompare`**: these are
  `TransformNumberVector3` items with GameExt's own factory, whose name gives the header.
  The stock `WriteData` would write `TransformNumberType`'s lines (`InputFactor`,
  `EventInputFactor` from the second input, `WrapInput`, then `EventInput` and
  `EventOutput`), which these kinds turn down, so the item vtable's +28 is GameExt's
  writer: each property the transform accepted, kept as it was read, in that order
  (`render/hud_number_math.cpp`). With the writers found the stock flag check stays;
  before, the flag check returned false, so the editor dropped a transform at the top of
  a file and wrote the stock lines for one inside a `ViewPort`.
- **`FillFrom`**: `ElementBarBitmap::WriteData` writes a bar from its live state, which
  `FillFrom` changed (below). Its detour hands the stock writer the bar as the stock setup
  left it, adds the line, and puts the bar back (`render/hud_bar_fill_from.cpp`). The
  editor's panel reads and changes a bar through its `GetProperty` (vtable +18, M
  `00695490`) and `SetProperty` (+14, M `006952F0`), thiscall(PblHash, value pointer) →
  bool, RET 8, which hand what they do not know (`BitmapRect`'s size, `TexCoords`, the
  alignments) on to the bitmap's. Both work on the live bar too, so on a `FillFrom` bar
  each is handed the bar as the file has it. A change is kept as the file's new state,
  with the bar's own `SetValue` held off it while the change is made (the `ScaleTexture`
  and `ScaleSize` branches call it), and `FillFrom` is laid out again from it, filled to
  the bar's value by GameExt itself: `SetValue` does nothing for an unchanged value
  (M `006960B5`).

`ElementBarBitmap::WriteData` (M `00695B10`, the bar vtable's +28, thiscall(PblFile*,
int), RET 8) writes `TexCoords` with U1 from `mBarU1` (+480), the `ScaleRect` and
`ScaleTexture` flags (+484), `FlashyScale` (+470) and the two fade times (+474, +478),
then widens the rectangle to `mBarWidth` (+47C) through `SetRect` (+4C) while the bitmap
base writes `BitmapRect`, and narrows it again. `FillFrom("Right")` stores the bar end
for end, with its own U span and the fade times swapped; the vertical modes clear both
flag bits and lay the rectangle out per value. So GameExt keeps each `FillFrom` bar's
rectangle, coordinates, width, U span, fade times and flags from before it changed
them, for this writer and the panel.

`tests/hud_number_math_abi_tests.py` and `tests/hud_bar_fill_from_abi_tests.py` check all
of these on modtools.

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

### The command post strip (built 2026-09-27, `render/hud_command_posts.cpp`)

The strip does not mimic the index-carrying pattern above: a generic `Element`
cannot demultiplex an index. It registers one event per
slot per field, `player1.commandPostN.{icon, iconDisable, color, capture,
captureColor, disable}` for N = 1..16 plus `player1.commandPosts.count`, and pumps
them from the `GameEvents::Update` detour, sending only on change. User-facing
details are in [HUD.md](../user/HUD.md#command-post-strip).

**The posts.** `CommandPost::sPostArray` (a `CommandPost*` array) and its count, both
behind static pointers (game_addrs `command_post_array_ptr`, `command_post_count_ptr`;
`ReadCommandPost` indexes the same array with a 4-bit net index). A `CommandPost` is
not a `GameObject`: its `mObject` handle (`+0x2C`) is the post's object, and the post's
team is that object's 4-bit team at `+0x234`, as Lua's `GetCommandPostTeam` and the
minimap's `ElementMap::UpdatePostIcons` (Phantom `0x00603470`) read it. The minimap
also hides a post whose object is dead (`+0x1FC` bit 3); so does the strip.

**`HUDIndex` and `HUDIndexDisplay`.** `HUDIndex` is `atoi`'d into `mHUDPostIndex`
(`+0x1C`), so blank is 0; it is the number `UpdatePostIcons` prints beside a post.
`HUDIndexDisplay` sets bit 1 of the post's flag byte (`+0x1A58` modtools, `+0xB40`
retail) and bit 0 of its class's (`+0x26C` / `+0x190`); the class default is on
(`CommandPostClass` ctor, Phantom `0x004DF95B`) and the post copies it. In the stock
ODFs it is 0 only for the invisible control zone and command vehicles (AT-TE,
gunships), so the strip uses it as its filter. Slots run numbered posts by number,
then the rest in map order.

**Capture.** `CommandPost::Update` (Phantom `0x004E48A0`) keeps two timers per post:
`mNeutralizeTimer` (`+0xA0`) runs up while another team holds an owned post, and
`mCaptureTimer` (`+0xA4`) runs for `mBiasTeam` (`+0x78`) on a neutral one, each against
the class's `NeutralizeTime` (`+0x04`) or `CaptureTime` (`+0x08`); `GetCaptureRatio`
and `GetNeutralizeRatio` divide them. The strip's `capture` is `1 - neutralize ratio`
for an owned post and the capture ratio for a neutral one, coloured by the owner or,
once a capture has started, by `mBiasTeam`. The timers only change while someone
holds the post, so a half-taken post stays half taken. At neutralisation the code
sets `mNeutralizeTimer` to the full time and nothing seen resets it at capture; the
new owners holding the post count it back down (`MSG_SAVED` at 0). So a post fresh
from neutral may show low until its owners secure it: read from the code, not yet
seen in play.

**Online.** Team changes are host-only and reach clients as `ChangeCommandPostTeams`
events, so ownership is right everywhere. The timers are not sent. A client runs
`CommandPost::Update` too, but only scans a post's capture region when `netOnClient`
is clear or `NetGame::IsNearLocalPlayer` says the post is within 100 units on X and Z
of `netLocalPos` (Phantom `0x00696FB0`; Steam `0x005B7470`, GOG `0x005B8420`,
modtools `0x006E3DD0`). The per-player capture bar is sent separately
(`WriteCaptureDisplay`, a 6-bit ratio), which is why the stock HUD's
`commandPost.charge` works on clients. The strip makes the same test and shows a post
at rest (1 owned, 0 neutral) where a client does not simulate it.

**Icons and colours.** `SetTeamIcon(team, icon, conquestIcon, ctfIcon)` hashes into
`Team::mIcon` (`+0x1C`), `mConquestIcon` and `mCTFIcon`, with no bounds check;
`Team::sTeams` (game_addrs `team_array_base`, a pointer to the array) has a neutral
team at 0 that the engine dereferences for neutral posts, so `SetTeamIcon(0, ...)` is
safe. The strip sends `mIcon` after checking the texture table. Colours are the
viewer's `Team::mColor[team]` (`+0x68`, `RedColor[8]`), the minimap's palette, with
the viewer from the local `Character::mTeamNumber` (`+0x134`). A Color event (type 7)
carries a pointer to the 4 bytes: HUD::Event's `RedColor` constructor stores the
pointer and every `EventColor` handler dereferences it (modtools `0x00692B80`, Steam
`0x0054A080`, GOG `0x0054ADD0`).

| | modtools | Steam | GOG |
|---|---|---|---|
| `CommandPost::sPostArray` pointer / count pointer | `0x00AD5498` / `0x00AD549C` | `0x007E6314` / `0x007E631C` | `0x007E7314` / `0x007E731C` |
| `CommandPost::mClass` | `+0x1A54` | `+0xB3C` | `+0xB3C` |
| `Team::sTeams` pointer | `0x00AD5D64` | `0x007E9AA0` | `0x007EAAA0` |
| `netOnClient` | `0x00BE14FD` | `0x01E62EAB` | `0x01E6435B` |
| `NetGame::IsNearLocalPlayer`, cdecl(`PblVector3*`) -> bool | `0x006E3DD0` | `0x005B7470` | `0x005B8420` |

Every read site is listed in `game/Battlefront2/Source/CommandPost.h` and checked on all three
executables by `tests/hud_command_posts_abi_tests.py`.

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

### The floating target bar (selection retention revised 2026-09-22, `render/target_bar_latch.cpp`)

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
- **Up close/offscreen:** while the engine picks the target, clamp the projected
  anchor to a built-in safe area, so a big vehicle up close keeps its bar. Once
  the pick is lost (the hold, the fade, a death fade) the anchor is not clamped
  and the bar leaves the screen with its target, held one viewport past each
  edge to keep near-plane projections finite (2026-09-26; before that it was
  always clamped, which parked a held bar at the screen edge until its target
  went wholly behind the camera). A small positive depth floor avoids division
  by zero/flips when the anchor crosses the eye plane but some of the bounds
  remain in front. Wholly behind-camera bounds and invalid data move the
  position offscreen.
- **Pixel alignment:** snap the anchor to framebuffer pixels using live screen
  dimensions, with the safe-area limits rounded inward. This does not rewrite
  bitmap sizes/child offsets, so it does not guarantee every child edge is an
  integer pixel when the HUD author uses fractional dimensions. Skipped while
  `HudSubPixel` is on; see [Pixel snapping](#pixel-snapping-built-2026-09-29).
- **Death fade:** `tick_after` reads bounds only from a valid, living focus. Death
  or an expired handle retains that channel's last live world-bounds snapshot,
  fixing its top-centre anchor in the world instead of on the screen. Every tick
  reprojects the snapshot through the current camera and reapplies pinning/pixel
  snapping. The retained box also preserves the close-up camera-plane policy.
  No corpse bounds or stale object are needed. Wholly behind-camera snapshots
  hide, but can project again if the camera returns during the native fade. A
  new pointer/handle-ID pair, invalid live bounds or mission/listener reset
  clears the cache. Native enable/disable/alpha and latch duration are unchanged.
  The cache policy is isolated in `target_bar_fade.hpp` with standalone tests.

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
Only `[Features] TargetBarLatchSeconds` remains configurable: its selection-hold
default is now 0.5 seconds; 0 preserves the explicit no-timeout override. Existing
INI values still apply. Layout and fade duration stay in the `.hud` file.

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

**Selection retention (2026-09-22):** the user reported the previous hit latch did
not work in the current build and requested reference-style selection retention.
The healthbar module no longer hooks `Damageable::ApplyDamage`, depends on
`IsMyEnemy`, or runs an extra LOS ray. Aim assist's independent damage hook is
untouched. This does not copy the reference game's hybrid autoaim producer.

Per channel, before lending, call the same Controllable primary-vtable methods as
native `UpdateWeaponEvents`: slot `+0x3C` with channel -> index; if nonnegative,
slot `+0x40` with index -> Weapon*. Both are thiscall with one stack argument.
Read `Weapon::mTarget` (`+0x128` modtools, `+0x104` Steam/GOG), handle-check it,
then fall back to the untouched reticule slot. A missing weapon clears retention.

Fresh raw-PE checks of these reads/calls:

| Build | Weapon-index / weapon calls | Weapon target read | Reticule fallback |
|---|---|---|---|
| modtools | `006B2F0A` / `006B2F18` | `006B3609` | `006B3630` |
| Steam | `00560ADE-00560AF1` | `00561025` | `0056104F` |
| GOG | `0056185E-00561871` | `00561DA5` | `00561DCF` |

After the native update, acquire/refresh only if the natural handle matches the
engine's accepted cached target, including the generation ID, and is alive with
finite positive current health (`GameObject+0x144`). This leaves class/target
eligibility filtering to the engine. A HUD cache by itself never acquires a target.

- Natural selected target: default deadline becomes `now + 0.5`.
- Different natural target: stop retaining the previous target immediately, even
  if the new target is later rejected; never snap back.
- No natural target: lend the retained valid target while before the deadline.
  Native acceptance of that loan never refreshes the deadline.
- No new LOS, distance or projection gate is imposed on retention. Projection
  still determines the visible position independently.
- Dead/stale/native-rejected targets clear retention. Each weapon channel is
  independent and only participates while its position event has a listener.
- Changing the local controlled object/handle, weapon, mission or listener clears
  the relevant state. Zero hold preserves the old explicit unlimited override;
  negative/nonfinite durations fall back to 0.5.

`target_bar_selection.hpp` isolates this state machine for standalone tests,
including borrowed-target feedback, generation reuse, replacement/reacquisition,
channel isolation and 15-360 Hz expiry checks. Read-only PE regression checks
cover the input offsets and virtual call sites on all three supported binaries.

The hold is NOT a fade timer. Expiry lets native `target.disable` fire; authors
can use `FadeInTime(0)` and `FadeOutTime(0.25)` on the bar/glow/text elements for
reference-style visual timing. The inspected BF3 `legacy_gameext_hud.hud` still
uses 0.65 on nine target elements; it has not been edited by this revision.

Scope: this ports selection retention, not all reference visibility rules.
The world-space death fade above is unchanged. The requested unpinned positioning
with its active-aim exception was built on 2026-09-26 (see Up close/offscreen
above); immediate zero-health hiding is not.

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

**Why a vehicle and its rider flicker (read 2026-09-26).** `PlayerController::Update`
(Phantom `0x0071CAA0`) re-picks each weapon channel's target on every tick. It
collects objects near the aim ray (`CollisionManager::GetObjectsInRange`, up to 300),
skips dead, invisible, cloaked-enemy and opted-out classes, and scores the rest with
`TargetInfo::GetTargetPrio` (`0x007737C0`):

```c
along = dot(dir, centre - aimStart);              // must lie in 0..radius + range
perp2 = |centre - aimStart - dir * along|^2;
prio  = perp2 <= (along * angle + radius)^2 ? 16 * perp2 + along : -1;
```

`centre` and `radius` come from the model's total bounding box. The lowest score wins
if a ray to its target point is clear (`CollisionManager::RayTest`). The current
target is re-scored with the same formula and only wins ties; a locked lock-on weapon
skips the search. So nothing damps a change: an exposed rider's centre sits just
above its vehicle's, and with the crosshair between them the two trade the pick as
the aim, the vehicle and the rider's animation move. Stock BF2 shows it too, as the
name in the fixed target box flipping.

The engine already knows who rides what. The same loop skips a candidate riding the
player's own vehicle through `GameObject::GetControllable` (primary vptr `+0x6C`),
`Controllable::mCharacter`, `Character::mVehicle` (`+0x14C`) and that vehicle's
`Trackable` (`+0x18`) `GetGameObject` (`+0x20`):

| | modtools | Steam | GOG |
|---|---|---|---|
| `Controllable::mCharacter` (Phantom `+0xC8`) | `+0xCC` | `+0xCC` | `+0xCC` |
| The chain, where read | `0x004CE784`, swapping a rider for its vehicle | `0x0061B863`, `PlayerController::Update` | `0x0061C8D3` |

**The rider pair (built 2026-09-26).** Each channel's display group is the target
itself, or a mounted rider's vehicle. When the pick moves to the other member of the
group being shown, the bar stays on the shown member until the other has been picked
on every tick for 0.3 seconds; a tick without a pick starts that count again. A pick
from any other group replaces it at once, as before. To show the kept member, the DLL
writes it over the engine's pick during `HUD::GameEvents::Update`, in whichever handle
the pick was read from (`Weapon::mTarget`, else `mReticuleTarget[channel]`), and
restores it afterwards, as the hold does. Only `UpdateWeaponEvents` reads
`Weapon::mTarget` in the HUD update, and a charging launcher's lock-on marker reads
the launcher's own `mCurTarget` and `mCurTargetBodyID` through `GetLocked` and
`GetLockedTargetBodyId` (Phantom `0x007C05A0`, `0x007C05E0`), so aim assist, lock-on
and firing never see the swap. If the HUD refuses the kept member, the pair is dropped
and the engine's pick shows from the next tick. The state machine is
`target_bar_selection::Pair`, tested in `tests/target_bar_selection_tests.cpp`; the
chain is audited on all three builds by `tests/target_bar_selection_abi_tests.py`.
The user confirmed it in play on 2026-09-27, together with the unpinned positioning.

**The user confirmed the revised positioning works better in play.** That is not
confirmation across every build or an online session. The always-on revision passed
the standalone positioning tests and a C++ syntax check; no DLL build was run for
that revision. Every original hook address,
offset and convention below was derived per build and then independently re-read by
a second pass told to refute it (42 of 42 items confirmed), and the one write target
was checked from raw bytes on all three. None of that is a substitute for a match.

**No engine projection call.** `RedCamera` is laid out identically on every build
(`_Matrix +0x30`, tan-half-FOV `+0x144`/`+0x148`) and `_MatrixInverse` is a rigid
inverse, so camera space is three dot products and projection is done in C.

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
`GetLocalPlayer` cdecl(uint). The two Controllable weapon getters used for natural
selection are thiscall with a single integer argument and callee stack cleanup.

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
0x10`, safe body id `-1`; `GetSmoothedMatrix` slot `+0x110`; `GetControllable` slot
`+0x6C`, no arguments, with `Controllable::mCharacter` at `+0xCC` (Phantom `+0xC8`).

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

## TransformNumberMath native adapter (2026-09-21)

The new factory is registered through the existing GameEvents::Open hook, after
stock events exist but before HUD assets load. Manager::Open still has the runtime
heap selected. The math module adds **no detour** and does not modify a stock
factory, vtable or transform instance. Resolve runs only after the shared hooks
successfully install; a math fingerprint failure leaves their existing features
available. This currently shares their installer dependency, not the healthbar's
listener-dependent per-frame work.

Phantom PDB evidence: Item::Factory ctor `00617070`, Manager::Open `00619970`,
Transform ctor/dtor `0061C310/0061C360`, Vector3 ctor `0061E1A0`, Item::Read
`00617D10`, ReadEvent `00617E30`. Factory::CreateItem (`00409845` thunk) constructs
a 96-byte Vector3 transform then calls Item::SetFactory with **its own factory**.

Factory is 28 bytes: vptr +0, property-list terminator +4, hash +8, double-list
node +C, flags +14, static type-name pointer +18. Its vtable has just deleting
destructor and CreateItem. The adapter allocates through native New<Factory>
(Modtools) or native operator new(28) (retail), calls the native base constructor,
then installs a private copy of the Vector3 factory vtable with CreateItem replaced.

Each new item delegates construction to the stock Vector3 factory method using
the **new** factory as `this`, preserving native allocation, naming and list
membership. It reuses the numeric handlers at +1C and +40, replaces callbacks,
and uses a DLL-owned sidecar for its math state. The instance's private vtable
overrides Read (native Item::Read), ReadData, PostReadSetup and destructor.
WriteEnabled is false: this initial source-authored implementation intentionally
does not claim native editor round-trip support. All stock instances retain their
original vtables. Mapping +58/count +5C stay zero; no mapping memory is allocated.

This allocation distinction is essential: Phantom Transform::DestroyAll
(`0041A659` thunk) invokes virtual dtor(0), **then** Delete<Transform>. Factory
DestroyAll (`0041BE7D`) does the same. A DLL-allocated engine item would therefore
be freed by the wrong heap. The wrapper deletes only its sidecar; the native
Vector3 destructor unregisters both handlers, removes list membership and follows
the engine's normal free path. No sidecar/event pointer survives mission cleanup.

| Adapter address | Modtools | Steam | GOG |
|---|---|---|---|
| Factory allocation | `006B7770` | `006C3540` | `006C45D0` |
| Item::Factory ctor | `006B6970` | `00564110` | `00564E90` |
| Vector3 Factory vtable | `00A60344` | `007A32E4` | `007A40AC` |
| Vector3 instance vtable | `00A61154` | `007A371C` | `007A455C` |
| Item::Read | `006B6B80` | `00564560` | `005652E0` |
| Item::ReadEvent | `006B6360` | `00564740` | `005654C0` |
| Item::GetFilteredEventName | `006B6270` | `00564690` | `00565410` |
| Vector3 Factory::CreateItem (resolved) | `006B7D50` | `00564BA0` | `00565920` |
| Vector3 deleting dtor (resolved) | `006BD170` | `005686A0` | `00569420` |

All addresses were read in the actual three shipping PEs, not ported by a global
offset. Factory ctor is thiscall(hash), CreateItem is thiscall(name, callback,
argument), deleting dtor is thiscall(flags). Instance virtual methods retain
their thiscall ABI. The DLL's wrappers use fastcall with unused EDX for those
methods. ReadEvent is **cdecl(data, handler)** on Modtools, **fastcall(data ECX,
handler EDX)** on retail. Filter is cdecl(name, buffer, length) on Modtools;
retail takes name ECX/buffer EDX and hardcodes length 511. Factory allocation is
cdecl(bool useEditHeap) on Modtools but cdecl(unsigned bytes) on retail.

PblConfig::Data is hash/count plus 128 DWORD args; float arguments are raw float
bits and strings are offsets relative to args. The new parser bounds-checks
string offsets/terminators, counts and finite numeric arguments before native
binding. Output creation explicitly finds first, rejects any collision, and uses
`Create(Float, "%s", filteredName)` so authored names never become format strings.

Initial publication is delayed until the first shared GameEvents update. Incoming
events cache both operands even before activation. Afterwards they publish
synchronously, only when the Float result changes. Pending flags avoid idle
recalculation. Both slots fed by one event update atomically before calculation.
Per-node busy and global depth guards stop synchronous feedback. The adapter has
no EventQueue slots, thread, timer, Lua dependency or runtime-created element.

Tests: `tests/hud_number_math_tests.cpp` compiles the actual adapter with native
callbacks mocked for both ABIs, exercises malformed configuration, allocation
failure, chains and repeated teardown. `tests/hud_number_math_abi_tests.py` reads
the production installer guards and address table and compares them with all
three PEs (including Modtools vtable JMP thunks). Actual game execution is still
required to validate loading/rendering; see the [HUD testing checklist](HUDAuthoring.md#testing-and-verification).

### TransformNumberLerp

Built 2026-09-28 on the same adapter: a second factory, `TransformNumberLerp`, with
its own copy of the Vector3 factory vtable whose CreateItem marks the sidecar as a
lerp. Everything else (shell, vtable overrides, activation, publication, teardown)
is shared with Math.

A lerp has three event inputs (the 0..1 input, A and B) and the shell has two
handlers: +1C carries `EventInput` and +40 `EventInputA`. `EventInputB` binds a
20-byte `EventHandler` inside the sidecar through the same `Item::ReadEvent`, with
the same callback and context. `ReadEvent` registers any handler the caller passes;
it does not care where the handler lives.

The native destructor unregisters only +1C and +40, so the wrapper unlinks the third
handler first. `EventClass::UnregisterEventHandler` (Phantom `0060F870`) and
`EventHandler`'s destructor (Modtools `006AD6B0`, Steam `0055DB70`, GOG `0055E8F0`,
called by TransformNumber's destructor on +40) are the same unlink: node `{next,
prev}` at +08, `next->prev = prev` at `next + 4`, `prev->next = next`, both only when
next and prev are set. The destructors do not clear `mClass` (+10); the wrapper
does. `tests/hud_number_math_abi_tests.py` checks those instruction bytes on all three
builds, so a build with a different handler layout fails the audit.

`InputRange` ends that name events bind two more sidecar handlers the same way.
`Item::ReadEvent` (Phantom `00617E30`) reads only argument 0: it checks that the
argument is a string offset, filters and hashes that name, finds the class and
registers the handler. So the reader hands it a one-argument copy of the property
holding just that end's name, and the native lookup, filtering and warnings all
apply unchanged.

`TransformNumberCompare`, built 2026-09-28, is a third factory on the same
adapter. Its optional `EventOutputTrue` and `EventOutputFalse` are Bool classes
(type 1) created like the Float output, sent with data 1 only when the result
changes, and never repeated by `OutputIsAlpha`: `EventEnable` restarts an
element's fade each time it fires.

Time comes from `QueryPerformanceCounter` between `hud_number_math_update` calls,
capped at 0.1 s. Modtools passes `UpdateFloat` a dt, but Steam and GOG dropped it
(`UpdateVoid`), so every build measures its own for the same behaviour.

### `EventAlpha` lasts one frame (read on modtools, 2026-09-28)

`EventAlpha` is a group property only: the one reader that takes its hash
(`0x41B355C1`) outside ColorBlend is the group base's `ReadData` (modtools
`00699FA0`), which binds it to a handler at +130. Phantom has no group alpha;
its `ElementGroupBase_data` puts `mEventRotation` at +130, which modtools moved
to +144. The callback (modtools `0069A6E0`) takes a Float, multiplies it by 255
and stores the byte straight into the group's `RedInterfaceElement` colour
alpha, `mElement` (+B0) → `m_color` (+2C) byte +2F.

`Element::Update` (modtools `006920F0`, called for every enabled element each
frame) eases `m_color` toward the element's own colour and then always writes
that alpha byte as `fader × mAlpha × 255`, `mAlpha` being the authored `Alpha`
(+AC). So an `EventAlpha` value survives only until the next update. For the
stock reticule fade to hold, `player1.reticule.alpha` must be re-sent every frame;
its sender has not been traced. A GameExt transform re-sends only with
`OutputIsAlpha(1)`, which `hud_number_math_update` honours after the element
updates of that frame, because GameEvents::Update runs later. Without it, a
lerp's fade shows while its value is still moving and snaps back once it settles.

`PropagateAlpha` (`0x09B602B0`, a stock key the stock HUDs use throughout, already
read by Phantom's group reader `005FC510`) sets bit 0 of the group's render
element at +84. The per-element render (modtools `00816FA0`) passes its colour into
the element's render, modulated channel by channel with the colour it is handed
(`a × b / 255`, `004D0ED0`). With the flag, a group's alpha reaches the elements
inside it. Without it, a group still tints its children's RGB. Every property of
every element class is in [HUDElementProperties.md](HUDElementProperties.md).

### ConfigMunge and trailing comments (tested 2026-09-28)

`ConfigMunge` drops a whole-line `//` comment but keeps one that follows a
property on the same line: every token after the `)` becomes one more argument.
Unquoted words are hashed like names, numbers stay floats, and quoted text stays a
string. `RiseTime(0.15) // fade out` arrives as `0.15, hash("//"), hash("fade"),
hash("out")`. The `//` token is always `0xA2D266E3`, even straight after the `)`,
so `Data::arguments()` ends the list there. `//fade`, with no space, is a single
hashed word and cannot be told from an argument. Native readers read their fixed
count and ignore the rest, which is why stock files never noticed.

Unquoted `true` and `false` are hashed too (`0x4DB211E5`, `0x0B069958`, any
case), and `PblConfig::Data::GetBoolArg` (Phantom `005F3570`) only tests the
argument against `0.0`, so a stock flag written `false` reads as on.
`Data::flag()` recognises both words.

## Unit and weapon state events (2026-09-28)

`player1.unit.state.*` and `player1.weaponN.state.*` are Float flags published by
`render/hud_class_icons.cpp` beside the icon events, from the same reads plus:

- **Soldier `mState`** at `Controllable + g_soldier->mState`, the value the stance
  already reads (SoldierState: 3 SPRINT, 4 JUMP, 5 ROLL, 6 JET_JUMP, 7 JET_HOVER,
  8 FALL, 9 to 13 the thrown and knocked-down states, 19 SLIDE).
  `EntitySoldier::EnterControllable` (Phantom `0056DA20`) deactivates the soldier and
  sets the Character's vehicle without touching `mState`, so the flags are read
  only while `Character::mVehicle` (+14C) and `mRemote` (+150) are both empty.
- **`Weapon::mState`** at +B0 on every build (WeaponState: 1 FIRE, 2 FIRE2, 3 CHARGE,
  4 RELOAD, 5 OVERHEAT). Melee weapons reuse FIRE for an attack, RELOAD for a block
  and OVERHEAT for the recovery after a swing, so a weapon whose `IsMelee` (primary
  vtable slot 21, as `controller/aim_assist.cpp` calls it) is true maps RELOAD to
  blocking and reports no reload, charge or overheat.
- **`Weapon::mLastFireTime`**, stored by `SignalFire`: Modtools +11C
  (`0061C8EF: 89 8E 1C 01 00 00`), Steam +F8 (`006796A5: F3 0F 11 8E F8 00 00 00`),
  GOG +F8 (`0067A745`, same bytes). A change on the same weapon is a shot; a change
  of weapon is not. A melee swing also passes through `SignalFire`.

`tests/hud_class_icons_abi_tests.py` checks the store sites, the `+B0` constant and
the shared IsMelee slot on all three builds.

## Bitmap sizing: `BitmapRect` (read on Phantom, 2026-09-26)

A bitmap's size comes only from `BitmapRect`, never from its texture.
`HUD::ElementBitmapBase::ReadData` (`0x005FA670`, hash `0x625FA794`) reads
`BitmapRect(width, height, hAlign, vAlign, mode)`:

- `hAlign` is `Left`, `Center` or `Right` (`sBitmapHAlignStrings`), `vAlign` is `Top`,
  `Center` or `Bottom` (`sBitmapVAlignStrings`); either defaults to the first.
- `mode` is `Pixels`, `Screen` or `Viewport` (`gRelativeModeStrings`) and defaults to
  `Viewport`. A bare numeric `0` in that slot means `Screen`.
- It calls the bitmap's `SetRect(float width, float height, int hAlign, int vAlign)`
  (vtable `+0x44` on Phantom), which turns the size into a quad around the element's
  position: `Left` spans `0..w`, `Center` `-w/2..w/2`, `Right` `-w..0`, and the same
  for `Top`, `Center`, `Bottom` on `h`. It then stores the alignment and mode in the
  element (`+0x119`, `+0x11A`, `+0x118`).

After the file is read, `ElementBitmapBase::PostReadSetup` (`0x005FA3B0`) converts the
quad's corners to pixels once with `HUD::ConvertRelativeToPixels` (`0x005F3090`):
`Pixels` keeps the number, `Screen` multiplies by the full screen size, `Viewport` by
the element's container view size. X is always scaled by a width and Y by a height,
so a `Viewport` rectangle's shape follows the viewport's shape rather than staying
square. Group `Scale` then applies on top.

The texture is drawn across the whole quad, using the `TexCoords(u0, v0, u1, v1)`
sub-rectangle (`0x1FFAD2E0`) if one is set. Its own pixel size and aspect play no
part, and `EventBitmap` only changes which texture is drawn, so a texture of a
different shape is stretched to the same box. `RedBitmapElement::SnapToTexture`
(`0x008C9260`) would size a zero-width or zero-height axis from the texture's pixel
size, but nothing calls it.

On a `BarBitmap` the value also moves the quad's right edge: `ElementBarBitmap::SetValue`
(`0x005F7920`) sets `right = left + value * mBarWidth`. `ScaleTexture(bool)` (`0x79B5F340`,
bit 0 of `+0x23C`, on by default) makes the fill crop the texture's U range to match,
so the fill reveals the picture; off, the whole texture is squeezed into the shrinking
quad. A second flag (bit 1, hash `0xF5B1A000`, also on by default) enables the edge
movement itself.

**The fill's U range is wrong unless `TexCoords` starts at `u0 = 0`.**
`ElementBarBitmap::PostReadSetup` (`0x005F7480`) stores `mBarWidth = right - left` but
`mBarU1 = u1`, the right-hand U itself, and `SetValue` then sets
`u1 = u0 + value * mBarU1`. It should be the span `u1 - u0`. So `TexCoords(0.25, 0,
0.75, 1)` shows U 0.25 to 1.0 at full value, and a horizontally flipped
`TexCoords(1, 0, 0, 1)` gets `mBarU1 = 0` and collapses to one stretched column.
Flipping V is safe; the fill never touches it. Every `BarBitmap` with `TexCoords` in
the stock and `data_BF3` HUDs uses `u0 = 0`, only V varies, so storing the span
instead would change none of them. With the span, a bar rotated 180 degrees with both
axes flipped fills from its other end with the picture upright.

`BarBitmap` hands every other property, `BitmapRect`, `EventBitmap`, `TexCoords` and
`BitmapStyle` (`0xA28F0EEB`, `Normal` or `Shadow`) included, to
`ElementBitmapBase::ReadData`.

### `FillFrom("Right")` (built 2026-09-26, `render/hud_bar_fill_from.cpp`)

Rather than change the fill, the bar is stored end for end once it has loaded, and
the unchanged stock `SetValue` then anchors on the right. At the end of
`ElementBarBitmap::PostReadSetup` the rectangle becomes `(right, top, left, bottom)`,
the coordinates `(u1, v0, u0, v1)`, `mBarWidth = left - right` and
`mBarU1 = u0 - u1`, so the fill gives `right' = R - value * W` and
`u1' = u1 - value * (u1 - u0)`. Each point keeps the full bar's pixel, the span is
right for any `u0`, and the flash strip, computed from the same edges, lands on the
moving edge. The fill judges growth by which way that edge moved, now reversed, so
the two fade times are swapped at the same moment. The user confirmed it in play the
same day, with a health silhouette and its missing-health complement.

The quad is then drawn right to left, which needs culling off. It is:
`pcInterfaceShader::Begin_Fixed`/`Begin_Prog1` pass `PCREDCULL_NONE` (0) to
`RedRenderer::pcSetCullMode` (Phantom `0x008F29D4`, `0x008F2AB4`; modtools
`0x00870284` calling `0x0080B940`), and the stock flash already draws such a quad
whenever a bar grows.

`EventBitmap` leaves the reversal alone. `RedBitmapElement::SetTexture(uint)` (Phantom
`0x008C8EE0`, modtools `0x008391E0`) only stores the texture hash in the element's
shader, after `Init` (vtable `+0x40`, modtools `+0x44`) creates the shader if there is
none, and `Init` (Phantom `0x008C8C50`) only creates it. The rectangle and coordinates
(modtools `+0x78` and `+0x94`) are not touched, so class and stance icons can drive a
`FillFrom("Right")` bar.

The `.hud` property reaches the element through `HUD::Item::Read` (modtools
`0x006B6B80`), which calls `ReadData` (vtable `+0x20`) per property and logs
`Error reading parameter 0x%08x` when it returns false, then calls `PostReadSetup`
(`+0x24`) once all are read. So the hooked `ReadData` records `FillFrom("Right")` and
`PostReadSetup` consumes it in the same pass, and a game without GameExt only logs
the line.

| | modtools | Steam | GOG |
|---|---|---|---|
| `ElementBarBitmap::ReadData`, thiscall `(PblConfig*, Data*)`, RET 8 | `0x00695900` | `0x0054B480` | `0x0054C1D0` |
| `ElementBarBitmap::PostReadSetup`, thiscall, RET 0 | `0x00696340` | `0x0054B320` | `0x0054C070` |
| `ElementBarBitmap::SetValue`, thiscall(float) -> float, RET 4 (hooked for the vertical modes) | `0x00696090` | `0x0054B070` | `0x0054BDC0` |
| `RedBitmapElement::GetRect`, RET 0x10 | `0x00838E50` | `0x006E4DB0` | `0x006E5E50` |
| `RedBitmapElement::GetTexCoords`, RET 0x10 | `0x008392A0` | `0x006E48F0` | `0x006E5990` |
| `RedBitmapElement::SetTexCoords(u0, v0, u1, v1, bool)`, RET 0x14 | `0x00839220` | `0x006E4B90` | `0x006E5C30` |

On all three: the bitmap pointer `+0xB0`, `mBarWidth +0x47C`, `mBarU1 +0x480`, the
flag byte `+0x484`, `FlashyScale +0x470`, `FlashyIncFadeOutTime +0x474`,
`FlashyDecFadeOutTime +0x478`, the flash bitmap `+0x370`, and `SetRect(l, t, r, b)` at
the bitmap's vtable `+0x4C` (Phantom `+0x48`). `SetTexCoords`' bool rotates the
coordinates; stock code always passes false. Checked on all three executables by
`tests/hud_bar_fill_from_abi_tests.py`.

### `FillFrom("Bottom")` and `FillFrom("Top")` (built 2026-09-27)

The stock fill cannot go vertical: `ElementBarBitmap::SetValue` only ever moves the
right edge and crops U. Rotating the bar turns its picture too, and the one thing that
could counter-rotate the texture, `SetTexCoords`' rotate bool, is reset to false by
every `SetValue`. So a vertical bar is laid out by GameExt instead. At the end of
`PostReadSetup` its full rectangle and coordinates are recorded and flag bits 0
(`ScaleTexture`) and 1 (edge movement) are cleared. With both clear, `SetValue` still
stores the value but changes nothing visible: it rewrites the coordinates it just read,
never touches the rectangle, and never starts the flash, which is gated on bit 1. A
detour on `SetValue` then runs it and lays the bar out for the value it returned: the
anchored edge stays, the other moves, and V is cropped to match (or not, if
`ScaleTexture` was authored off).

`SetValue` is `thiscall(float) -> float` in ST0, `RET 4`, and is reached with the bar's
`ElementBar` base, bar `+0x220`, as `this`: its bitmap read is `[this-0x170]` (bar
`+0xB0`), its flags `[this+0x264]` (bar `+0x484`) and `mValue` `[this+0x1C]`. Modtools
`0x00696090`, Steam `0x0054B070`, GOG `0x0054BDC0`, prologues and sites checked by the
same audit. The recorded bars are forgotten when `GameEvents::Open` runs for a new HUD;
a record left by a freed bar is inert, because only a bar with both flag bits clear is
laid out and a stock bar keeps bit 1 on by default.

## World markers and distances (researched 2026-09-28)

Addresses are Phantom (P) unless a build is named: M modtools, S Steam, G GOG.
Nothing here is built.

### The stock `Target` element already draws 3D markers

`Target("player1target")` is in the stock `1playerhud.hud` (line 1491; `PC\` variant
line 1389). It draws the GameObjects `LockOnManager` tracks, with off-screen edge
arrows:

- **Common targets**, a global `sTargetCommon[32]` (M `00B306C8`, S `01EC7D50`,
  G `01EC91F0`): script and objective markers and flags. Only
  `TargetManager::AddMarker` (P `00773CA0`) adds them, through
  `LockOnManager::AddTargetCommon` (P `0063A190`; M `00457360` cdecl; S `0057DA40`
  fastcall, object in ECX and team mask in EDX; G `0057E7C0`), and only for an
  **object** marker whose `showHudTarget` is set. Region markers never become 3D
  targets. `AddMarker` is called by Lua `MapAddEntityMarker` (P `00655B50`), class
  markers (`CheckObjectClassMarker`, P `007741D0`), replication
  (`AloMapDisplayMarkers::Read`, P `0069ABE0`) and `TeamAssist`, which passes
  `showHudTarget` false.
- **Player targets**, `mTargetPlayer[64]` at `LockOnManager` +8: lock-on, attacker and
  hint targets. `LockOnManager::sLockOnManagers[player]` is game_addrs
  `lockon_mgr_array` (M `00B30698`, S `01E57400`, G `01E588B0`).
- **Ranges.** Common targets: track 10000, min 100, max 10000 (`InitCommon`, P
  `0063A870`, M `004546D0`, S `0057ACB0`). Player targets take the class's lock-on
  min, max and track distances (+31C, +320, +324; defaults 100, 500, 200000;
  `InitPlayer`, P `0063A8C0`).

Each frame `LockOnManager::Update` (P `0063D0B0`, M `00457670`, S `0057B030`,
G `0057BDB0`) calls `UpdateTargetVisibility` (P `0063D4D0`, M `00454B60`, S
`0057B720`, G `0057C4A0`) per target:

- **Anchor:** the collision-sphere centre; a soldier adds up × 0.4, or uses bone
  `0x3483A489` if the model has it. Common targets are lifted by up × 1.3 m (the
  `1.3f` stores at M `00454CD7`, S `0057B8AA`, G `0057C62A`; checked on M and S).
- **Distance** is from the camera to the anchor, before the lift; a target beyond
  its track distance is dropped.
- **Occlusion:** `AIUtil::CanSeePosition` (P `00486740`, M `0058F4E0`), a ray test with
  mask `0x9E` ignoring viewer and target, one ray per frame round-robin.
- **Info flags** at +C: `0x02` on screen, `0x04` visible, `0x40` tracked, `0x80`
  friendly, `0x10` locked, `0x08` attacker; +D bit 0 marks a flag.

`LockOnManager::CalculateScreenCoordinates` (P `0063A2A0`, M `00453DF0`, S
`0057B430`, G `0057C1B0`) places a target and pins it to the screen edge:

1. Project through the camera's inverse matrix and
   `TransformCameraPointToProjectionSpace`; behind the camera, negate x and y.
2. `facing` = dot(view direction, direction to target). Except in flyers, if
   `facing` < 0.2 and |y| > 0.3|x|, squash y toward 0.3|x|, so side and rear targets
   slide to the left and right edges rather than the top and bottom.
3. On screen if `facing` > 0 and |x| and |y| are within `mSafeZone` (+96C, on all
   three builds); otherwise both are scaled by `mSafeZone / max(|x|, |y|)` onto that
   square. `mSafeZone` is `s_screenFull.m_fSafeZoneWidth`, 0.9 on Phantom
   (`RedRenderer::internalResetState`, P `0087FB30`); the shipping value is unverified.

`Target::Update` (P `0060A360`, M `006A7FD0`, S `00558F30`, G `00559CA0`; the element's
own Update is P `0060A0A0`, M `006A9170`, S `0055A340`, G `0055B0B0`) draws it at
x = `ConvertXForWideScreen((x·0.5 + 0.5)·W)`, y = (y·0.5 + 0.5)·H, rotated by
atan2(x, −y) off screen, scaled by `lerp(MinScale, MaxScale, clamp((d − min) /
(max − min)))` (flyers and remote terminals use fixed 200 to 1700 m ranges). There is
no distance fade: occluded targets swap to the "Behind" templates, which carry a lower
`Alpha`. Common targets take the marker's colour and pulse, but the icon is always the
template bitmap; the marker's texture is used only on the minimap.

Stock single-player conquest already puts these markers on command posts:
`ObjectiveConquest.lua` calls `MapAddEntityMarker(cp, self.icon, 4.0, self.teamATT,
"YELLOW", true)` only when `multiplayerRules` is off (lines 264 to 303; checked).

### Why command post markers should not reuse it

Feeding posts in with data (`MapAddEntityMarker` on each post, which replicates to
clients) is limited: GameObjects only, one bitmap for every objective, a static colour
where the first marker's colour wins and nothing is relative to the viewer, no capture
progress, 32 slots shared with every objective, and a fixed 100 to 10000 m scale range
that barely shrinks on infantry maps. Feeding them from the DLL through
`AddTargetCommon` would make the stock element draw an objective icon over every post
in unmodified HUDs, and `RemoveMarker` (P `00774620`) removes common targets by object,
so conquest removing its marker on capture would remove ours too.

### The minimap

- **Posts:** `TargetManager::AddPost` (P `00773F10`, from `CommandPost::Respawn` P
  `004E23F0`) fills `gPost[16]` and sends `map.refreshPost`; `EventRefreshPost` (P
  `005FF900`) takes the icon from the class's `MapTexture`; `UpdatePostIcons` (P
  `00603470`) places it from the post object's translation. Lua
  `MapHideCommandPosts` (P `00656060`) sends `map.hideCPs`.
- **Markers:** `gMarkers[32]`, 40 bytes each (actor, region, team mask, texture hash,
  scale, colour, `showHudTarget` +28, pulse scale, flash and pulse bits +36; array
  pointer P `00C47A74`, M `00B47140`). Lua signature:
  `MapAddEntityMarker(ent, icon, scale, team (0 = all), color, showHudTarget = true,
  flashAlpha = true, pulseSize = true)`. `UpdateMarkerIcons` (P `00602980`) places a
  marker at the sphere centre plus the smoothing offset, or at a region's centre
  (`RedRegion` +30); a colour with alpha 0 means the viewer's team colour.
- **Other stock world-space drawing:** command post holograms (`CommandPost::Attach`,
  P `004DFF60`, at bone `0xD1E4CC7A`, never on a dedicated host), player name tags
  (`NameDisplayThread`) and item countdowns (`ItemTimerDisplay`).

### Distances the stock HUD shows

- **`player1.lockOnDistance`** (`GameEvents::UpdateLockOn`, P `006148B0`, M `006B2720`,
  S `00560520` fastcall, G `005612A0`): from the controlled object's translation
  (remote, else vehicle, else soldier) to the locked object's (`LockOnManager` +928),
  in world units, which the engine calls metres. Sent every HUD tick while locked,
  **only** in an `EntityFlyer` or a remote-piloted `EntityBuildingArmed` (pilot type
  +144 = 3), with the map closed; `PlayerEvents` +300 on M and S. The stock PC HUD
  prints it in `player1lockon.distance` with `FloatFormat("%.0f")`. Infantry has no
  distance at all.
- **`missileLockDistance`** (Uint) is `Controllable::mEnemyLockedOnMeDistance`, every
  tick while an enemy has a lock on you, flyers and turrets only (`UpdateMissleLock`,
  P `00615080`).
- **Name tags** (`NameDisplayThread::Update`, P `00678000`, M `0067F1E0`, S
  `005AAA60`, G `005ABA10`): anchored at the origin plus radius × 1.2 up, hidden behind
  the camera, no occlusion test, alpha 255 to 20 m (350 m for a named flyer pilot) then
  335 − 4·d, gone by about 84 m. Always on in split-screen; in single-camera play only
  online with `netShowNames` (M `00ADABCE`, S `007E8E50`). Item countdowns
  (`ItemTimerDisplay::DisplayThread::Update`, P `00621720`, S `0056AC50`, G
  `0056B9D0`) use the same 20 m fade.
- **Printing:** `EventNumber` takes Int, Uint or Float; `FLT_MAX` prints nothing, or
  `--` with `InfiniteDashes(1)` (key present on M and S).

### Where a command post is

- `CommandPost`: `mObject` +2C; `mPosition` +34, the translation cached at attach (M
  `Attach` `0064B880`, write `0064B8B5`; the retail offset is inferred);
  `mCaptureRegion` +80 (M `00591614`, S `0058FF0D`, G `00590EAD`) with its centre at
  `RedRegion` +30. `AIUtil::GetCommandPostPos` (M `00591610`) uses the region centre,
  else the object's translation.
- The GameObject: matrix +F0, up row +100, translation +120; collision sphere +18 and
  radius +24, or, when the pointer at +10 is set, at `stack + 0x40 + index·0x10` with
  the index at +14 (read on M and S).
- Neither `CommandPostClass_data` nor `GameObjectClass_data` has a HUD height. The stock
  anchors: objective markers sphere centre + up × 1.3; name tags origin + radius × 1.2;
  item countdowns sphere centre + radius × 1.2; the minimap the object origin.

### Designs

Items 1 and 2 were built on 2026-09-28: the distance in `render/target_bar_latch.cpp`,
the markers in `render/hud_command_posts.cpp`, the placement in
`render/hud_world_markers_core.hpp`. As built:

- The anchor reads the sphere as the stock code does (+10 stack, +14 index, +18
  centre; audited on all three builds with the 1.3 m lift), but lifts along the
  object's matrix up row (+100) rather than the rendered matrix the stock virtual
  returns, and skips the bone `0x3483A489` override. The two are the same for a
  static post.
- The safe square is the constant 0.9. The shipping builds' safe-zone value is still
  unread.
- Occlusion is not built.
- The distance from what the player controls falls back to the camera while the player
  is dead.
- Positions are rounded to whole pixels unless `HudSubPixel` is on; see
  [Pixel snapping](#pixel-snapping-built-2026-09-29).

The designs as researched:

1. **`player1.weaponN.target.distance`** (Float, metres). After the stock update, from
   `gPlayerData.weaponData[ch].target` (+14/+18), which already holds our retention;
   measured as `lockOnDistance` is, controlled object's translation to the target's.
   Send `FLT_MAX` with no target, so text prints blank or `--`. Gate it on its own
   listener; it stays through the retention hold like every `target.*` event.
2. **Per-slot command post markers**, pumped beside `hud_command_posts_update` and using
   its slot order: `player1.commandPostN.position` (Vector3 viewport fractions: the
   projection, or the pinned edge point, pixel-snapped), an `onScreen`/`offScreen` Bool
   pair, `direction` (Vector3 `(0, 0, degrees)` for `EventRotation`, the stock angle
   atan2(x, −y)), `distance`, and optionally `visible`/`occluded` from one
   `engine_ray_hit` per tick round-robin, as `barrel_fire_origin.cpp` casts. Anchor at
   the stock objective anchor (sphere centre + up × 1.3), where stock conquest's
   markers sit, falling back to `mPosition` plus a height. Copy the stock pin rule
   (mirroring behind, the side squash, scaling onto the 0.9 square) into a new testable
   header; for points behind or near the camera use (x/tanW, y/tanH) rather than
   dividing by depth. The target bar's `project_anchor` and `pin_to_screen` follow a
   different rule. Scale and fade are left to `.hud` authors through transforms.
   Online, projection, distance and rays are local; ownership replicates; capture
   progress only simulates within 100 units of the player on a client, as the strip
   already handles. In single-player conquest the stock objective icons appear too.

### Not determined (markers)

The names behind bones `0x3483A489` and `0xD1E4CC7A`; the ODF keys for the lock-on
distances; who sets the nearest-target count (`SetupAttackerTargets`, P `0063B240`,
looks like a stub); whether `RayHit`'s mask means the same as `RayTest`'s `0x9E`; the
shipping safe-zone value; `CanSeePosition` on S and G (probably inlined); `mPosition`
+34 on retail; whether `EventRotation` turns the same way as the stock arrow; whether
slot bitmaps honour a template's fades; and a probably harmless stock slip where
`EventRefreshTargetCommon` indexes `gMarkers` with a common-target index.

## Pixel snapping (built 2026-09-29)

Moving HUD elements stepped one or two pixels at a time instead of gliding. Two things
rounded them, and `[Features] HudSubPixel` (`render/hud_sub_pixel.cpp`) turns both off.
Addresses are modtools (M), Steam (S), GOG (G) and Phantom (P).

### The draw rounds every element

Every interface element, the HUD and the shell's `ifs` screens alike
(`ScriptCB_AddIFText`, P `006413A0`, adds a `RedTextElement` to the same hierarchy), is
drawn by one non-virtual `RedInterfaceElement` draw, thiscall(`const PblMatrix*`
parent, `const RedColor*`), RET 8:

| | M | S | G |
|---|---|---|---|
| draw | `00816FA0` | `006C0DE0` | `006C1E70` |
| `CALL floor`, x | `00816FFE` | `006C0E43` | `006C1ED3` |
| `CALL floor`, y | `00817014` | `006C0E66` | `006C1EF6` |
| `floor` | `008D5020`, the CRT's | `0075317C`, MSVCR120 import thunk | `0075427C`, the same |
| called from | `00817CB8`, `00838C34`, `00838C4B` | `006C142F`, `006D6B9E`, `006D6BB5` | `006C24BF`, `006D7C3E`, `006D7C55` |

With the enabled flag set (+14 bit `0x100`, `m_uiFlags` bit 0), it multiplies `m_mLocal`
(+30) by the parent matrix, rounds the result's translation x and y as
`floor(v + 0.5)`, sets w to 1, multiplies the colour, and calls `RenderUsingContext`
(vtable +40) with the rounded matrix, then hands the same matrix to the hot spot (+18).
The first caller is `RedInterfaceScreen::Render`, once per top-level group. The other
two are `RedGroupElement::RenderUsingContext`, once per child, with and without
`PropagateAlpha` (+84 bit 0). So a child starts from its group's rounded matrix and is
rounded again. Modtools adds the 0.5 with x87 `FADD` and retail with `ADDSS`; both pass
the sum to `floor` as a double on the stack.

Phantom does not round: its `RedInterfaceScreen::Render` (P `008930E0`) and
`RedGroupElement::RenderUsingContext` (P `008C38F0`) inline the same multiply, colour
and call, with no `floor`.

HUD units are device pixels (`HUD::Element::SetupViewportDimensions`, P `005F57D0`, from
`s_screenFull`). `EventPosition` stays in floats all the way to the matrix: the group
callback (M `0069A350`) converts by the group's relative mode with a multiply
(M `00691120`), `RedInterfaceElement::SetPosition` (M `008285C0`) stores floats, and the
alignment step (M `0069A230`) adds float offsets.

### Rounding that stays

These place an element's pieces relative to it once, when they are set, and never move
the element:

- `RedBitmapElement::SetRect` (M `00839130`) rounds its four edges into the quad; the
  bordered variant (M `008398A0`) rounds six.
- `RedGlyphCacheElement::AddGlyph` (M `00827C40`) rounds each glyph quad as
  `RedTextElement::WriteCache` (P `008C6A20`) lays text out in local space.
  `RedGlyphCacheElement::RenderUsingContext` (P `008C4870`) draws the cache with the
  matrix as given.

### GameExt rounded too

`hud_world_markers::place` (the command post markers) and
`target_bar_geometry::pin_to_screen` and `snap_to_screen` (the floating target bar)
rounded their positions to whole pixels as well. With the draw patched but those still
rounding, markers moved in even one-pixel steps instead of uneven ones: smoother, but
still stepping. They now round only while `HudSubPixel` is off.

### Filtering

The interface samples textures bilinearly: `pcInterfaceShader::Begin_Fixed`
(P `008F29C0`, M `00870270`) sets `MAGFILTER`, `MINFILTER` and `MIPFILTER` to `LINEAR`
on both stages. An element a fraction of a pixel off is filtered across two pixels,
so it glides in motion and looks slightly soft at rest.

### `HudSubPixel`

On by default since 2026-10-04 (off when first built). When on, both floor CALLs go to
`keep_fraction`, cdecl(double) → ST0 like `floor`, which returns its argument less 0.5,
so the translation keeps its fraction. Its only error is what the draw's own single-precision add already rounded,
where 0.5 carries a coordinate into the next power of two: at most half a float step,
2^-10 of a pixel at 16384.

The install checks the draw's prologue through its enabled-flag test, then at each site
`FSTP qword [esp]`, `CALL floor` and the add's constant of 0.5, before writing either
CALL. If any check fails it leaves the draw alone and clears the switch, so the markers
and target bar go on rounding with it. `tests/hud_sub_pixel_abi_tests.py` audits the
same bytes on all three builds, and `tests/hud_sub_pixel_tests.cpp` runs the stand-in,
including under the 24-bit x87 precision Direct3D sets. Confirmed much smoother in
play on modtools; Steam and GOG are audited, not played.

### Per element (not built)

A `SubPixel(1)` property covering one element and everything inside it would need:

- **A reader.** `HUD::Element::ReadData` (M `00693790`, S `00549250`, G `00549FA0`;
  thiscall(`PblConfig*`, `Data*`) → bool, RET 8) is where every element type ends up:
  its only direct callers on each build are `ElementGroupBase`, `ElementBitmapBase`,
  `ElementText` and `ElementModel3D::ReadData`. `mElement` (+B0) exists by then; the
  stock `PropagateAlpha` writes straight into it.
- **A mark.** Bits 29 to 31 of +14 are outside the declared bitfields (`m_zorder:8`,
  `m_uiFlags:21`). The constructors clear bits 11 to 31 (`AND [+14], 5FF`: M `00828E56`;
  S `006BFF68`, `006C004E`; G `006C0FF8`, `006C10DE`), and the other writes read on
  modtools (destructor, name registration, z-order) change only bits 0 to 10.
- **A count.** A detour on the draw counting marked elements while they draw, since a
  group draws its children inside its own draw. The stand-in would call `floor` while
  the count is zero, and GameExt's position events could stop rounding altogether, as
  the draw rounds an unmarked element anyway.

## Wide screens (built 2026-10-05, `render/hud_true_widescreen.cpp`)

What the stock game does to a HUD on a screen wider than 4:3, and `TrueWidescreen(1)`,
the per-file opt-out of it. Addresses are modtools (M), Steam (S) and GOG (G). The
stock mapping was read on modtools and measured in game at 1280×720
with a temporary diagnostic (2026-10-05); the user's own measurement agreed
(`BitmapRect` 128 by 108 pixels looked square).

### The stock mapping

A screen is wide once height/width is below 0.75. Then:

| Step | Where | Effect at 16:9 |
|---|---|---|
| Positions and sizes are fractions of the real screen | `HUD::Element::SetupViewportDimensions` (M `00692A30`, S `00549DE0`, G `0054AB30`) sets `sViewportWidth`/`Height` from `s_screenFull` | the layout spreads to the edges, a third wider than at 4:3 |
| The HUD screen groups are drawn through a 640×480 mapping, and each group's local matrix is letterboxed | `RedInterfaceScreen::Render` (M `00817870`, S `006C0F70`, G `006C2000`); the letterbox is set by the loader for each group (M `006B7300`, S `00565060`, G `00565DE0`) | x exact, y = H/18 + 8/9 y: a band from 40 to 680 at 720p |
| Each bitmap's drawable is scaled (1, 0.75 / aspect) | the bitmap rect setup (`ElementBitmapBase::PostReadSetup`) | 4/3 tall, 32/27 after the band: `BitmapRect` 128×108 is square |
| The minimap is scaled by 1 + (0.75 / aspect − 1) / 4 | `ElementMap::PostReadSetup` | |
| A segmented bar's ring is kept round | `ElementBarSegmented::SetValue` scales the ellipse's vertical radius by 0.75 / aspect | |
| Text gets nothing | | 8/9 flat |

All of it applies per screen group, and every `.hud` file shares the groups, so it
cannot be turned off for one file at the group. `ReticleCorrection`
(`render/hud_widescreen.cpp`) moves the stock reticule back onto the aim point
inside the band.

### `TrueWidescreen(1)`

An opted-in file is laid out as on a 4:3 screen of the real height, W43 = 4H/3, and
drawn one layout pixel to one screen pixel:

1. **The flag.** `HUD::Manager::ConfigFile` (the `FileInfo` item) `ReadData`, its vtable
   +20, is detoured for the PblHash of `TrueWidescreen`, `0xCA64BB16`.
2. **Which elements are the file's.** `HUD::Manager::Load` (one call per file) is
   detoured: the last node of `HUD::Element::sList` before the load against the list
   after it gives the elements the file made (the element constructor appends; node at
   +B4). Load's read of each top-level item (`MOV ECX,item / CALL [vtable+8]`) is
   replaced by a CALL to a stand-in that notes the item and goes on into its `Read`.
   While the file loads, `sViewportWidth` holds W43: `ElementMap`'s constructor and
   `ElementText`'s `TextBox` read it directly.
3. **The 4:3 width afterwards.** `GetContainerViewWidth` returns W43 for an element of a
   placed piece, and the relative-to-pixels conversion is handed W43 as the Screen
   width (mode 1) when the view width is W43, so events that move the file's elements
   later use the layout too.
4. **No stretch.** The six `GetScreenAspectRatio` calls in the bitmap rect setup,
   `ElementMap::PostReadSetup` (four) and `ElementBarSegmented::SetValue` return 0.75 for
   an opted-in element.
5. **Placement.** After the load, each top-level item gets a slide: 0, (W − W43)/2 or
   W − W43 by the third of W43 its x falls in. One at (0, 0) is a plain container: its
   direct children are placed by the same rule instead. An `ElementTarget` (by vtable) is
   never slid: `ElementTarget::Update` (its vtable +2C) places its markers from the real
   width.
6. **The draw.** A detour on the element draw counts depth under the HUD screen groups
   (`gHudViewPorts`, five `RedScreenGroupElement`s 0xA0 apart). At depth 1 an opted-in
   piece is drawn under a parent of its own instead of its group's letterboxed one: one
   pixel to one pixel on the plane the groups are drawn on, z = 640/t, where the
   interface camera (frustum width t at distance 1) sees 640 units across, so
   k = 640/W units per pixel, and slid. At depth 2, under a plain container, a child's
   parent gets its slide × k added.
7. **The world.** The `EventPosition` handler (`ElementGroupBase`'s, cdecl(`Event*`,
   element)) gets, for an opted-in element, a copy of the event whose x is
   (v·W − slide)/W43, which lands on the real point v·W, for
   `player1.weaponN.reticule.position`, `.lockOnPosition`, `.target.position` and
   `player1.commandPostN.position`. A reticule's y has `ReticleCorrection` taken back
   off (`hud_widescreen_reticle_uncorrect`).

Only with one viewport (`CameraManager::m_iNumCam`, +1C, is 1) and on a wide screen;
the screen is read as each file loads. A piece is found in the drawable tree, the one
the draw walks: a drawable's parent is its link at +1C less 0x70 (`RedGroupElement`'s
child list), checked by the link's item pointing back (node `{ list, next, prev, item }`).
`HUD::Element`'s own group pointer (+F4) is only set for some, and a top-level drawable
is only in its screen group while enabled (`HUD::Element::UpdateEnableState`, vtable
+34: M `00692860`, S `00549BF0`).

The modtools HUD editor writes a file back through the same per-element width, so it
writes the 4:3 numbers. Three gaps are closed on modtools: `ConfigFile::WriteData`
(vtable +28) is followed by `TrueWidescreen(1)` for a `FileInfo` that read it and is
still in `gConfigFiles`; the pixels-to-relative conversion gets W43 for Screen mode; and
`ElementText`'s `SetProperty`, `GetProperty` and `WriteData`, which read
`sViewportWidth` for `TextBox`, run with it lent. GameExt turns the editor off on Steam
and GOG (see [The HUD editor](#the-hud-editor)), so those hooks are modtools only.

### Addresses

| | M | S | G |
|---|---|---|---|
| `ConfigFile::ReadData` | `006B7F50` | `00564DB0` | `00565B30` |
| `HUD::Manager::Load` | `006B8480` | `00565950` | `005666D0` |
| its read of a top-level item | `006B8730` | `00565B5C` | `005668DC` |
| `GetContainerViewWidth` | `006926B0` | `005490B0` | `00549E00` |
| relative-to-pixels conversion | `00691120` | `00547010` | `00547D60` |
| `EventPosition` handler | `0069A350` | `0054F110` | `0054FE60` |
| element draw | `00816FA0` | `006C0DE0` | `006C1E70` |
| `HUD::Element::sList` terminator | `00AD7EE0` | `007EBA18` | `007EC9E8` |
| `gHudViewPorts` cell | `00BA4478` | `01E573C8` | `01E58878` |
| interface frustum width t | `00E5B504` | `0093E4E0` | `0093F980` |
| `GetScreenAspectRatio` | `008059A0` | `006B1890` | `006B2910` |
| aspect call, bitmap rect setup | `006988F9` | `0054D738` | `0054E488` |
| aspect calls, `ElementMap` | `0069BA00`, `0069BA4E`, `0069BA66`, `0069BAA3` | `005521AC`, `00552208`, `00552226`, `0055227E` | `00552F0C`, `00552F68`, `00552F86`, `00552FDE` |
| aspect call, segmented bar | `00696B97` | `0054C24C` | `0054CF9C` |
| `ConfigFile` vtable | `00A60398` | `007A329C` | `007A4064` |
| `HUD::Element::sViewportWidth` | `00BA38FC` | `01E56C34` | `01E580E4` |
| `ElementTarget` vtable / `Update` | `00A5E000` / `006A9170` | `007A17FC` / `0055A340` | `007A2658` / `0055B0B0` |
| `ConfigFile::WriteData`, indent and format writers | `006B8090`, `006B5A20`, `006B5A50` | | |
| pixels-to-relative conversion | `00691170` | | |
| `gConfigFiles` / count | `00BA4070` / `00BA4480` | | |
| `ElementText` `SetProperty` / `GetProperty` / `WriteData` | `006AAE10` / `006A95C0` / `006A9A90` | | |

Retail was found through RTTI, which Steam and GOG keep: `FileInfo` is
`.?AVConfigFile@Manager@HUD@@`, and `ElementGroupBase`'s constructor (S `0054E3B0`,
G `0054F100`) registers `EventPosition` with a `PUSH` of its address. The frustum width
is not beside `s_screenFull` on retail as it is on modtools; it is the third argument
of the interface camera's `SetFrustum` in `RedInterfaceScreen::Render`. GOG matches
Steam byte for byte but for moved addresses in every function above.

### Contracts

Modtools and retail differ, LTCG-built retail in four places:

| | M | S, G |
|---|---|---|
| `Manager::Load` | cdecl(`PblConfig*`) | the `PblConfig` in ECX, plain RET |
| its read of an item | item in EDI, vtable in EDX | item in ESI, vtable in EAX |
| `GetContainerViewWidth` | thiscall → ST0; keeps ECX (PUSH/POP) and EDX | ECX → XMM0; changes only EAX, ECX and XMM0 |
| relative-to-pixels conversion | cdecl(mode, value, frame, view, screen) → ST0 | mode in ECX, value, frame and view in XMM1-3, screen on the stack (caller pops) → XMM0; changes only XMM0 and XMM1 |
| aspect calls | element in ESI (bitmap), EBX (map); the bar's `ElementBar` base, element + 0x200, in EBX | element in ESI (bitmap and map); the bar's base in EDI |
| `ConfigFile::ReadData`, `EventPosition`, the draw | thiscall, RET 8; cdecl; thiscall, RET 8 | the same |

Callers on both kinds of build keep values in registers a small helper leaves alone:
modtools' `HUD::Matrix::WritePosition` keeps the property name in ECX across two
pixels-to-relative conversions (M `00691628` to `00691690`), and `ApplyRelativeMode`
pushes the mode from ECX again after a conversion (M `006913B1`); retail's
`Element::WriteData` keeps a value in EDX across `GetContainerViewWidth` (S `005498BA`).
So every stand-in is naked and gives back every register but the result: ECX and EDX
on modtools, and on retail every general and XMM register. Load, `ReadData`,
`EventPosition` and the draw are C detours: their callers reload what they use after
the call.

### Checks

The install compares each function's opening bytes; on retail, through a mask, with
each address the loader moves inside them (Load's camera manager and `gHudViewPorts`,
`GetContainerViewWidth`'s `sViewportWidth` and screen width, the conversion's own jump
table) checked against the address it should name, moved with the exe. It checks the
`ConfigFile` and `ElementTarget` vtable slots and that each aspect site is a `CALL` to
`GetScreenAspectRatio`, and installs nothing if anything differs.
`tests/hud_true_widescreen_abi_tests.py` audits all of it and the contracts above on
all three builds, including that every moved address inside compared or written bytes
is one the install handles; `tests/hud_true_widescreen_tests.cpp` tests the placement
maths on five screen shapes. Played on modtools at 16:9; Steam and GOG are audited, not
played.

## Colour gradients (researched 2026-09-30)

Every element is drawn in one colour, its tint, so an icon or bar only shows a gradient
baked into its own texture. Read on Phantom (P); nothing here is ported or built.

### How an element's colour reaches the screen

- A `RedBitmapElement` keeps four `BitmapVertexT` {x, y, z, u0, v0, u1, v1}, 28 bytes
  each, and a `pcInterfaceShader*` of its own (`RedBitmapElement_data` +18 and +88). There
  is no colour per corner. `SetTexture` (P `008C8EE0`) writes the hash into that shader,
  creating it through the virtual `Init` if it is missing, so each bitmap owns its shader.
  `RenderUsingContext` (P `008C8CB0`) copies the four vertices into a strip for
  `RedRenderer::pcRenderPrimitive` (P `00882870`), which keeps the tint as the render
  item's `tweakColor`.
- `pcInterfaceShader` holds `m_textureHash`, `m_bitmapMaskHash`, `m_shadowEnable`,
  `m_mode` and `m_materialFlags`, and has two pipelines (`_InitPipelineFuncArray`,
  P `008F34D0`): fixed function and Prog1. The Prog1 render (P thunk `0040C7DE`) uploads
  the tint as pixel shader constant c0, one value for the whole draw, and loads the vertex
  format for the mode.
- The modes, as the fixed-function render (P thunk `00405362`) spells them out:

| Mode | Result | Vertex |
|---|---|---|
| Bitmap | tint, or tint × texture | position, UV |
| Masked bitmap | tint × texture × mask (stage 1 at u1/v1), colour and alpha | position, two UVs |
| Vector | tint × the vertex's colour, blended across the shape | position, colour |

- Vertex format bits (`pcRedVertexFormat::CreateVertexDeclaration`, P `008ED270`): 0x2
  position, 0x20 normal, 0x40 tangents, 0x80 colour (D3DCOLOR), 0x100 a second colour,
  0x200 to 0x600 one to three UV sets. A vector point is 0x82, 16 bytes.
- Both samplers clamp and filter bilinearly (`Begin_Prog1`, P thunk `00411982`).

### What stock can do

- `BitmapMasked` (`Mask`, `MaskTexCoords`) gives tint × texture × mask, so a gradient
  mask texture tints an icon without baking the gradient into the icon. No stock HUD uses
  it.
- A `BarBitmap` draws through a plain `RedBitmapElement` (+B0) in bitmap mode, so a bar
  cannot take a mask.
- Vector mode has a colour per corner but no texture.
- `ZOrder`: `SetZOrder` (P `00891EE0`) keeps a group's children highest first, and the
  group draws from the head, so a higher ZOrder draws first, underneath.

### Designs

1. **Masked mode with a runtime gradient.** Switch the element's own shader to masked mode,
   with a gradient texture GameExt makes from the colours (rebuilt when an event colour
   changes) as the mask, and set u1/v1 per corner from the gradient's direction across the
   element's full rectangle, so any angle works. For a bar, recompute u1/v1 as the fill
   moves, so the gradient stays fixed to the full bar; the FillFrom `SetValue` hook
   already has the full rectangle. One pass per element. Needs a way to create a
   `RedTexture` and register it in `RedTexture::s_textureHashTable`, where the render looks
   the mask up by hash.
2. **Strips.** Draw the element as N strips, each with its own tint and slice of UVs,
   clipped to a bar's fill. No texture and any number of colours, but N draws per element
   and visible bands at small N.
3. Not viable: a colour per corner on a textured element in one pass needs a new shader,
   and Shader Patch replaces the game's shaders.

Not determined: how to create a `RedTexture` at runtime, and whether Shader Patch accepts
one; Shader Patch's masked and vector states; the modtools and retail addresses.

## Lua and the HUD (researched 2026-09-28)

Addresses are Phantom (P) unless a build is named. Nothing here is built.

### Load order: a script can create events in time

| Step | P | M | S |
|---|---|---|---|
| `LuaHelper::InitState`, where GameExt registers its Lua functions | call `005DF76F` in PreStateInit `005DF6D0` | call `0044EF5E` in PreStateInit `0044EEC0` | in PreStateInit `0053AF10` |
| `HUD::Manager::Open`, which ends with `GameEvents::Open` (GameExt's `hooked_Open`) | call `005DF77E` → `00619970` | call `0044EF6D` → `006B8A00` | call `0053AFC4` → `00565140` |
| `MISSION.lvl`, `OpenScript` (file scope), `InitMission`, **ScriptPreInit** | `005DF85A` | push `0044F078` | push `0053B0C1` |
| `GameLoop::Init`: `EventManager::Init`, InitState again, **ScriptInit** | `005CC3A0`; ScriptInit call `005CCB4E` | `00734040`; push `007347B7` | `00530EE0`; push `0053169B` |
| `GameLoop::PostLoad`: **ScriptPostLoad** | `005CD2B0`, call `005CD90E` | `007352B0`, push `0073586A` | `00531A70`, push `005320C9` |

Checked on M: the `Manager::Open` call at `0044EF6D` comes before the `MISSION.lvl`
push (`0044EFA1`) and the `ScriptPreInit` push (`0044F078`).

A Lua `ReadDataFile` parses synchronously: `Lua_Callbacks::ReadDataFile` (P `00651ED0`)
→ `LoadUtil::ReadDataFile` (P `00638F30`) → `ReadDataFileOnHeap` (P `006398D0`) →
`ReadDataFileChunk` (P `00638F60`) → `HUD::Manager::Load` (P `00619510`) →
`Item::ReadEvent` (P `00617E30`), which only finds. On S, `FindByHashID` (`0055DEE0`)
is called only by `ReadEvent`, `CreateEventA`, `SetEvent` and the HUD editor, and every
access to `sList` lies inside `0055DC30`–`0055DFD2`, so no reader has its own lookup.

Everything is torn down at each state change: `MissionState::Exit` (P `005DD240`) →
`PostStateCleanup` (P `005DF0D0`) runs `LuaHelper::CleanupState` and then
`HUD::Manager::Close` (P `00619070`), whose `EventClass::DestroyAll` frees every event.

So a Lua call at file scope, in `ScriptPreInit` or at the top of `ScriptInit` runs
after `GameEvents::Open` and before `ReadDataFile("ingame.lvl")`, the first load in
stock `ScriptInit`, and every `.hud` read after it can bind the event. Conditions:

- Create with `EventClass::Create(type, "%s", name)` (P `0060F400`, M `006AD8A0`, S
  `0055DE40`, G `0055EBC0`) after `FindByHashID`: `Create` never de-duplicates.
  Not `CreateEventA`, which applies a stale `sEventFilter`.
- Wrap it in `RedSetCurrentHeap(RunTimeHeap)`. Static reading says RunTimeHeap is
  current during ScriptInit, but [RedHeapSystem.md](RedHeapSystem.md) says TempLoadHeap
  throughout; unresolved, so set it explicitly.
- Reset any per-name cache in `hooked_Open`, not in `hooked_init_state`: InitState runs
  three times per mission.

### Stock Lua functions that reach the HUD

| Lua function | How it reaches the HUD | Multiplayer |
|---|---|---|
| `ShowMessageText` (P `006554A0`, S `0058D6A0`) | `GameEvents::DisplayCenterMessage` (P `00611B20`) sends `player1.message.color` and `player1.message` | Replicated to clients (`NetGame::CreateEvent_ScriptMessage`, P `0068AD00`) |
| `ShowTimer` (P `00655660`) | `GameEvents::SetObjectiveTimer` (P `00613230`), read by `GameEvents::Update` | Host and single player; the timer itself replicates |
| `SetMissionTimer` / `SetVictoryTimer` / `SetDefeatTimer` | `MissionTimer::Set` | Replicated (`AloMissionTimer`) |
| `ShowObjectiveTextPopup` / `ShowSelectionTextPopup` | `PopupText::ShowPopup` (P `00725910`): `objectivePopup` / `selectionPopup` plus three Bool events | Local |
| `MapHideCommandPosts` (P `00656060`) | sends one Bool event itself, the only stock direct send (class at P `00B270EC`, presumably `map.hideCPs`) | Local |
| `ShowTeamPoints` (P `00653C50`) | flags in `gPlayerData` | Local |
| `DisableSmallMapMiniMap` (P `006518E0`) | `ElementMap` flags | Local |
| `SpaceAssaultSetupBitmaps` (P `006552E0`) | finds the `player%dspaceassaultgroup` element by name (`Element::FindByHashID`, P `005F34D0`) | |

No stock Lua call can show or hide an arbitrary element. Only whole-HUD toggles exist:
`ScriptCB_DoConsoleCmd("ToggleDisplay")`, modtools only, and `ScriptCB_Freecamera`.
The engine has element-level primitives, `Element::FindByHashID` and the editor's
`Element::SetProperty` (P `005F4B00`, which can rebind `EventEnable` through
`Item::SetEvent`, P `00618020`), but no Lua binding reaches them.

### Multiplayer

- Load-phase script code (file scope, `ScriptPreInit`, `ScriptInit`,
  `ScriptPostLoad`) runs on every machine; it has to, to load each client's assets.
- Lua timers tick on the host only: `Timer::Start` (P `0077AD00`) activates on the
  host turn list, which a pure client never runs, so `OnTimerElapse` never fires there
  (static reading).
- `On…` callbacks: the registrars install `DummyLuaCallback` only when `netOnClient`
  is set at registration, but that flag is scoped (set in `NetGame::EnterClient`,
  P `0068D790`; cleared in `LeaveClient` and `NetGame::Create`), and
  `EventManager::Init` runs before the first `EnterClient` (P `005CC3FF` against
  `005CC440`). So clients probably get real registrations, contrary to
  [OnEventSystem.md](OnEventSystem.md), although fire sites gated on `netOnClient`
  still keep most runtime callbacks on the host. Needs a runtime check: log
  `netOnClient` in the `EventManager::Init` detour on a client. If clients do get real
  registrations, GameExt's own `OnCharacterExitVehicle`, fired from
  `hooked_char_exit_vehicle` with no `netOnClient` check, may run on clients too.
- HUD changes that can work on clients: load-time sends, engine-replicated output
  (messages, timers, objectives) and native publishers in `GameEvents::Update`.
  Anything driven by callbacks or timers is host-local.
- A call to a function the game does not have aborts `ScriptInit` under `lua_pcall`
  (`LuaHelper::CallProc`, P `0066AF10`) and the level loads broken, so scripts must
  guard new calls: `if HudEventSend then ... end`.

### What the element handlers accept (P)

- **Value ignored:** `EventEnable` (`005F3410`), `EventDisable` (`005F33E0`),
  `EventChanged` (`005F3380`), `Sound::EventTrigger` (`0061C140`). Sending false to an
  `EventEnable` binding still shows the element: hiding needs a second name, as stock
  does, or `TransformNumberCompare`'s `EventOutputTrue`/`EventOutputFalse`.
- **Int, Uint or Float:** `ElementText::EventNumber` (`0060BE50`; Int and Uint print
  with `IntegerFormat`, Float with `FloatFormat`), `ElementBar::EventValue`
  (`005F6650`), `EventPulseRate`, the transforms' `EventInput`, ColorBlend's
  `EventAlpha`.
- **Text:** `EventText` on Text (`0060C360`) and MultilineText (`00607960`) takes a
  String or a Uint localize hash and copies it (`RedTextElement::SetText` `008C6690`,
  `AddText` `00607180`); MultilineText appends a line per send.
- **Copied values:** Color (`005F3390`, `006078A0`), Vector3 (`EventPosition`
  `005FB930`, `EventScale` `005FBDE0`).
- **Uint or pointer:** `EventBitmap` (`005FA0B0`, a texture hash not checked for being
  loaded), `EventMesh` (`006057C0`); NameMesh's `EventInput` (`0061C8E0`) Uint only;
  `EventPlayerIndex` (`005FCE30`) Int only.

String, Color and Vector3 payloads therefore only need to outlive the synchronous send;
`SendDelayed` shares the engine's 32-slot queue and would need persistent storage.

### Design

1. **`HudEventCreate(name, type)` and `HudEventSend(name, value)`**, explicit types, in
   a reserved `lua.` namespace so the `player%` filter never rewrites the name.
   `HudEventCreate` finds first, reuses a match of the same type, and refuses a stock
   name or a different type; it creates under RunTimeHeap. `HudEventSend` checks the
   value against the type and skips when nothing listens (the handler list at +8 is
   self-linked). A Uint accepts a name and hashes it, with textures checked against the
   texture table as `hud_class_icons.cpp` does. Lua never passes Model or Texture
   pointers. Cache per name, cleared in `hooked_Open`. It needs no new detour: Create,
   FindByHashID and Send are already resolved and guarded.
2. **Then, optionally, creation at bind time:** a detour on `Item::ReadEvent` (M
   `006B6360` cdecl(Data*, Handler*); S `00564740`, G `005654C0` fastcall, Data in ECX,
   Handler in EDX; the prologues are already guarded by `hud_number_math.cpp`) that
   creates a missing `lua.` name with the type the binding key implies, but only for
   keys where that is unambiguous. It must hash the filtered name
   (`GetFilteredEventName`). An explicit `HudEventCreate` wins.
3. **Keep the last value per event and replay it once** from the `GameEvents::Update`
   detour, so a send made during loading is not lost.

Not recommended: a fixed pool of anonymous events (slots collide between scripts), and
driving elements directly through `Element::FindByHashID` and the fade virtual (stock
events fight it, and the retail slots are not derived).

### Not determined (Lua)

`netOnClient` at `EventManager::Init` on a real client; which heap is current in
`ScriptInit`; whether spawning or `initialize` resets element state after a load-time
send; the names behind P `00B270EC` and PopupText's three Bool events; the payload
types of `EventRotation`, `EventBlend`, `EventInputFactor` and `Sound::EventStop`; the
client-side HUD path for objectives and `MapAddEntityMarker`; retail addresses for
direct element control; whether the Lua IF screens can overlay in game. The gates in
the table were checked on Phantom only, `ShowMessageText` on Steam too.

## Candidate events (researched 2026-09-25, not built)

The research behind the HUD entries in `ROADMAP.md`. **Addresses are Phantom unless
stated. Nothing here is ported or built.**

### Class and vehicle icons that work online

**Built on modtools** in `render/hud_class_icons.cpp` (2026-09-26), and confirmed in
play there the same day: the unit icon follows stance changes. Vehicle icons and a
multiplayer client are not yet confirmed. Ported to Steam and GOG the same day, with
every read below taken from their executables and audited by
`tests/hud_class_icons_abi_tests.py`, and confirmed in play on retail too. Authoring is in
[HUD.md](../user/HUD.md#class-stance-and-vehicle-icons).

Today a mod can only swap its health icon per class from Lua. The BF3 mod's
`HealthIcons.lua` hooks `OnCharacterSpawn` and `OnCharacterEnterVehicle` and reloads a
per-class sub-level of `healthicons.lvl`, replacing a same-named texture. Those
callbacks are `DummyLuaCallback` stubs on a multiplayer client (see
[OnEventSystem.md](OnEventSystem.md)), so it is single player only. It also costs a
`ReadDataFile` per spawn, needs hand-kept class lists, and has no exit-vehicle handler,
so a vehicle icon stays after the player gets out.

The engine already has every piece needed to do it from the HUD update, which runs on
every machine:

- **Every entity class can name its own icon.** `EntityClass::SetProperty`
  (`Entity.cpp`) parses `IconTexture` (`0x8DDBEB4B`), `HealthTexture` (`0x0DB7468E`),
  `MapTexture` (`0x9DC38D80`) and `HUDModel` (`0x8F8FAEB3`) for every class, soldiers
  and heroes included. Phantom `EntityClass_data` holds them as name hashes:
  `mIconTexture`, `mHealthTexture`, `mHUDModelID` at `EntityClass+0x44/+0x48/+0x4C`.
  modtools matches, read off its `EntityClass::SetProperty` (`0x004D0110`: `+0x44`,
  `+0x48` at `0x004D017F`, `+0x4C`, and `MapTexture` at `+0x64`). Retail has no filename
  buffer (`mLabel +0x20`), so `+0x24/+0x28/+0x2C` are expected there, but not read yet.
  In `data_BF3`, 60 ODFs set `HealthTexture`, almost all vehicles and turrets; no
  soldier does.
- **The engine already does this for vehicles, with a mesh.**
  `HUD::GameEvents::UpdateVehicleHealth` (`0x00615930`) reads the vehicle class's
  `mHUDModelID` on entry, looks it up in `RedModel::_HashTable` (`0x800` slots), sets
  `field_0x10 |= 1` and sends `player1.vehicle.seatingMesh` (Model). Nothing does the
  same for the soldier.
- **A bitmap can be swapped by texture name hash alone.**
  `HUD::ElementBitmapBase::EventBitmap` (`0x005FA0B0`) takes `type_Uint` and calls
  `RedBitmapElement::SetTexture(uint)` (vtable `+0x4C`, body `0x008C8EE0`), which only
  stores the hash in its shader's `m_textureHash`. It takes `type_Texture` too; that
  overload (`0x008C9240`) converts the `RedTexture*` to its `m_uiNameHash` and calls the
  same function. **Nothing validates the hash**, so an unloaded name is accepted as
  readily as a loaded one. `GameEvents::UpdateTeam` (`0x00615500`) shows the stock
  pattern: `_Find(RedTexture::s_textureHashTable, 0x2000, hash)`, `field_0x1c |= 1`,
  then send.
- **The values are plain PblHashes.** `EntityClass::SetProperty` (`0x00502400`) stores
  all four properties with `PblHash::PblHash`. PblHash has no finalisation step, so the
  hash of `name + suffix` is the stored hash continued over the suffix bytes, and the
  original string is never needed.

Design, as built: from the shared `GameEvents::Open`/`Update` hooks in
`target_bar_latch.cpp`, publish for the local player

- `player1.unit.healthTexture` (Uint, a texture name hash) for the spawned soldier,
  hero or droideka, varied by stance;
- `player1.unit.stance` (Uint: 0 stand, 1 crouch, 2 prone, 3 ball), for elements that
  switch on stance directly. The raw soldier state is not published;
- `player1.vehicle.healthTexture` for `Character::mVehicle`, the vehicle or turret seat
  the stock `player1.vehicle.*` events and seating mesh come from, carrying that
  class's `HealthTexture`, which many vehicle and turret ODFs in `data_BF3` already
  set. On foot, or with no loaded icon, its Disable twin is sent; it never falls back
  to the soldier's icon, and remotes are not covered, as with the stock vehicle
  events. (First built as `player1.controlled.healthTexture`, remote, else vehicle,
  else unit; renamed on 2026-09-26 to sit with the stock vehicle family.) The stock
  `player1.vehicle.seatingMesh` is left untouched and drawn as an overlay on the icon.

Textures only. `HUDModel` versions for `Model3D` icons would follow the same rules but
are not planned. Each slot carries one name, so companion layers such as the BF3
`_background` outlines are not published; those outlines are unused, and would be worth
revisiting only if health overcharge is added.

**Stance variants are chosen by name suffix, so no new ODF property is needed.**
`HealthTexture = "rep_icon_rifleman"` names the standing icon, and the DLL looks for
`rep_icon_rifleman_crouch` and `rep_icon_rifleman_prone`. A droideka shows
`<base>_ball` only while fully balled (state `0x0C`); rolling up (`0x0B`) and unrolling
(`0x0D`) keep the standing icon. A missing variant falls back along a chain: prone to
crouch to standing, crouch and ball straight to standing. Only loaded textures count,
and that has to be checked before sending, because `SetTexture` accepts any hash:
`_Find` on the texture table the way `UpdateTeam` does. The result is cached until the
class or stance changes, since a miss walks the table and that walk has no bound if the
table is ever full. `UpdateTeam` also sets `RedTexture::m_bFind` (`+0x1C` bit 0) on the
texture it sends. The only reader found is the "Used" column of the debug report
`RedTexture::DumpTextureMemUsage` (`0x00889D00`), so the DLL does not set it.

`SoldierState` is an enum in the PDB: 0 `STAND`, 1 `CROUCH`, 2 `PRONE`, 3 `SPRINT`,
4 `JUMP`, 5 `ROLL`, 6 `JET_JUMP`, 7 `JET_HOVER`, 8 `FALL`, 9 `FLY`, 10 `TUMBLE`,
11 `BOUNCE`, 12 `FLY_RECOVER`, 13 `TUMBLE_RECOVER`, 14 `CHOKE`, 15 `DEAD`,
16 `HERO_FLEE`, 17 `PILOT_ANIMATION`, 18 `ARMED_PASSENGER`, 19 `SLIDE`. Only 1 and 2
pick a variant. Every other state, knockdowns included, counts as standing. The
similarly named `Combo::Condition::SOLDIERSTATE` is a bitmask for combo conditions, not
the stored state.

What it reads, with the site each value was read at:

| Source | modtools | Steam | GOG |
|---|---|---|---|
| Soldier `mState`, from the Controllable | `+0x514` | `+0x504` | `+0x504` |
| Droideka `mState`, from the object start (`layout::EntityDroideka`) | `+0x1A74` | `+0x1A54` | `+0x1A54` |
| Droideka RTTI hash global | `0x00B7D934` | `0x01EBBC58` | `0x01EBD06C` |
| `Trackable::GetGameObject`: vptr at Controllable `+0x18`, slot | `+0x1C` @ `0x006B48EF` | `+0x1C` @ `0x00561D8C` | `+0x1C` @ `0x00562B0C` |
| `GameObject::GetEntityClass`: primary vptr slot | `+0x28` @ `0x006B4AE3` | `+0x28` @ `0x00561EF7` | `+0x28` @ `0x00562C77` |
| `EntityClass::mHealthTexture` (`SetProperty`'s store) | `+0x48` @ `0x004D017F` | `+0x28` @ `0x00491B5D` | `+0x28` @ `0x00491B5D` |
| `PblHashTableCode::_Find`, `cdecl(table, 0x2000, hash)` | `0x007E1A40` | `0x00726E00` | `0x00727ED0` |
| Texture table, `s_textureHashTable._uiTable` | `0x00D4F994` | `0x008EED8C` | `0x008F022C` |
| `EventBitmap` with a Uint: `SetTexture(uint)` vtable slot | `+0x50` @ `0x00698F40` | `+0x50` @ `0x0054DBF0` | `+0x50` @ `0x0054E940` |
| `BarBitmap` constructor, building an `ElementBitmapBase` | `0x00695DB0` | `0x0054AA30` | `0x0054B780` |

Retail `EntityClass::SetProperty` is `0x00491AF0` on both Steam and GOG and stores
`IconTexture +0x24`, `HealthTexture +0x28`, `HUDModel +0x2C` and `MapTexture +0x44`;
retail `UpdateVehicleHealth` (Steam `0x00561D70`, GOG `0x00562AF0`) reads `HUDModel` at
`+0x2C`, which agrees. Steam and GOG are byte-identical at every site above apart from
call targets.

The two virtuals are exactly the calls modtools' `HUD::GameEvents::UpdateVehicleHealth`
(`0x006B48D0`) makes on `mVehicle` before it reads `mHUDModelID` (`+0x4C`) for the
seating mesh: `CALL [EDX+0x1C]` on `Controllable+0x18` at `0x006B48EF`, then
`CALL [EAX+0x28]` at `0x006B4AE3`, both thiscall with no arguments. Following that path
means a seat resolves to the same object, and so the same class, as the stock seating
mesh. The target bar calls the neighbouring overload at `+0x20`. modtools'
`EventBitmap` is `0x00698F40`: type 3 calls the bitmap's vtable `+0x50` (`+0x4C` on
Phantom), type 6 its `SetTexture(RedTexture*)`.

The two state offsets are measured from different pointers. The soldier's is from the
unit pointer the character slot holds, which is the soldier's Controllable part, `+0x240`
into the object. The droideka's is from the start of the object, the `this` of
`EntityDroideka::UpdatePilot`, which is what `Trackable::GetGameObject` returns. On
Phantom the two classes put every base at the same offset (`+0x0C`, `+0x94`, `+0x140`,
`+0x200`, `+0x240` Thread and Controllable, `+0x258` Trackable, `+0x3C8`), read from
their RTTI complete object locators.

Send on change and on the first tick after `GameEvents::Open`. Each texture event has
a Bool Disable twin, sent when there is nothing to show, so a `.hud` can show its
default icon. Mods set `HealthTexture` in soldier, hero and droideka ODFs, a stock
property the DLL does not parse, and pack every variant texture in a level the map
already loads. Texture names are global, so an icon named after its class can clash
with a model texture of the same name: in `data_BF3`, `cis_hover_aat.tga` and
`republic_hover_fightertank.tga` are the vehicles' skins.

**Nothing in the stock game reads `mHealthTexture` or `mIconTexture` after parsing**, so
setting `HealthTexture` on a soldier changes nothing by itself. A decompile of every
Phantom function whose name places it in the HUD, interface, map, lock-on or Lua
callbacks (4,707), plus everything from `0x00400000` to `0x00600000` (6,463), finds
them only in the two `EntityClass` constructors (`0x00501AE0`, `0x00501C70`) and
`EntityClass::SetProperty`. The other hits, in `EntityHologram`, `EntityCarrier` and
`EntityMine`, are mistyped pointers reading derived-class fields. `UpdateLockOn`
(`0x006148B0`) sends name, class name, team colour, health, distance, direction and
flag carrier, and no texture. The only icon field stock code uses is `mHUDModelID`, in
`UpdateVehicleHealth`.

Open: the rest of `0x00600000` to `0x00800000` (weapons, AI, network) was not scanned.

#### Weapon icons

The same module publishes `player1.weapon1.iconTexture` and `player1.weapon2.iconTexture`
(Uint) with Disable twins, from each weapon class's `IconTexture`. No stock event carries
it: the stock weapon family is `change`, `disable`, the ammo and clip counts,
`ammoInfinite`, `heat`, `charge`, `refire`, `name`, `target.*`, `reticule.*` and
`lockOn*`. Phantom `WeaponClass_data` holds `mIconTexture +0x68`, `mReticuleTexture +0x6C`
and `mScopeTexture +0x70`, so `WeaponClass+0x6C` for the icon, stored by
`WeaponClass::SetProperty` (`0x007AF120`, `MOV [EDI+0x6C]`). Unlike `EntityClass`, the
offset does not move on retail:

| | modtools | Steam | GOG |
|---|---|---|---|
| `WeaponClass::SetProperty` | `0x0061E6C0` (`this` in ESI) | `0x0067A450` (`this` in EDI) | same code as Steam |
| `IconTexture` store at `+0x6C` | `0x0061F754` | `0x0067B084` | `0x0067C124` |
| `UpdateWeaponEvents`: `GetWeaponIndex`, `GetWeapon`, `Weapon::mClass +0x64` | `0x006B2F0A` | `0x00560ADE` | `0x0056185E` |

The weapon per slot is picked exactly as `HUD::GameEvents::UpdateWeaponEvents`
(`0x00615BC0`) picks it: `GetWeaponIndex(channel)` (Controllable primary slot `+0x3C`),
`GetWeapon(index)` (`+0x40`), then `Weapon::mClass` (`layout::Weapon::kClass`), on the
same remote, else vehicle, else unit object, and re-read every update, so the icon
changes whenever the weapon or its class does. Whether a charge weapon's `mClass`
moves to `mNextCharge` while charging, which would change the icon mid-charge, is not
checked. The reads are audited by `tests/hud_class_icons_abi_tests.py` on all three
builds. The user confirmed the weapon icons in play on 2026-09-27.

### HUD shake

`CameraManager::ApplyShake(amount, duration)` (`0x00499B50`, thunk `0x00407EF0`)
appends to four slots on `mChaseCamera[0]`: in `ChaseCamera_data`, `mShakeCount +0x90`,
`mShakeAmount[4] +0x94` and `mShakeDecay[4] +0xA4`, the decay being amount / duration.
The chase camera also keeps `mPreShakeMatrix +0x44` beside `mMatrix +0x04`, and a
combined `mShake +0x88`.

Its callers are `ExplosionClass::CreateExplosion`, `EntityWalker::Render` (footfalls),
`EntityFlyer::CollisionCallback` and `EntityFlyer::PostCollisionUpdate`. A soldier
being hit or firing never shakes the camera.

Design: publish `player1.hud.shakeOffset` (Vector3, viewport fractions), the screen
displacement between `mMatrix` and `mPreShakeMatrix`, so the HUD moves exactly as the
camera does, and `player1.hud.shakeAmount` (Float). Add the soldier triggers the engine
lacks: taking damage, from the `Damageable::ApplyDamage` hook aim assist already uses,
and firing, from a rise in the weapon's `mKickSpread` (below), which needs no hook.
Scale with an INI strength. `EventPosition` replaces a group's static position, so
shaken elements go in a wrapper group resting at 0,0.

Open: whether the first-person and cockpit cameras use this shake at all.

### Spread-scaled reticle

`WeaponCannon::GetPitchSpread` (`0x007B6240`) and `GetYawSpread` (`0x007B62A0`) both
return `max(mKickSpread, 0) + mStanceSpread` plus the class's base pitch or yaw spread.
The base `Weapon::GetPitchSpread` (`0x007ADF90`) returns 0, so only cannons report a
spread. They are Weapon vtable slots 16 and 17 per
[CharacterWeaponSystem.md](CharacterWeaponSystem.md), which was read on Steam.
`mKickSpread` is the per-shot bloom that recovers over time; `mStanceSpread` follows
stance and movement.

Design: publish `player1.weaponN.spread` (Float, the angle) and
`player1.weaponN.spreadScale` (Vector3, `tan(spread) / tanHalfFov` per axis, so FOV and
zoom are already included) for `EventScale` on the reticle.

Open: the unit of the angle (the ODF values suggest degrees); the slots per build.

### Other candidates

| Idea | Data source | State |
|---|---|---|
| Heading, pitch, zoom | RedCamera matrix `+0x30`, `_fZoom +0x140` | layout verified on all three builds |
| Stance | `player1.unit.stance`, part of the class and vehicle icons above | built on modtools |
| Speed, altitude | a velocity virtual; height above ground from `engine_ray_hit` | velocity slot not derived |
| Damage direction | `Damageable::ApplyDamage`, attacker at `DamageDesc+0x04` | contract verified on all three builds; unverified on a client |
| Kill confirm | stock `player1.statistic.kills` first; else the damage hook on the host, or the kill feed on clients | untested |
| Missile direction | whatever computes `missileLockDistance` | not traced |
| Grenade warning | the live ordnance list | not traced |
| Command post markers | the strip's post list and each post object's position | strip built 2026-09-27; markers not |
| Objective waypoints | the minimap marker storage behind `map.refreshMarker`; 79 Lua files under `BF2_ModTools` call `MapAddEntityMarker` | not traced |
| Friendly name tags | `Character::sCharacters` | addressed on all three builds |
| Off-screen arrows | the screen pinning in `target_bar_geometry.hpp` | building block exists |
| Script-assigned markers | a new Lua function | host only: `On*` callbacks are stubs on clients |

---

## Open questions

- The older catalogue still has unmapped Steam/GOG functions (later sections
  above include the addresses derived for implemented features). Anchors for porting: the strings
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
