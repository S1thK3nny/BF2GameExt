# BF2 (2005) Game Struct Reference

Cross-build struct layouts, access paths, and verified offsets for modtools, Steam, and GOG executables.

---

## Struct Sizes

| Struct         | Modtools | Steam/GOG | Notes |
|----------------|----------|-----------|-------|
| Weapon         | ~292     | ~256      | -36 bytes: 3x GameSound (20 vs 8 bytes each) |
| Controllable   | 396      | 392       | -4 bytes: padding before mCtrl removed |
| Character      | 432      | 432       | Identical across builds |
| WeaponClass    | —        | —         | Identical across builds |

---

## Weapon

| Field            | Modtools | Steam | GOG  | Type           | Notes |
|------------------|----------|-------|------|----------------|-------|
| mFirePointMatrix | 0x20     | 0x20  | 0x20 | PblMatrix (64) | Same all builds |
| mStart           | 0x60     | 0x60  | 0x60 | WeaponClass*   | Same all builds |
| mClass           | 0x64     | 0x64  | 0x64 | WeaponClass*   | Same all builds |
| mOwner           | 0x6C     | 0x6C  | 0x6C | Controllable*  | Same all builds |
| mAimer           | 0x70     | 0x70  | 0x70 | Aimer*         | Same all builds |
| mLastFireTime    | 0x11C    | 0xF8  | 0xF8 | float          | After GameSound shift |

**Why the shift:** Retail strips `GameSound` from 20 bytes (handle + type + char* + Node(8) + void*) to 8 bytes (dword handle + byte type). Three GameSound fields x 12 extra bytes = 36-byte shift for fields after ~0xD8.

### WeaponClass

| Field     | Offset | Type      | Notes |
|-----------|--------|-----------|-------|
| mFilename | 0x30   | char[32]  | ODF name, same all builds |

---

## Controllable

| Field              | Modtools | Steam | GOG  | Type             | Notes |
|--------------------|----------|-------|------|------------------|-------|
| Thread (base)      | 0x00     | 0x00  | 0x00 | Thread (24)      | |
| Trackable          | 0x18     | 0x18  | 0x18 | Trackable (32)   | |
| mControlFire       | 0x38     | 0x38  | 0x38 | Trigger[2]       | |
| mControlReload     | 0x40     | 0x40  | 0x40 | Trigger          | |
| mControlMove       | 0x7C     | 0x7C  | 0x7C | float            | |
| mControlStrafe     | 0x80     | 0x80  | 0x80 | float            | |
| mControlTurn       | 0x84     | 0x84  | 0x84 | float            | |
| mControlPitch      | 0x88     | 0x88  | 0x88 | float            | |
| mWpnChannel        | 0x94     | 0x94  | 0x94 | int              | |
| mAIThread          | 0x98     | 0x98  | 0x98 | AIThread (28)    | |
| mNode              | 0xB4     | 0xB4  | 0xB4 | Node (12)        | |
| *(padding)*        | 0xC0 (8) | 0xC0 (4) | 0xC0 (4) | —       | **4-byte diff starts here** |
| mCtrl              | 0xC8     | 0xC4  | 0xC4 | Controller*      | |
| mCharacter         | 0xCC     | 0xC8  | 0xC8 | Character*       | |
| mPilot             | 0xD0     | 0xCC  | 0xCC | Controllable*    | |
| mPlayerId          | 0xD4     | 0xD0  | 0xD0 | int              | == character array index |
| mInitialPlayerId   | 0xD8     | 0xD4  | 0xD4 | int              | |
| mEyePoint          | 0xDC     | 0xD8  | 0xD8 | PblVector3 (12)  | |
| mEyeDir            | 0xE8     | 0xE4  | 0xE4 | PblVector3 (12)  | |
| mTurnBuildup       | 0xF4     | 0xF0  | 0xF0 | float            | |
| mPitchBuildup      | 0xF8     | 0xF4  | 0xF4 | float            | |
| mIndirectControlFlag | 0xFC   | 0xF8  | 0xF8 | bool             | |
| mAnimNameId        | 0x100    | 0xFC  | 0xFC | uint             | |
| mWpnDistFactor     | 0x104    | 0x100 | 0x100| float            | |
| mCollisionObj      | 0x108    | 0x104 | 0x104| PblHandle (8)    | |
| mEnemyLockedOnMeState | 0x110 | 0x10C | 0x10C| float           | |
| mPilotType         | 0x144    | 0x140 | 0x140| int (PilotType)  | |
| mTargetInfo        | 0x148    | 0x144 | 0x144| TargetInfo (28)  | |
| mReticuleTarget    | 0x164    | 0x160 | 0x160| PblHandle[2] (16)| |
| mAIReticuleTarget  | 0x174    | 0x170 | 0x170| PblHandle[2] (16)| |
| mReticuleAffiliation| 0x184   | 0x180 | 0x180| int[2] (8)       | |

**PilotType enum:**
- 0 = PILOT_NONE
- 1 = PILOT_SELF (infantry soldier in own body)
- 2 = PILOT_VEHICLE
- 3 = PILOT_REMOTE
- 4 = PILOT_VEHICLESELF (skip hop if mPilot == owner)

---

## Character

Character array: `sCharacters`, stride = 0x1B0 (432 bytes). Layout is **identical across all builds**.

| Field                  | Offset | Type             | Notes |
|------------------------|--------|------------------|-------|
| mAssignedPost          | 0x18   | ConstCommandPostHandle | |
| mAdrenaline            | 0x1C   | Adrenaline (4)   | |
| mGoalNode              | 0x20   | Node (16)        | |
| mName                  | 0x30   | ushort[64]       | Wide string |
| mRegularName           | 0xB0   | ushort[64]       | Wide string |
| mPlayerId              | 0x130  | int              | == array index |
| mTeamNumber            | 0x134  | int              | |
| mTeamPtr               | 0x138  | Team*            | |
| mClassIndex            | 0x13C  | int              | |
| mClassPtr              | 0x140  | EntityClass*     | |
| mPost                  | 0x144  | CommandPostHandle | |
| mUnit                  | 0x148  | Controllable*    | Primary soldier Controllable |
| mVehicle               | 0x14C  | Controllable*    | Vehicle currently entered |
| mRemote                | 0x150  | Controllable*    | Remote unit |
| mLastVehicleOrRemoteChangeTime | 0x154 | float     | |
| mDelayTimer            | 0x158  | float            | |
| mSpawnCycle            | 0x15C  | int              | |
| mSpawnSlot             | 0x160  | int              | |
| mSpawnReady            | 0x164  | bool             | |
| mHeroFlag              | 0x165  | bool             | |
| mVehicleBlockTime      | 0x168  | float            | |
| mOutOfBoundsTimer      | 0x184  | float            | |
| mSpawnTime             | 0x188  | float            | |
| mCarriedFlagPtr        | 0x18C  | FlagItem*        | |
| mMedalsMgr             | 0x1A0  | MedalsMgr*       | |
| mTakeoverChr           | 0x1A4  | Character*       | |
| mTakeoverTime          | 0x1A8  | float            | |

### Character globals

| Symbol          | Modtools       | Steam          |
|-----------------|----------------|----------------|
| sCharacters     | [0x00b93a08]   | [0x01e30334]   |
| sCharacterLimit | [0x00b939f4]   | [0x01e30330]   |

### mPlayerId invariant

`Character.mPlayerId` always equals the character's array index. Proven by `FindPlayer`:
```c
Character* FindPlayer(int charIdx) {
    if (sCharacters == NULL || charIdx < 0 || charIdx >= sCharacterLimit)
        return NULL;
    if (sCharacters[charIdx].mPlayerId != charIdx)
        return NULL;
    return &sCharacters[charIdx];
}
```

`Controllable.mPlayerId` holds the same value (the owning character's array index). Used by `EnterControllable` for network/event dispatch. Value of -1 means no character assigned. **Warning:** `Controllable.mPlayerId` is NOT reliably set for AI units — do not use it for charIndex resolution.

### Vanilla charIndex resolution (Character* → int)

The vanilla Lua callbacks never read `mPlayerId` to get the character index. They already have a `Character*` pointer and compute the array index via pointer arithmetic:
```c
// Modtools: FUN_007aa5d0  |  Steam: FUN_00519ab0
charIndex = (int)((chrPtr - (uintptr_t)sCharacters) / 0x1B0);
```

### Reverse lookup (Controllable* → charIndex)

To go from a `Controllable*` to a character index (e.g. from a weapon hook), use the `Controllable.mCharacter` back-pointer and the same pointer arithmetic:
```c
Character* chr = controllable->mCharacter;   // +0xCC modtools, +0xC8 retail
charIndex = (int)((uintptr_t)chr - (uintptr_t)sCharacters) / 0x1B0;
```
This is O(1) and matches the vanilla approach exactly.

---

## Access Paths

### Weapon → Character Index (OnCharacterFireWeapon)

```
Weapon*
  ├─ + mClass_offset → WeaponClass* → +0x30 → ODF name
  ├─ + 0x6C → Controllable* owner (mOwner)
  │    ├─ + mPilotType_offset → PilotType
  │    └─ + mPilot_offset → Controllable* pilot
  │         └─ (if vehicle: shooter = pilot, else shooter = owner)
  └─ shooter (Controllable*)
       └─ + mCharacter_offset → Character* chr (back-pointer, O(1))
            └─ charIndex = (chr - sCharacters) / 0x1B0
```

Uses the same pointer-arithmetic approach as vanilla Lua callbacks.
Non-character fires (unmanned turrets, stale state) produce a NULL mCharacter
or an out-of-bounds index and are filtered out.

### Character Index → Controllable (GetCharacterUnit)

```
charIndex
  → sCharacters + charIndex * 0x1B0 → Character
    → +0x148 (mUnit) → Controllable*
      → Trackable vtable → GetGameObject → GameObject*
```

### Character Index → Weapon ODF (GetCharacterWeapon)

```
charIndex
  → sCharacters + charIndex * 0x1B0 → Character
    → +0x148 (mUnit) → intermediate (Controllable*)
      → intermediate + 0x18 → ctrl
        → ctrl + 0x4F8 + channel → uint8 slotIdx
        → ctrl + 0x4D8 + slotIdx*4 → Weapon*
          → +0x60 → WeaponClass* → +0x30 → ODF name
```

### Character Index → Class Name (GetCharacterClassName)

```
charIndex
  → sCharacters + charIndex * 0x1B0 → Character
    → +0x148 → intermediate
      → intermediate - 0x240 → EntitySoldier*
        → +0x08 → EntityClass*
          → +0x20 → char[32] mFilename (class ODF name)
```

---

## Key Function Addresses

| Function           | Modtools   | Steam      | Notes |
|--------------------|------------|------------|-------|
| FindPlayer         | 0x00447b10 | 0x00429470 | Validates Character.mPlayerId == index |
| GetCharacterUnit   | 0x0046ec80 | 0x0058fcd0 | Lua callback |
| EnterControllable  | 0x005448d0 | 0x004f0ca0 | Reads Controllable.mPlayerId |
| AddPlayerCharacter | 0x0065f6b0 | unknown    | SpawnManager method |
| CharToIndex (vanilla) | 0x007aa5d0 | 0x00519ab0 | `(chr - sCharacters) / 0x1B0` → lua_pushnumber |

## Ghidra Instances

| Port | Executable |
|------|------------|
| 8192 | BF2_modtools_MemExt.exe (modtools) |
| 8193 | BattlefrontII.exe (Steam) |
| 8194 | BF2_modtools_NoDVD |
| 8195 | BattlefrontII_MemExt.exe (GOG) |
