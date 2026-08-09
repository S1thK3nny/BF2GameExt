# Ghidra Fixup Plan — BF2_modtools.exe

## Overview

This document captures everything needed to fix up the `BF2_modtools.exe` Ghidra project:
struct definitions, inheritance hierarchy, this-pointer adjustments, known addresses,
and the ultimate goal — enabling flyer weapon firing in landing regions.

The main retail exe has RTTI partially set up; the modtools exe has none. We need to
apply struct definitions from the PDB, fix inheritance, and correct decompiler output.

---

## Target Binary

- **Executable**: `BF2_modtools.exe`
- **Imagebase**: `0x400000` (unrelocated)
- **Address resolution**: `resolved = (unrelocated_addr - 0x400000) + runtime_base`
- **PDB source**: `/Battlefront2.pdb/` (from retail build — offsets may need verification against modtools binary)

---

## Class Hierarchy (from PDB + RE)

```
GameObject                    (0x240 bytes, offset 0x000 in EntityFlyer)
├── EntityGeometry            (312 bytes)
├── Damageable                (192 bytes)
├── PblHandled                (8 bytes)
├── PblList<Controllable>     (16 bytes)
├── classUnion                (4 bytes)
├── mNetUniqueId              (4 bytes)
├── mPrevFoliageObj           (4 bytes)
├── GameSoundControllable     (4 bytes)
├── PblHandle<FLEffectObject> (8 bytes)
├── mCreatedTime              (4 bytes)
└── bitfields: mTeam, mOwningTeam, mPerceivedTeam, mHealthTypeForLockOn, mIsTargetable

Controllable                  (0x18C = 396 bytes, offset 0x240 in EntityFlyer)
├── Thread                    (24 bytes)
├── Trackable                 (32 bytes)
├── mControlFire[2]           (Trigger, 8 bytes)
├── mControlReload..Sprint    (Trigger × 5)
├── mControlZoom/View/etc     (Trigger × 4)
├── mControlThrustFwd/Back    (Trigger × 2)
├── mControlStrafeL/R         (Trigger × 2)
├── mTriggerControlSwitch[2]  (Trigger, 8 bytes)
├── mControlMove/Strafe/Turn/Pitch (float × 4)
├── mControlSwitch[2]         (float, 8 bytes)
├── mWpnChannel               (int)
├── mAIThread                 (28 bytes)
├── mNode                     (12 bytes)
├── mCtrl, mCharacter, mPilot (pointers)
├── mPlayerId, mInitialPlayerId (int)
├── mEyePoint, mEyeDir       (PblVector3 × 2)
├── mTurnBuildup, mPitchBuildup (float × 2)
├── mIndirectControlFlag, mUsingTurnAdjusted, mPlayAnim (bool × 3)
├── mAnimNameId               (uint)
├── mWpnDistFactor            (float)
├── mCollisionObj             (PblHandle<GameObject>, 8 bytes)
├── mEnemyLockedOnMeState/Timestamp/Missile/Distance
├── mTurnAdjusted             (float)
├── mPitchAdjusted            (float, offset 0x124/292 in Controllable)  ← FLIGHT STATE in EntityFlyer
├── mTurnAuto                 (float)
├── mPitchAuto                (float)
├── mLockBreakTimer           (float)
├── mInCaptureRegionTimestamp  (float)
├── mTargetLockedObj           (PblHandle, 8 bytes)
├── mAllowedLockTypes          (int)
├── mPilotType                 (PilotType, 4 bytes)
├── mTargetInfo                (TargetInfo, 28 bytes)
├── mReticuleTarget[2]         (PblHandle × 2, 16 bytes)
├── mAIReticuleTarget[2]       (PblHandle × 2, 16 bytes)
└── mReticuleAffiliation[2]    (int × 2, 8 bytes)

EntityPathFollower            (0xE0 = 224 bytes, offset 0x3CC in EntityFlyer)
├── EntitySquadron            (52 bytes)
├── PblList<EntityPathFollower>::Node (16 bytes)
├── mNode                     (8 bytes)
├── mSpeed/mOffPathSpeed/mCurSpeed/mAcceleration (float × 4)
├── mYaw/mSplineRoll/mCurRoll/mCurRollAcceleration/mRollVelocity (float × 5)
├── mStrafeTimer/mT           (float × 2)
├── mPathSpline               (PblCatmullRom, 48 bytes)
├── mPath                     (EntityPath*)
├── mIndex                    (int)
├── mDirection/mSetPoints     (char/uchar)
├── mStaredownTimer/mFireTimer/mBoostTimer (float × 3)
├── bool flags: mbTeleportNow, mbQueuePoint, mbGoMinSpeed, mbDelayIndexIncrement,
│               mbCirclingPoint, mbLandNow, mbStraightLine
├── mLastUsedID               (uint)
└── drawcolor                 (RedColor)

VehicleMusic                  (0x64 = 100 bytes, offset 0x4AC in EntityFlyer)
VehicleEngine                 (0x68 = 104 bytes, offset 0x510 in EntityFlyer)
```

### EntityFlyer — Full Field Layout

```
Offset  Size  Type                          Name
------  ----  ----                          ----
0x000   0x240 GameObject                    (base class)
0x240   0x18C Controllable                  (base class)
0x3CC   0x0E0 EntityPathFollower            (base class)
0x4AC   0x064 VehicleMusic                  VehicleMusic
0x510   0x068 VehicleEngine                 mVehicleEngine
0x578   0x008 PblHandle<GameObject>         mTowCableWalker
0x580   0x00C PblVector3                    mVelocity
0x58C   0x00C PblVector3                    mOmega
0x598   0x004 float                         mSetSpeed
0x59C   0x004 float                         mSetStrafe
0x5A0   0x004 float                         mSetStrafeVertical
0x5A4   0x004 State                         mState              ← State enum
0x5A8   0x004 float                         mFlightRatio
0x5AC   0x004 float                         mBankPitchAngle
0x5B0   0x004 float                         mBankPitchAngleDelta
0x5B4   0x004 float                         mBankRollAngle
0x5B8   0x004 float                         mBankRollAngleDelta
0x5BC   0x004 float                         mRoll
0x5C0   0x004 float                         mRollTarget
0x5C4   0x004 float                         mFlip
0x5C8   0x004 float                         mFlipTarget
0x5CC   0x004 float                         mTrickCameraInterpDetach
0x5D0   0x004 float                         mTrickCameraInterpDir
0x5D4   0x004 float                         mTrickCameraDetach
0x5D8   0x00C PblVector3                    mSideRoll
0x5E4   0x004 Trick                         mLastTrickPerformed
0x5E8   0x004 float                         mPitchControllerPrevUpdate
0x5EC   0x004 float                         mTurnControllerPrevUpdate
0x5F0   0x004 float                         mReserveForPlayer
0x5F4         bool:1 bits                   FLAGS BITFIELD:
              bit 0                           mInRoll
              bit 1                           mInFlip
              bit 2                           mBoost
              bit 3                           mFollowingPath
              bit 4                           mSliding
              bit 5                           mInLandingRegionFlag      ← KEY for task
              bit 6                           mInLandingRegionFlag_ToolTip
              bit 7                           mFlyerAutoCorrectingToStayInBattlefield
0x5F8   0x004 float                         mGetSpeedSpeed
0x5FC   0x004 float                         mInLandingRegionFactor
0x600   0x004 float                         mLandedHeight
0x604   0x004 float                         mAutoLevelingAllowed
0x608   0x004 float                         mWorldRoll
0x60C   0x004 float                         mCrashTimer
0x610   0x004 float                         mTrick
0x614   0x004 float                         mTrickDoubleclick
0x618   0x00C EnergyBar                     mEnergyBar
0x624   0x00C PblVector3                    mAmbientShift
0x630   0x00C PblVector3                    mAmbientShiftTarget
0x63C   0x004 float                         mAmbientShiftSpeedMax
0x640   0x00C PblVector3                    mAmbientShiftSpeed
0x64C   0x004 RedAnimation*                 mAnimObj
0x650   0x004 float                         mNextBarrierCheck
0x654   0x00C PblVector3                    mSlidePos
0x660   0x00C PblVector3                    mSlideBeginPos
0x66C   0x004 EntityFlyerClass*             mClass                ← class definition pointer
0x670   0x004 PassengerSlot[4]*             mPassengerSlots
0x680   0x004 MountedTurret[8]*             mTurret
0x6A0   0x001 char                          mNumTurrets
0x6A4   0x004 float                         mAnimationTime
0x6A8   0x004 Aimer[4]*                     mAimer
0x6B8   0x004 Weapon[4]*                    mWeapon
0x6D4   0x008 int[2]                        mWeaponIndex
0x6DC   0x004 float                         mLastAltitude
0x6E0   0x810 ZephyrSkeleton<32>            mZephyrSkeleton
0xEF0   0x9B0 ZephyrPoseDyn<32>             mZephyrPoseDyn
0x18A0  0x408 RedPose                       mPose
0x1CA8  0x028 SoundParameterized            mSoundEngine
0x1CD0  0x004 float                         mSpeedPrev
0x1CD4  0x004 float                         mTurbulance
0x1CD8  0x004 float                         mLastDistanceToTarget
0x1CDC  0x020 AIEng                         mAIEng
0x1CFC  0x004 Obstacle*                     mObstacle
0x1D00  0x004 float                         mTotalUpdateDt
0x1D04  0x001 bool                          mJustRendered
0x1D08  0x004 float                         mTrickEngineValue
0x1D0C  0x004 GameSoundControllable         mSoundTrick
0x1D20  0x040 PblHandle<ParticleEmitterObject>[8]   mThrustEffect
0x1D60  0x060 PblHandle<ParticleEmitterObject>[12]  mContrailEffect
```

**Total EntityFlyer size**: ~0x1DD0 bytes

---

## Critical: this-Pointer Adjustment in EntityFlyer::Update

`EntityFlyer::Update` receives `this` (ECX) pointing to the **Controllable subobject**
at EntityFlyer + 0x240, NOT the EntityFlyer base. This is a C++ multiple inheritance
vtable thunk adjustment.

### Evidence

| Ghidra reference | Raw offset from this | + 0x240 = EntityFlyer offset | Real field |
|---|---|---|---|
| `field_0x42c` | 0x42C | **0x66C** | `EntityFlyerClass* mClass` |
| `field_0x488` | 0x488 | **0x6C8** | near `mWeapon` area |
| `field_0x478` | 0x478 | **0x6B8** | `Weapon[4]* mWeapon` |
| `field_0x490` | 0x490 | **0x6D0** | within mWeapon array |
| `field_0x3cc` | 0x3CC | **0x60C** | `float mCrashTimer` |
| `field_0x3dc` | 0x3DC | **0x61C** | within `EnergyBar` |
| `field_0x3d8` | 0x3D8 | **0x618** | `EnergyBar mEnergyBar` |
| `field_0x460` | 0x460 | **0x6A0** | `char mNumTurrets` |

**Controllable fields** (like `mPitchAdjusted`, `mTurnAuto`, `mControlSwitch`, etc.)
are accessed correctly since `this` already points at the Controllable subobject.

The expression `this[-1].mPose._uiTable + 0xbb` that appears throughout the decompilation
is the code computing the **EntityFlyer base pointer** by reaching backward from the
Controllable subobject.

### Ghidra Fix

Either:
1. Retype `this` as `Controllable *` — Controllable fields resolve correctly, other
   EntityFlyer fields need +0x240 mental offset
2. Or create a custom struct starting at the Controllable offset that includes all the
   EntityFlyer fields from 0x240 onward (preferred — gives all fields correct names)

---

## Controllable.mPitchAdjusted — Flight State Machine

The field `Controllable.mPitchAdjusted` at Controllable + 0x124 (EntityFlyer + 0x364)
is a `float` in the PDB but stores **integer state values** as their float bit patterns.

| Ghidra float | Int bits | Hex | State | Notes |
|---|---|---|---|---|
| `0.0` | 0 | 0x00000000 | **LANDED** | SendLandedEvent, TurnOffThrust |
| `1.4013e-45` | 1 | 0x00000001 | **TAKEOFF** | Ascending, terrain height checks |
| `2.8026e-45` | 2 | 0x00000002 | **FLYING** | Main flight, AI path follower |
| `4.2039e-45` | 3 | 0x00000003 | **LANDING** | Descending, dust effect |
| `5.60519e-45` | 4 | 0x00000004 | **DISABLED** | Post-destruction |
| `7.00649e-45` | 5 | 0x00000005 | **CRASHED** | Timer expiry |

State transitions: `FLYING(2) → LANDING(3) → LANDED(0) → TAKEOFF(1) → FLYING(2)`

---

## EntityFlyerClass Flags

`mClass` (EntityFlyerClass*) at EntityFlyer + 0x66C.

The byte at `mClass + 0x1118` contains flags:
- **Bit 1** (0x01): Used in controllable/weapon count logic
- **Bit 2** (0x02): When SET → prevents FLYING→LANDING transition AND disables weapons
  even when not landed. When CLEAR → normal landing and weapon behavior.

---

## The Weapon Gating Mechanism (The Fix Target)

In `EntityFlyer::Update`, at decompilation lines 1105-1141, weapon firing is gated:

```c
// Condition (from this = Controllable*):
//   mPitchAdjusted at this + 0x124
//   mClass at this + 0x42C (= EntityFlyer + 0x66C)
//   mClass[0x1118] & 2 = class flag
if ((mPitchAdjusted != 0  /* not LANDED */) &&
    ((mClass[0x1118] & 2) == 0  /* class flag clear */))
    || (collision_fallback_condition) {
    // PATH A: weapons ENABLED
    //   → GetControllableList() → process weapons → allow fire
} else {
    // PATH B: weapons DISABLED
    //   → GetJoystickIndex() → thunk_FUN_00774440()
    //   → weapon vtable[0x70] (stop) or vtable[0x68](1)
}
```

### What Prevents Shooting When Landed

When `mPitchAdjusted == 0` (LANDED state), the first term `mPitchAdjusted != 0` is false.
The entire AND expression fails. If the collision fallback also fails, Path B executes
and weapons are disabled.

### The Fix

To allow shooting when landed **in a landing region**, modify the condition so that
`mPitchAdjusted == 0` still allows Path A when `mInLandingRegionFlag` (EntityFlyer + 0x5F4,
bit 5) is set.

**Assembly target**: Find the `cmp [reg + 0x124], 0` / `jz` instruction pair in
`EntityFlyer::Update` (function range ~0x4FC000–0x5012F1) near calls to
`GetControllableList` (Path A) and `thunk_FUN_00774440` (Path B).

Options:
1. **NOP patch**: Change the conditional jump to always take Path A (allows weapons in
   ALL landed states, not just landing regions)
2. **Code cave / Detours hook**: Replace the conditional jump with a jump to injected
   code that checks `mInLandingRegionFlag` and conditionally allows Path A
3. **Lua-toggled patch**: Like SetBarrelFireOrigin — a Lua function that patches/unpatches
   the instruction at runtime

---

## Known Addresses (BF2_modtools.exe, unrelocated base 0x400000)

### Global Data
| Address | Type | Label | Notes |
|---|---|---|---|
| 0xB93A08 | void** | g_mCharacterStructArray | Character slot array base |
| 0xB939F4 | int | g_MaxCharacterCount | Max valid charIndex |
| 0xAD5D64 | void** | g_ppTeams | Team array pointer (double deref) |
| 0xACD2C8 | void* | g_ClassDefList | Global class def linked list head |
| 0xB35A58 | lua_State** | g_lua_state_ptr | Pointer to global Lua state |
| 0xACE360 | PblVector3[3] | sEyePointOffset | Stance offsets (stand/crouch/prone) |
| 0xACE384 | | sEyePointRelativeWeaponOffset | |

### Functions
| Address | Label | Signature |
|---|---|---|
| 0x7E3D50 | GameLog | `void __cdecl (const char* fmt, ...)` |
| 0x7E1BD0 | HashString | `void* __thiscall (void* buf8, const char* name)` |
| 0x486660 | LuaHelper::InitState | `void __cdecl ()` — hooked by BF2GameExt |
| 0x5EE9D0 | Aimer::SetSoldierInfo | `void (Aimer*, PblVector3* pos, PblVector3* dir)` |
| 0x5E6F70 | Controllable::SetWeaponIndex | `void __thiscall (int slot, int channel)` |
| 0x5E7090 | Controllable::GetCurWpn | Float-priority list traversal |
| 0x4DBCF0 | Controllable::GetActiveWeaponChannel | Stub: xor eax,eax; ret |
| 0x5E7100 | Controllable::GetWpnChannel | Pure virtual (all INT3) |
| 0x61CEE0 | Weapon::OverrideAimer (impl) | Base returns false |
| 0x4068DE | Weapon::OverrideAimer (thunk) | |
| 0x61B640 | Weapon::ZoomFirstPerson | Static type check, not runtime |
| 0x52C980 | EntitySoldier::UpdateWeaponAndAimer | Per-frame weapon update |
| 0x662DF0 | Team::AddClassByName | Searches g_ClassDefList by hash |
| 0x662DA0 | Team::AddClassByDef | Appends to team SoA arrays |
| 0x662C20 | Team::SetUnitClassMinMax | Writes min/max, fires notification |
| 0x6617B0 | Team::GetClassMinUnits | |
| 0x6617A0 | Team::GetClassMaxUnits | |
| 0x663020 | Team::SetClassUnitCount | Updates existing class counts |
| 0x6633C0 | Team::CopyAllFromTemplate | |
| 0x662FC0 | GetClassDefByName | Searches team's own classDefArr |
| 0x46BAD0 | AddUnitClass (Lua) | Vanilla Lua function |

### Lua API Functions
| Address | Function |
|---|---|
| 0x7B86A0 | lua_pushcclosure |
| 0x7B8580 | lua_pushlstring |
| 0x7B8960 | lua_settable |
| 0x7B7B00 | lua_tolstring |
| 0x7B8560 | lua_pushnumber |
| 0x7B82A0 | lua_tonumber |
| 0x7B7E60 | lua_gettop |
| 0x7B8540 | lua_pushnil |
| 0x7B8720 | lua_pushboolean |
| 0x7B82F0 | lua_toboolean |
| 0x7B8440 | lua_touserdata |
| 0x7B8070 | lua_isnumber |

### Vtables
| Address | Label |
|---|---|
| 0x00A403A0 | Controllable::vftable (base — derived overrides all) |
| 0x00A40500 | Entity::vftable |
| 0x00A3B0AC | Controllable_SubObj084::vftable |
| 0x00A40664 | Controllable_SubObj174::vftable |
| 0x00A52468 | WeaponA::vftable (blaster, melee) |
| 0x00A53510 | WeaponB::vftable (thermal detonator) |
| 0x00A53AE8 | WeaponC::vftable (rocket launcher) |
| 0x00A53020 | WeaponD::vftable (mine dispenser) |
| 0x00A525F4 | WeaponClass::vftable |
| 0x00A2B1BC | IntrusiveListNode::vftable |
| 0x00A5A6E0 | SoldierElement::vftable |
| 0x00A58E20 | WeaponSentinel::vftable |
| 0xA524D8 | WeaponCannon vtable — OverrideAimer slot (patched) |

### Class Limit Patch Sites
| Address | Instruction |
|---|---|
| 0x0068A5CF | `cmp ebx, 0A` |
| 0x0068A5EF | `push 0A` |
| 0x0068A5FC | `mov [esp+30], 0A` |
| 0x0068A6C9 | `cmp ebx, 0A` |
| 0x0068A6CE | `mov edi, 0A` |

---

## Controllable Layout (from Character Weapon System RE)

These were confirmed via runtime testing on BF2_modtools:

| Offset | Type | Field |
|---|---|---|
| +0x034 | Tracker* | mTracker (via Trackable+0x1C) |
| +0x084 | void* | embedded sub-object (vtable 0xA3B0AC) |
| +0x09C | void* | points to ctrl - 0x18 (= intermediate) |
| +0x160 | bool | mTargetInfo.mIsAiming (runtime zoom state) |
| +0x174 | void* | embedded sub-object (vtable 0xA40664) |
| +0x290 | void* | back-pointer to Entity |
| +0x4C0 | Weapon* | always = slot[0], NOT active weapon |
| +0x4D8 | Weapon*[8] | Weapon slot array |
| +0x4F8 | uint8[] | Channel→slot index array |
| +0x744 | int | likely max weapon slot capacity (value: 8) |

**NOTE**: These offsets are from the Controllable pointer in the EntitySoldier context.
The EntityFlyer may use different weapon management (mWeapon at EntityFlyer + 0x6B8,
mAimer at EntityFlyer + 0x6A8).

---

## Weapon Object Layout

| Offset | Type | Field |
|---|---|---|
| +0x000 | void** | vtable |
| +0x020 | PblMatrix | mFirePointMatrix (world-space, 0x40 bytes) |
| +0x060 | WeaponClass* | mStart / mClass |
| +0x064 | WeaponClass* | mClass |
| +0x06C | Controllable* | mOwner |
| +0x070 | Aimer* | mAimer |
| +0x07C | PblVector3 | mFirePos (uninitialized for soldiers) |
| +0x0BC | float | mZoom (max zoom from ODF, not runtime) |

## WeaponClass Layout

| Offset | Type | Field |
|---|---|---|
| +0x000 | void** | vtable (0xA525F4) |
| +0x018 | uint32 | m_nHash |
| +0x030 | char[] | m_szOdfName (inline, null-terminated) |
| +0x2B0 | flags | bit 3 = ZoomFirstPerson type flag |

---

## Aimer Layout

| Offset | Type | Field |
|---|---|---|
| +0x029 | bool | bDirect |
| +0x02C | PblVector3 | mOffsetPos |
| +0x048 | PblVector3 | mDirection |
| +0x054 | PblVector3 | mMountPos |
| +0x070 | PblVector3 | mRootPos |
| +0x088 | PblVector3 | mFirePos |
| +0x0B0 | PblMatrix | mMountPoseMatrix |
| +0x0F0 | PblMatrix[4] | mBarrelPoseMatrix |
| +0x1F0 | RedPose* | mPose |
| +0x204 | int | mCurrentBarrel |
| +0x208 | Weapon* | mWeapon |

---

## Tracker Layout

| Offset | Type | Field |
|---|---|---|
| +0x14 | bool | mIsFirstPersonView |

---

## Team System Structs

Team object layout:
| Offset | Type | Field |
|---|---|---|
| +0x44 | int | classCapacity (max slots) |
| +0x48 | int | classCount (live entries) |
| +0x50 | void** | classDefArr (parallel array of ClassDef*) |
| +0x54 | int* | minUnitsArr |
| +0x58 | int* | maxUnitsArr |

ClassDef linked list node (g_ClassDefList at 0xACD2C8):
| Offset | Type | Field |
|---|---|---|
| +0x04 | node* | next (nullptr = end) |
| +0x0C | void* | classDef (nullptr = sentinel) |

ClassDef:
| Offset | Type | Field |
|---|---|---|
| +0x18 | int | nameHash (NOT a char*) |
| +0x20 | char* | name string (confirmed from spawn function) |

---

## Character Slot Layout

Each character slot is `0x1B0` bytes. Array base at global `0xB93A08`.

| Offset | Type | Field |
|---|---|---|
| +0x00C | void* | changes on weapon switch |
| +0x148 | void* | pointer to "intermediate" object |

Resolution chain to get Controllable:
```
arrayBase + charIndex * 0x1B0 → charSlot
*(charSlot + 0x148) → intermediate
intermediate + 0x18 → Controllable*
```

---

## Existing BF2GameExt Hooks & Patches

### Lua InitState Hook (Detours)
- Hooks `LuaHelper::InitState` at 0x486660
- After original returns, reads `g_lua_state_ptr` and registers custom Lua functions

### WeaponCannon OverrideAimer (Vtable Patch)
- Patches vtable slot at 0xA524D8 (OverrideAimer, vtable offset 0x70)
- Replaces with `hooked_cannon_OverrideAimer` for barrel fire origin
- Toggled at runtime via `SetBarrelFireOrigin(1/0)` Lua function
- Handles zoom detection (Controllable+0x160 mIsAiming, Tracker+0x14 mIsFirstPersonView)
- Handles water reflection Y-clamp

### Class Limit Patch (Binary)
- Patches 5 instruction sites that enforce 10-class limit
- Replaces single 0x0A byte at each site

### Custom Lua Functions
| Function | Purpose |
|---|---|
| PrintToLog(msg) | Write to Bfront2.log |
| GetSystemTickCount() | Windows tick count |
| ReadTextFile(path) | Read file from disk |
| HttpGet/Put/Post(url, body) | Synchronous HTTP |
| HttpGetAsync/PutAsync/PostAsync(url, body) | Fire-and-forget HTTP |
| GetCharacterWeapon(charIdx, channel) | Get weapon ODF name |
| RemoveUnitClass(teamIdx, class) | Remove unit class from team |
| SetBarrelFireOrigin(enable) | Toggle barrel fire origin |
| DumpAimerInfo(charIdx, channel) | Diagnostic dump |

---

## Ghidra TODO List

### 1. Apply EntityFlyer Struct
- Create/update the EntityFlyer data type with the full layout above
- Ensure all sub-structs (GameObject, Controllable, EntityPathFollower, etc.) are defined
- Apply to `EntityFlyer::Update` and other EntityFlyer methods

### 2. Fix this-Pointer on EntityFlyer::Update
- The function at ~0x4FC000 receives `this` at Controllable subobject (EntityFlyer+0x240)
- Either retype `this` as `Controllable*` or create a struct starting at offset 0x240
- Verify by checking that `this + 0x42C` resolves to mClass (EntityFlyerClass*)

### 3. Apply Vtable Labels
- Label all vtables listed in the address table above
- Cross-reference with RTTI from the main exe where available

### 4. Apply Known Function Signatures
- All functions in the address table with known signatures
- Especially: GameLog, HashString, Team methods, Lua API

### 5. Define Enums
- `State` enum for mPitchAdjusted/mState: {LANDED=0, TAKEOFF=1, FLYING=2, LANDING=3, DISABLED=4, CRASHED=5}
- `PilotType` enum (if known values)
- `Trick` enum (referenced in decompilation)

### 6. Transfer RTTI from Main Exe
- The main retail exe has RTTI partially set up
- Copy class hierarchy, vtable assignments, and type information
- Apply to modtools exe functions/vtables

### 7. Find Weapon Gating Assembly Address
- In EntityFlyer::Update, locate the `cmp [reg + 0x124], 0` near:
  - Path A: `GetControllableList` call
  - Path B: `GetJoystickIndex` + `thunk_FUN_00774440`
- Record address for the flyer landing fire patch

---

## Ultimate Goal: Enable Flyer Shooting in Landing Regions

Once the Ghidra struct is fixed and the weapon gating instruction is located:

1. **Identify the exact x86 address** of the `mPitchAdjusted != 0` conditional jump
2. **Implement a patch** (Detours hook or byte patch) that allows weapons when:
   - `mPitchAdjusted == 0` (LANDED) AND
   - `mInLandingRegionFlag` (EntityFlyer + 0x5F4, bit 5) is set
3. **Expose via Lua**: `SetFlyerLandingFire(enable)` — similar pattern to SetBarrelFireOrigin
4. **Add to lua/lua_hooks.cpp and lua/lua_funcs.cpp** following existing patterns

---

## Dead Ends (Don't Retry These)

| Approach | Why It Failed |
|---|---|
| `ctrl+0x9D0` as weapon array | Always zero; ESI in GetAimTurnRate ≠ Controllable |
| `ctrl+0x4C0` as active weapon | Always equals slot[0], never changes |
| Vtable-based weapon channel classification | 3+ vtables per channel |
| `GetActiveWeaponChannel()` | Stub: xor eax,eax; ret |
| `GetWpnChannel()` | All INT3 — pure virtual |
| `Weapon::mZoom` (+0xBC) for runtime zoom | Static max zoom, not current |
| `classDef+0x18` as char* | It's an int hash — strcmp crashes |
| Single-deref g_ppTeams | It's a pointer variable, double deref needed |
| Swap-and-pop for RemoveUnitClass | Hero appears in trooper's slot |
| Left-shift RemoveUnitClass | Pre-cached slot indices cause "no class" warning |
| Detours on Aimer::SetSoldierInfo | Hooks ALL aimers globally |
| Weapon::ZoomFirstPerson for zoom | Static type check, not runtime state |
| Controllable+0x15C for mIsAiming | PDB offset; modtools binary shifted +4 |
| Lateral clamping for barrel fire | No visible effect on bolt origin |
