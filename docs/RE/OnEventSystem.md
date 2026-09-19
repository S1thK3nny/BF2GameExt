# BF2 Lua `On*` Callback System

How the stock `OnCharacterDeath` / `OnObjectKill` / `OnFlagPickUp` family actually works,
and how BF2GameExt adds its own events to it.

Re-derived from scratch on the **Phantom** build (full PDB symbols: every class, template
instantiation and method below is a real name out of the debug build, not a guess), then
ported by hand to modtools, Steam and GOG.

| Build | Status |
|---|---|
| Phantom (`Battlefront2_Phantom.exe`) | Fully symbolized. Every decompile quoted here comes from it |
| Modtools (`BF2_modtools.exe`) | All addresses derived |
| Steam (`BattlefrontII.exe`) | All addresses derived |
| GOG | Ported from Steam with `tools/port_gog.py`; every code address lands at the same VA |

> Addresses are unrelocated (imagebase `0x400000`). Resolve with
> `addr - 0x400000 + GetModuleHandleW(nullptr)`.

---

## The shape of the thing

The whole system is one C++ template family in namespace `EventManager`, instantiated
16 times over `<T, A>` pairs. There is no table of event names, no dispatcher switch, no
per-event handwritten C function. Each event is a **static `Event<T,A>` object carrying its
own name**, and every Lua global is generated from that name at registration time by
`snprintf`.

```
EventManager::Event<T,A>              a named signal, statically allocated in .data
EventManager::Node                    base class: refcounted tree node (PblRef + parent/child/sibling)
EventManager::LuaCallback<T,A>        a leaf Node holding one luaL_ref'd Lua function
EventManager::Filter<Matcher,Event>   an interior Node that gates its whole subtree

EventManager::RegisterLuaCallback<T,A>[,Matcher]    the Lua C function behind On<Name>[Suffix]
EventManager::UnregisterLuaCallback<T,A>            the Lua C function behind Release<Name>
EventManager::RegisterRegisterLuaCallback<...>      installs the above under its generated name
EventManager::RegisterUnregisterLuaCallback<...>    installs Release<Name>
EventManager::LuaPushItem<X>                        the per-type argument marshaller
EventManager::LuaGetItem<Matcher>                   the per-matcher filter-argument reader
```

The listener set is a **tree**, not a list. That single fact is what the old notes in this
file got wrong, and it is what makes hand-rolled dispatch unsafe (see
[What the previous notes got wrong](#what-the-previous-notes-got-wrong)).

---

## `Event<T,A>` object layout — 0x18 bytes

Verified identical on Phantom, modtools, Steam and GOG.

| Offset | Field | Notes |
|---|---|---|
| `+0x00` | `const char* mName` | e.g. `"CharacterEnterVehicle"`. Statically initialized |
| `+0x04` | vptr | Start of the embedded `Node`. **`Node*` == `Event* + 4`** |
| `+0x08` | `uint16_t` PblRef word | count in the high 14 bits, low 2 bits are flags. Live value `7` = count 1, flags `0b11` |
| `+0x0C` | `Node* mParent` | always null for an Event |
| `+0x10` | `Node* mChild` | listener tree root |
| `+0x14` | `Node* mSibling` | always null for an Event |

`Node` is therefore `{ vptr, refword, mParent, mChild, mSibling }` = 0x14 bytes, and
`Event` is `const char* mName` followed by that `Node`.

`LuaCallback<T,A>` is a `Node` plus one field:

| Offset | Field |
|---|---|
| `+0x00` .. `+0x10` | `Node` |
| `+0x14` | `int mFunction` — `luaL_ref` index into `LUA_REGISTRYINDEX`, `-1` = none |

Size 0x18 (`operator delete(this, 0x18)` in its deleting destructor).

`Filter<Matcher,Event>` is also a `Node` plus one field: the stored filter value at `+0x14`
(a team index, a name hash, or an `EntityClass*` depending on the matcher).

### Node vtable

| Slot | Meaning |
|---|---|
| `+0x00` | `PblRef::Kill` |
| `+0x04` | scalar deleting destructor |
| `+0x08` | type tag getter — returns `"Name"` / `"Team"` / `"Class"` for filter nodes, used for filter dedupe |
| `+0x0C` | `operator()(arg0, arg1)` — the dispatch/predicate. Base `Event`/`Callback` returns `true` |

Slot `+0x0C` is the fire hook: for a `LuaCallback` it pushes and calls the Lua function and
returns `true`; for a `Filter` it evaluates the matcher and returns whether the subtree should
be visited; for the `Event` itself it is a stub that returns `true`.

---

## Registration: `On<Name>` is generated, never written

`EventManager::RegisterRegisterLuaCallback<T,A>` (Phantom `0x005A2F50`, the `<Character,char>`
instantiation) is the whole story:

```c
void RegisterRegisterLuaCallback<Character,char>(Event* ev, lua_CFunction registrar)
{
    char name[0x3f + 1];
    snprintf(name, 0x3f, "On%s", ev->mName);       // <- ev->mName is [ESI], i.e. Event+0
    lua_pushlightuserdata(LuaHelper::mState, ev);  // <- becomes upvalue 1

    char net = netEnabled;
    if (netInShell) net = netEnabledNext;
    if (net && netEnabled && netOnClient) {        // multiplayer client
        LuaHelper::Register(name, DummyLuaCallback, 0);
        return;
    }
    LuaHelper::Register(name, registrar, 1);       // 1 upvalue = the Event*
}
```

Four sibling templates exist:

| Template | Format string | Produces |
|---|---|---|
| `RegisterRegisterLuaCallback<T,A>` | `"On%s"` | `OnCharacterDeath` |
| `RegisterRegisterLuaCallback<T,A,Matcher>` | `"On%s%s"` | `OnCharacterDeathName` / `…Team` / `…Class` |
| `RegisterRegisterLuaCallback<T,A,M1,M2>` | `"On%s%s%s"` | region events, which always carry a region matcher plus an optional second one |
| `RegisterUnregisterLuaCallback<T,A>` | `"Release%s"` | `ReleaseCharacterDeath` |

The second `%s` is the matcher's static tag string, `"Name"` / `"Team"` / `"Class"`
(Phantom `0x009F7DE0` = `"Name"`).

Two consequences worth internalising:

* **The Lua names are a pure function of `mName`.** Nothing else knows the name. Give an
  `Event` object a name and register it, and `On<Name>`, `On<Name>Name`, `On<Name>Team`,
  `On<Name>Class` and `Release<Name>` all exist, spelled correctly, with no per-event code.
* **MP clients get `DummyLuaCallback` instead.** On a networked client every `On*` global is
  a no-op stub that silently accepts the callback and never fires. This is the engine's own
  enforcement of "events are host-only", and any custom event that goes through this path
  inherits it for free. See [[mp_scripting_constraints]].

### `On<Name>(func)` — what the registrar does

`EventManager::RegisterLuaCallback<Character,char>`, Phantom `0x005A1690`:

```c
int RegisterLuaCallback<Character,char>(lua_State*)
{
    Event* ev = lua_topointer(mState, -10002);      // lua_upvalueindex(1)
    if (lua_type(mState, 1) != 6) {                 // 6 = LUA_TFUNCTION
        lua_pushnil(mState);
        return 1;
    }
    lua_pushvalue(mState, 1);
    int ref = luaL_ref(mState, -10000);             // LUA_REGISTRYINDEX
    auto* cb = new LuaCallback<Character,char>(ref);
    Node::AttachToParent(cb, (Node*)((char*)ev + 4));
    luaL_unref(mState, -10000, ref);
    lua_pushlightuserdata(mState, cb);              // the release handle
    PblRef::Release(cb);
    return 1;
}
```

Note the handle handed back to Lua is the `LuaCallback` node itself, and the node is the
only owner of the Lua reference.

### `On<Name>Team(func, team)` — the filter node

The filtered registrars do the same thing with one extra step, and this is the part that
makes the listener set a tree:

```c
LuaGetItem<CharacterMatchesTeam<char>>(&filterValue);   // read arg 2
Node* f = ev->mChild;                                    // scan existing children
while (f) {
    if (f->vtable[2]() == "Team" && f->value == filterValue) {
        f->addref();
        goto attach;                                     // reuse the existing filter node
    }
    f = f->mSibling;
}
f = new Filter<CharacterMatchesTeam,…>(filterValue);
Node::AttachToParent(f, &ev->mNode);
attach:
Node::AttachToParent(new LuaCallback<T,A>(ref), f ? f : &ev->mNode);
```

So `OnCharacterDeathTeam(fn, 1)` called five times produces **one** `Filter` node with five
`LuaCallback` children, and the team test runs once for all five. Unfiltered callbacks hang
directly off the event.

### `Release<Name>(handle)`

`EventManager::UnregisterLuaCallback<T,A>`, Phantom `0x005A3E50`:

```c
int UnregisterLuaCallback<Character,char>(lua_State*)
{
    Event* ev = lua_topointer(mState, -10002);
    if (lua_type(mState, 1) == 2) {              // 2 = LUA_TLIGHTUSERDATA
        Callback* cb = lua_topointer(mState, 1);
        if (cb) Event<Character,char>::Unregister(ev, cb);
    }
    return 0;
}
```

`Event::Unregister` detaches the callback, then walks up detaching any filter nodes that
were left childless. Filter nodes are reference-counted and garbage-collected on the way out;
nothing leaks and nothing is orphaned.

---

## Dispatch: `Node::Trigger`

One non-template function does every broadcast, because the walk only touches `Node` fields
and virtuals.

```
modtools  0x00661F10      (thunk 0x00404A57)
Steam     0x004B9820
GOG       0x004B9820
__thiscall void Node::Trigger(Node* this, void* arg0, void* arg1)
```

```c
this->addref();
if (this->vtable[3](arg0, arg1)) {              // predicate / Lua call
    Node* c = this->mChild;                      // this+0xC
    if (c) {
        c->addref();
        do {
            if (!(c->refword & 2)) { c->release(); break; }   // node died mid-walk
            Node* next = c->mSibling;                          // c+0x10
            if (next) next->addref();
            Trigger(c, arg0, arg1);                            // recurse
            c->release();
            c = next;
        } while (c);
    }
}
this->release();
```

Every node visited is add-ref'd across its own dispatch and the sibling is pinned before
recursing, so a Lua callback may legally call `Release<Name>` on itself or on a sibling from
inside the callback without the walk stepping on freed memory. Release drives the refcount to
zero and calls vtable `+0x04`, the deleting destructor.

**Callers pass `&event.mNode`, i.e. `Event* + 4`.** The firing site for
`CharacterEnterVehicle` in `EntitySoldier::EnterControllable` (modtools `0x00544B37`):

```asm
00544af1  8B 43 18        MOV  EAX,[EBX+0x18]
00544af4  8D 4B 18        LEA  ECX,[EBX+0x18]
00544af7  FF 50 20        CALL [EAX+0x20]          ; -> GameObject* for the vehicle
00544afa  8A 15 ...       MOV  DL, netOnClient
00544b00  8B 8E 0C030000  MOV  ECX,[ESI+0x30C]     ; Character* from EntitySoldier+0x30C
...       84 D2 75 12     TEST DL,DL / JNZ         ; MP client: skip the whole fire
00544b37  50              PUSH EAX                 ; arg1
00544b38  51              PUSH ECX                 ; arg0
00544b39  B9 18F4AD00     MOV  ECX, 0x00ADF418     ; &mEnterVehicle + 4
00544b3e  E8 ...          CALL Node::Trigger
```

Two reusable facts fall out of that: **`EntitySoldier + 0x30C` is the soldier's `Character*`**
(same offset on Steam), and the engine's own firing sites skip the broadcast entirely on a
networked client rather than relying on `DummyLuaCallback` alone.

### Argument marshalling

`LuaCallback<T,A>::operator()` (Phantom `0x0040AA9C` for `<Character,char>`):

```c
bool operator()(T* a0, A* a1)
{
    if (mFunction == -1) return false;
    if (!LuaHelper::PushRefProc(mFunction)) return false;
    if (!LuaPushItem<T>(a0))  { lua_settop(mState, -2); return false; }
    if (!LuaPushItem<A>(a1))  { lua_settop(mState, -3); return false; }
    LuaHelper::CallProc(0);
    return true;
}
```

`LuaPushItem<X>` is the only place a type becomes a Lua value:

| `X` | Pushed as |
|---|---|
| `Character` | **number** — `(ptr - Character::sCharacters) / 0x1B0`, i.e. the character index every `GetCharacter*` takes |
| `GameObject`, `CommandPost`, `FlagItem`, `Team`, `RedRegion`, `Timer`, `DamageOwner` | light userdata |
| `int`, `float`, `unsigned __int64`, `enum AI::EventType` | number |
| `char const*` | string |
| `bool` | boolean |
| `char` | the "no payload" filler used by events whose second argument carries nothing |

---

## Lifecycle

```
GameLoop::Init    -> EventManager::Init()       modtools 0x00798220   Steam/GOG 0x0050B3A0
GameLoop::Cleanup -> EventManager::Cleanup()    modtools 0x00798260   Steam/GOG 0x0050B3D0
```

`EventManager::Init` is **per mission load**, not once per process — it sits in `GameLoop::Init`
alongside `Team::CreateAll`, `SpawnManager::Create` and every display's `Create`. That is why
the registrations land in the mission's fresh Lua state, and it is the correct place to add
our own.

It calls nine family initialisers on modtools (eight on Steam/GOG):

```
CommandPostEvent::Init   ControllableEvent::Init   FlagEvent::Init
LuaUserEvent::Init       ObjectEvent::Init         CharacterEvent::Init
RegionEvent::Init        TeamEvent::Init           TimerEvent::Init
```

Each is a flat list of registrar calls, five per event. `EventManager::CharacterEvent::Init`
(modtools `0x0079D750`) in full:

```c
RegisterUnregisterLuaCallback<Character,DamageOwner>(&mDeath, …);   // ReleaseCharacterDeath
RegisterRegisterLuaCallback  <Character,DamageOwner>(&mDeath, …);   // OnCharacterDeath
RegisterRegisterLuaCallback  <…,CharacterMatchesName <DamageOwner>>(&mDeath, …);
RegisterRegisterLuaCallback  <…,CharacterMatchesTeam <DamageOwner>>(&mDeath, …);
RegisterRegisterLuaCallback  <…,CharacterMatchesClass<DamageOwner>>(&mDeath, …);
… the same five for mSpawn, mChangeClass, mDispensePowerup, mDispenseControllable,
  mLandedFlyer, mEnterVehicle, mIssueAICommand
```

`EventManager::Cleanup` mirrors it and ends at a single helper that detaches every child of
one event:

```
__fastcall void Event::Cleanup(Node* this)   // this = Event + 4
    while (this->mChild) Node::DetachFromParent(this->mChild);

modtools 0x007981F0    Steam/GOG 0x0050B380
```

Detaching the last reference destroys the `LuaCallback`, whose destructor calls
`luaL_unref(LuaHelper::mState, LUA_REGISTRYINDEX, mFunction)`. **This has to run while the Lua
state that issued the ref is still alive**, which is exactly why cleanup is bound to
`GameLoop::Cleanup` and not deferred to the next `Init`. Custom events must observe the same
rule or they will unref stale indices into a fresh registry.

---

## Stock event inventory

Names as they appear in `mName`; the Lua globals are `On<Name>`, `On<Name>Name`,
`On<Name>Team`, `On<Name>Class` and `Release<Name>`.

| Family | Events |
|---|---|
| `CharacterEvent` | `CharacterDeath`, `CharacterSpawn`, `CharacterChangeClass`, `CharacterDispensePowerup`, `CharacterDispenseControllable`, `CharacterLandedFlyer`, `CharacterEnterVehicle`, `CharacterIssueAICommand` |
| `ObjectEvent` | `ObjectCreate`, `ObjectInit`, `ObjectDamage`, `ObjectRepair`, `ObjectHack`, `ObjectKill`, `ObjectHeadshot`, `ObjectRespawn`, `ObjectDelete`, `TeamChange`, `HealthChange`, `ShieldChange` |
| `CommandPostEvent` | `BeginNeutralize`, `AbortNeutralize`, `FinishNeutralize`, `BeginCapture`, `AbortCapture`, `FinishCapture`, `CommandPostKill`, `CommandPostRespawn` |
| `FlagEvent` | `FlagPickUp`, … |
| `TeamEvent` | `TicketCountChange`, `TicketBleedChange`, `PointsChange`, `KillsChange`, `DeathsChange`, `ScoreChange` |
| `RegionEvent`, `TimerEvent`, `ControllableEvent`, `LuaUserEvent` | region enter/exit, timer elapse, and the script-created event family |

**There is no `CharacterExitVehicle`.** The engine fires `CharacterEnterVehicle` and stops
there; the exit side was never wired up. That gap is the reason BF2GameExt has its own.

`Event<T,A>` instantiations that exist, i.e. the payload shapes available to a new event:
`<Character,char>`, `<Character,DamageOwner>`, `<Character,GameObject>`,
`<Character,enum AI::EventType>`, `<CommandPost,char>`, `<CommandPost,DamageOwner>`,
`<CommandPost,unsigned __int64>`, `<FlagItem,char>`, `<FlagItem,Character>`,
`<GameObject,char>`, `<GameObject,int>`, `<GameObject,float>`, `<GameObject,DamageOwner>`,
`<Team,int>`, `<Team,float>`, `<RedRegion,Character>`, `<Timer,char>`,
`<LuaObject,LuaObject>`.

---

## Adding a custom event from the DLL

The registrars take the `Event*` and read everything they need out of it. Nothing checks that
the object lives in the exe's `.data`, so a correctly shaped 0x18-byte struct in the DLL is
indistinguishable from a stock event. Recipe:

1. **Pick a family** whose `<T,A>` matches the payload. `CharacterExitVehicle` wants
   `(Character*, GameObject*)`, so it borrows `<Character,GameObject>` — the same family as
   `CharacterEnterVehicle`.
2. **Build the object**: `mName` = your name, vptr **copied at runtime from the stock template
   event of that family** (never hardcode the vtable VA; read `*(void**)(templateEvent + 4)`),
   refword = `7`, the three `Node*` null.
3. **Call the five registrars** from a detour on `EventManager::Init`, after the original. They
   are per-family, so the same five serve any number of custom events in that family.
4. **Fire** with `Node::Trigger(&ev + 4, a0, a1)` from wherever the engine actually does the
   thing.
5. **Clean up** with `Event::Cleanup(&ev + 4)` from a detour on `EventManager::Cleanup`, before
   the original.

Everything else — the generated names, the filter tree, dedupe, refcounting, reentrancy,
`Release`, `luaL_ref` bookkeeping, MP client stubbing, character-index marshalling — is the
engine's and comes for free.

### Calling conventions differ between builds

The modtools build compiles the registrars as `__cdecl(Event*, lua_CFunction)`. The retail
LTCG builds saw that each call site always passed the same function pointer, folded it into
the body, and converted the result to `__fastcall(Event* ECX)`. So the modtools side needs the
callback function pointers too, and retail does not.

### Address table

`<Character,GameObject>` family. Order is Release, On, OnName, OnTeam, OnClass.

| | modtools | Steam | GOG |
|---|---|---|---|
| `EventManager::Init` | `0x00798220` | `0x0050B3A0` | `0x0050B3A0` |
| `EventManager::Cleanup` | `0x00798260` | `0x0050B3D0` | `0x0050B3D0` |
| `Node::Trigger` | `0x00661F10` | `0x004B9820` | `0x004B9820` |
| `Event::Cleanup(Node*)` | `0x007981F0` | `0x0050B380` | `0x0050B380` |
| template event (`mEnterVehicle`) | `0x00ADF414` | `0x007EB980` | `0x007EC970` |
| registrar: `Release%s` | `0x0079A530` | `0x0050CF90` | `0x0050CF90` |
| registrar: `On%s` | `0x0079A640` | `0x0050D020` | `0x0050D020` |
| registrar: `On%s` + `Name` | `0x0079A810` | `0x0050D1D0` | `0x0050D1D0` |
| registrar: `On%s` + `Team` | `0x0079A8D0` | `0x0050D3E0` | `0x0050D3E0` |
| registrar: `On%s` + `Class` | `0x0079A990` | `0x0050D5F0` | `0x0050D5F0` |
| modtools-only callback fns | `0x004109D3` / `0x004135D9` / `0x0041253A` / `0x004061B8` / `0x0040685C` | folded in | folded in |

GOG code addresses were ported from Steam with `tools/port_gog.py code`; every one landed at
the same VA (shift `+0x0`). The event global was ported with `port_gog.py data`, five agreeing
reference sites.

Implementation: `PatcherDLL/src/lua/lua_events.cpp`.

---

## What the previous notes got wrong

The earlier revision of this file was written before the Phantom symbols were in play. It had
the 0x18 size, the name-at-`+0`, the five generated globals and the modtools registrar and
callback addresses right. The rest was wrong in ways that mattered:

| Claim | Reality |
|---|---|
| `+0x08` is an "Event ID integer (0 for custom events)" | It is the `PblRef` count/flags word. A custom event created with `0` there has refcount 0, and the first `Node::Trigger` decrements it to zero and invokes the deleting destructor on a static object |
| `+0x10` is `listHead`, `+0x14` unknown | `+0x0C` `mParent`, `+0x10` `mChild`, `+0x14` `mSibling` |
| Listeners are a null-terminated singly-linked list walked through `node+0x0C` | Listeners are a **tree**. `+0x0C` is the parent pointer; siblings are at `+0x10`. Walking `+0x0C` as "next" walks upward into the event itself |
| The Lua ref is at `node+0x14` for every node | Only for `LuaCallback` leaves. On a `Filter` node `+0x14` is the filter value, and reading it as a registry index feeds garbage to `lua_rawgeti`. Any script using a `Name`/`Team`/`Class` variant creates such a node |
| vtable slot 3 "is a type identifier, not the fire/dispatch function" | Slot 3 **is** `operator()`, the dispatch |
| Fire by hand-walking the list and `lua_pcall`ing each ref | `Node::Trigger` exists, is refcount-correct and reentrancy-safe, and is what the engine uses |

The hand-rolled walk the old doc prescribed would have worked for plain `On<Name>` callbacks
and corrupted the Lua stack the moment a script used a filtered one.
