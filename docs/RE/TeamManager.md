# TeamManager object cache

The per-team registry of every team-assigned `GameObject`, and the array the AI
scans to find enemies. Addresses are modtools unless stated; Phantom twins are
given where its PDB names the symbol.

---

## The array

`TeamManager::sTeamList`, PDB-typed `ObjCacheData[8][600]` — eight teams, **600
slots each**, 4800 elements, 24 bytes apiece, 115,200 bytes total.

`ObjCacheData` (24 bytes, from Phantom's PDB):

| Offset | Size | Type | Name |
|---|---|---|---|
| 0x00 | 8 | `PblHandle<GameObject>` | `mObj` (`GameObject* mObject` +0, `int mSavedHandleId` +4) |
| 0x08 | 16 | `PblSphere` | `mSphere` (x, y, z, radius) |

| Build | `sTeamList` | `sMemberCount` (`int[8]`) | `AddObject` | `sGetObjectsInRange` |
|---|---|---|---|---|
| phantom | `0x00c50138` | `0x00c6c338` | `0x007778c0` | `0x00778050` |
| modtools | `0x00b48af8` | `0x00b48ad0` | `0x0048ef00` | `0x0048f210` |
| steam | `0x01f7d0e0` | `0x01faa430` | `0x00655050` | `0x006552d0` |
| gog | `0x01f7e390` | `0x01fab8e0` *(unverified)* | `0x006560f0` | `0x00656370` |

The 600 is read, not inferred. `TeamManager::AddObject`, modtools `0x0048ef00`:

```asm
0048ef40  MOV  ESI,[EDI+0x234]        ; team field
0048ef46  SHL  ESI,0x1c / SAR ESI,0x1c ; sign-extend the low nibble
0048ef4c  MOV  ECX,[ESI*4 + 0xb48ad0]  ; sMemberCount[team]
0048ef55  IMUL EAX,EAX,0x258           ; team * 600
0048ef5b  ADD  EAX,ECX                 ; + member index
0048ef5d  LEA  EBX,[EAX+EAX*2] / SHL EBX,0x3   ; * 24
0048ef63  MOV  [EBX + 0xb48af8],EDI    ; sTeamList[team][n].mObj.mObject
0048efb2  INC  [ESI*4 + 0xb48ad0]      ; ++sMemberCount[team]
```

`0x258` is applied **before** the `x24`, so it is unambiguously an element count
and not a byte size. Phantom's region confirms the shape with zero slack:
`0x00c50138 + 0x1c200 = 0x00c6c338`, exactly where `sMemberCount` begins.

The `0x258` / `0x3840` (600 x 24) stride is inlined at four sites — `AddObject`,
`RemoveObject`, `UpdatePositionsInternal` and `sGetObjectsInRange` — so resizing
the array means patching all four together.

---

## The AI link

`UnitController::UpdateHighLevel` -> `VisionManager::UpdatePotentiallyVisible`
(phantom `0x007a2220`) calls `TeamManager::GetObjectsInRange` at `0x007a248d`
with the unit's eye point, `GetMaxCombatRange`, and `AFFIL_ENEMY`. That walks
`sTeamList` across all eight teams and produces the AI's candidate enemy set,
which then runs `AIUtil::IsValidEnemy` -> `CanPotentiallySee` -> `MaxVisibleDist`
-> `GetVisualPriority` -> `UnitThreatManager::ShouldRaytestUnit`.

**Every enemy an AI can perceive comes out of this array.** It is not an AI
structure, though — it is a general spatial broad-phase with a dozen-odd non-AI
readers (`AttackHelper::UpdateAudience`, `sUpdateFriendlyFire`, `VehicleGetIn`,
`CheckForAutoFollow`, `WhoTwiggedMe`, `MineHelper::FindMinePos`,
`SnipeHelper::IsHintSuitable`). Describe it as *the shared per-team object cache
that AI enemy acquisition scans*, not as "the AI targeting array".

---

## `AddObject` has no bounds check

Verified by reading the whole function on modtools: there is **no `CMP` against
`0x258`** anywhere in it. The write is
`sTeamList[team][sMemberCount[team]++]`, blind.

Membership is near-universal — `GameObject::AddToTeamManager` (phantom
`0x004da650`) is `return true;` outright, and the only override found is
`FlagItem::AddToTeamManager` (`0x005bc4b0`) returning false. So every
team-assigned game object registers.

Consequences of overflowing one team's row:

- teams 0-6 spill into the **next team's** slots, silently corrupting another
  team's cache
- team 7 spills past the end of the array — into `sMemberCount` itself on the
  Phantom layout

`sMemberCount` is decremented by `RemoveObject`, so 600 is a **concurrent-live**
cap rather than a cumulative one. Whether a real high-unit-count match approaches
it has NOT been measured, and should be before this is treated as a live bug.

The team index is sign-extended from four bits (`SHL 0x1c / SAR 0x1c`), so a team
value of 8-15 would index *negatively* into `sMemberCount` and `sTeamList`.
Whether any code path can produce one was not established.

---

## 600 is a recurring AI capacity

`AIUtil::Init` (modtools `0x0058cc30`, phantom `0x004888c0`) sizes five pools at
600 in one block — read from the decompile, identical on both builds:

| Pool | Count | Element size |
|---|---|---|
| `UnitController::sMemoryPool` | 600 | `0x444` |
| `UnitAgent::sMemoryPool` | 600 | `0x358` |
| `Navigator::sMemoryPool` | 600 | `0x1e8` |
| `PathFollower::sMemoryPool` | 600 | runtime-set |
| `ConnectivityGraphFollower::sMemoryPool` | 600 | runtime-set |
| `BaseHint::sMemoryPool` | 350 (`0x15e`) | `0x40` |
| `AI::AIGoal::sMemoryPool` | 20 (`0x14`) | `0x3c` |
| `VisionManager::RayRequest::sMemoryPool` | **201** (`0xc9`) | runtime-set |

Each is a single `PUSH 0x258` before its `MemoryPool::Setup` call, so any one of
them is a one-immediate patch.

`UnitController::sMemoryPool` is the other strong reading of "600 objects related
to AI targeting": 600 controllers, each embedding an `AI::UnitThreatManager`
(204 bytes, `Threat[6]`) at `+0x1f4`.

`AIUtil::sSpied` is `ulong64[600]` — modtools `0x00b8d720`, phantom `0x00aba3c0`,
steam `0x01f9af80`, gog `0x01f9c440` — the per-character "who has detected me"
bitmask. `AIUtil::Init` zeroes it with a `0x4ae`-iteration dword copy: 8 bytes
seeded plus 4792 copied = 4800 = 600 x 8.

Ruled out by size, recorded so they are not re-checked: `VisionManager::sPVRect`
is 1200 `GameObject*` (`operator new[](0x12c0)` = 4800 bytes — the same BYTE size
as `sSpied`, which makes it look like a 600 twin and it is not);
`PathFinder::PathNode` 1024; `AIPath` 1200; `TargetManager::_gTarget[450]` is the
HUD radar, not AI.

**No `CMP` against any of these counts exists as an array bound anywhere in AI
code, on any build.** 600 is a provisioned capacity, not an enforced one.
