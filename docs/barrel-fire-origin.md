# Barrel Fire Origin — Implementation Notes

## Overview

Overrides the projectile fire origin for `WeaponCannon` so bolts originate from the
weapon model's barrel hardpoint (`Weapon::mFirePointMatrix`) instead of the hardcoded
chest-level aimer position (`sEyePointOffset`).

Controlled at runtime via Lua: `SetBarrelFireOrigin(1)` / `SetBarrelFireOrigin(0)`.

Currently scoped to **WeaponCannon only** via vtable patching. Can be extended to
other weapon classes (WeaponLaser, WeaponLauncher, etc.) by patching their vtables.

---

## Current Status

**Working:**
- Barrel fire origin override — bolts come from gun barrel in both first and third person
- Toggle on/off via `SetBarrelFireOrigin(1)` / `SetBarrelFireOrigin(0)`
- Lua 5.0 truthiness fix — `0` correctly disables (was the root cause of all
  toggle-off failures across every approach we tried)
- Water reflection Y-clamp — when mFirePointMatrix Y delta from mRootPos exceeds
  5 units, Y is clamped to `rootPos - 2.7` (approximate barrel offset)
- Writes corrected position to both `mFirePos` AND `mFirePointMatrix` trans (muzzle
  flash reads mFirePos; projectile system may read the matrix directly)
- Zoom detection — reverts to vanilla aimer during first-person zoom only:
  - `mIsAiming` at `Controllable+0x160` detects runtime zoom state
  - `mIsFirstPersonView` at `Controllable+0x34` (mTracker ptr) → `Tracker+0x14`
    detects first-person camera mode
  - Third-person zoom keeps barrel fire origin active

**Known Issues:**
- **Water reflection — projectile origin:** Muzzle flash is correct (reads mFirePos)
  but bolts still originate from reflected position. The render reflection pass
  overwrites `mFirePointMatrix` AFTER our hook. Y-clamp helps for large reflections
  but subtle reflections (water near character height, delta < 5) slip through.
- **Animation timing — running to fire:** When firing during a running animation, the
  bolt originates from the current barrel tip position (which may be far to the side
  in the run animation) before the fire animation plays. This is inherent to reading
  `mFirePointMatrix` which reflects the current animation frame. Lateral clamping was
  attempted (decompose into aim-parallel/perpendicular, clamp lateral offset) but had
  no visible effect — the bolt visual still renders from the animated barrel position
  regardless of where `mFirePos`/`mFirePointMatrix` trans are written. Accepted as a
  cosmetic limitation.

---

## Architecture

### Hook Mechanism — WeaponCannon vtable patch

`Weapon::OverrideAimer` is a virtual method at vtable slot `0x70` (byte offset in
vtable). The base implementation at `0x61CEE0` simply returns `false`. No vanilla
weapon subclass overrides it except `WeaponMelee` (which adjusts direction, not
position). The method is called by the engine once per frame per weapon during the
aimer update cycle.

At DLL load, `lua_hooks_install()` patches the WeaponCannon vtable entry to point to
our `hooked_cannon_OverrideAimer`. On toggle off, `SetBarrelFireOrigin(0)` swaps it
back to the vanilla pointer via `VirtualProtect`.

**Why vtable patch instead of Detours on SetSoldierInfo:**
We tried Detouring `Aimer::SetSoldierInfo` (`0x5EE9D0`) — this intercepts at the
exact moment `mFirePos` is written and can't be overwritten afterward. However, it
hooks **all** aimers globally (every weapon type, every AI unit, vehicles, turrets),
which caused regressions. The vtable approach is scoped to WeaponCannon only.

### Data Flow

```
Engine frame update:
  EntitySoldier::UpdateWeaponAndAimer (0x52C980)
    → reads sEyePointOffset[stance] (stand/crouch/prone offsets)
    → computes aimer position = soldier_pos + stance_offset
    → calls Aimer::SetSoldierInfo(aimer, pos, dir)
        → writes mFirePos, mRootPos, mDirection, bDirect=true
    → calls weapon->OverrideAimer() via vtable
        → our hook checks zoom + first-person → skip if both true
        → reads Weapon::mFirePointMatrix trans
        → overwrites aimer->mFirePos with barrel world position
        → also writes corrected pos back to mFirePointMatrix trans
        → returns true

Rendering:
  MuzzleFlashRenderer::RenderFlash reads aimer->mFirePos → correct position
  Projectile system reads mFirePointMatrix → may get re-contaminated by reflection
```

### Key Insight

Our hook writes to `mFirePos` **after** `SetSoldierInfo` has already set it. This is
important — `SetSoldierInfo` sets both `mFirePos` and `mRootPos` to the same value.
We only overwrite `mFirePos`, leaving `mRootPos` at the engine's correct value. This
prevents corrupting the aimer's stance calculations, LOS checks, and next-frame
positioning.

---

## Engine Struct Layouts (from PDB + Ghidra)

### Controllable (partial — offsets from Controllable base, i.e. weapon->mOwner)

**NOTE:** The PDB struct has a 4-byte shift starting around offset 0x144. Fields
before this (Thread, Trackable, triggers) use PDB offsets directly. Fields at 0x144+
are shifted by +4 bytes in the modtools binary compared to the PDB.

| Offset | Type | Field | Notes |
|--------|------|-------|-------|
| +0x00 | Thread | (base class) | 24 bytes (0x18) |
| +0x18 | Trackable | (base class) | 32 bytes (0x20) |
| +0x34 | Tracker* | mTracker | Trackable+0x1C; has camera state |
| +0x54 | Trigger | mControlZoom | |
| +0x58 | Trigger | mControlView | |
| +0x140 | PilotType | mPilotType | |
| +0x144 | int(?) | unknown | Extra 4 bytes not in PDB (always 1) |
| +0x148 | TargetInfo | mTargetInfo | PDB says 0x144; shifted +4 in modtools |
| +0x160 | bool | mTargetInfo.mIsAiming | 0x148 + 0x18; runtime zoom state |
| +0x164 | PblHandle[2] | mReticuleTarget | PDB says 0x160; shifted +4 |

### Tracker (partial)

| Offset | Type | Field |
|--------|------|-------|
| +0x14 | bool | mIsFirstPersonView |

### Aimer (partial)

| Offset | Type | Field |
|--------|------|-------|
| +0x29 | bool | bDirect |
| +0x2C | PblVector3 | mOffsetPos |
| +0x48 | PblVector3 | mDirection |
| +0x54 | PblVector3 | mMountPos |
| +0x70 | PblVector3 | mRootPos |
| +0x88 | PblVector3 | mFirePos |
| +0xB0 | PblMatrix | mMountPoseMatrix (uninitialized for soldiers) |
| +0xF0 | PblMatrix[4] | mBarrelPoseMatrix (uninitialized for soldiers) |
| +0x1F0 | RedPose* | mPose |
| +0x204 | int | mCurrentBarrel |
| +0x208 | Weapon* | mWeapon |

### Weapon (partial)

| Offset | Type | Field |
|--------|------|-------|
| +0x20 | PblMatrix | mFirePointMatrix (world-space fire hardpoint) |
| +0x60 | WeaponClass* | mStart |
| +0x64 | WeaponClass* | mClass |
| +0x6C | Controllable* | mOwner |
| +0x70 | Aimer* | mAimer |
| +0x7C | PblVector3 | mFirePos (uninitialized for soldiers — 0xCDCDCDCD) |
| +0xBC | float | mZoom (zoom magnification, e.g. 2.5 — does NOT change at runtime) |

### WeaponClass (partial)

| Offset | Type | Field |
|--------|------|-------|
| +0x30 | char[] | ODF name |
| +0x2B0 | flags | bit 3 = ZoomFirstPerson type flag (forces scoped aim, not general FP) |

### PblMatrix layout (0x40 bytes)

| Offset | Row |
|--------|-----|
| +0x00 | right (PblVector4) |
| +0x10 | up (PblVector4) |
| +0x20 | forward (PblVector4) |
| +0x30 | trans (PblVector4) — world position |

### sEyePointOffset (0xACE360) — hardcoded stance offsets

| Stance | X | Y | Z |
|--------|------|------|------|
| Stand | 0.06 | 1.70 | 0.00 |
| Crouch | -0.10 | 1.20 | 0.00 |
| Prone | 0.06 | 0.60 | 0.40 |

---

## Current Code State

### weapon/barrel_fire_origin.cpp — hooked_cannon_OverrideAimer (snippet below is an older revision; see the source for current)

```cpp
static bool __fastcall hooked_cannon_OverrideAimer(void* weapon, void* /*edx*/)
{
   if (!g_useBarrelFireOrigin) return false;

   // Zoom detection: revert to vanilla aimer only during first-person zoom.
   // mIsAiming (owner+0x160): runtime zoomed state.
   // mIsFirstPersonView: Controllable+0x34 (mTracker ptr) -> Tracker+0x14.
   void* owner = *(void**)((char*)weapon + 0x6C);
   if (owner) {
      bool isZoomed = *(bool*)((char*)owner + 0x160);
      if (isZoomed) {
         void* tracker = *(void**)((char*)owner + 0x34);
         if (tracker) {
            bool isFirstPerson = *(bool*)((char*)tracker + 0x14);
            if (isFirstPerson) return false;
         }
      }
   }

   __try {
      void* aimer = *(void**)((char*)weapon + 0x70);
      if (!aimer) return false;

      float* trans = (float*)((char*)weapon + 0x20 + 0x30);

      const uint32_t raw = *(uint32_t*)&trans[0];
      if (raw == 0xCDCDCDCD ||
          (trans[0] == 0.0f && trans[1] == 0.0f && trans[2] == 0.0f))
         return false;

      float* aimerFirePos = (float*)((char*)aimer + 0x88);
      float* rootPos      = (float*)((char*)aimer + 0x70);

      // Water reflection Y-clamp
      float fireY = trans[1];
      float barrelRootDy = fireY - rootPos[1];
      if (barrelRootDy < -5.0f || barrelRootDy > 5.0f) {
         fireY = rootPos[1] - 2.7f;
      }

      // Write to both mFirePos and mFirePointMatrix trans
      aimerFirePos[0] = trans[0];
      aimerFirePos[1] = fireY;
      aimerFirePos[2] = trans[2];

      trans[0] = aimerFirePos[0];
      trans[1] = fireY;
      trans[2] = aimerFirePos[2];
      return true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}
```

### lua/lua_funcs.cpp — SetBarrelFireOrigin

Uses `isnumber`/`tonumber` to handle Lua 5.0 truthiness (`0` is truthy in Lua).
Swaps WeaponCannon vtable entry between hook and vanilla via VirtualProtect.

### lua/lua_funcs.cpp — DumpAimerInfo

Diagnostic function. Dumps aimer and weapon data to `Bfront2.log`, including
a wide byte range at `owner+0x100..+0x300` for struct exploration.

---

## Zoom Detection — Resolution Chain

```
weapon + 0x6C                → Controllable* (mOwner)
Controllable + 0x160         → bool mIsAiming (runtime zoom state)
Controllable + 0x34          → Tracker* (via Trackable::mTracker)
Tracker + 0x14               → bool mIsFirstPersonView
```

Both must be true to skip barrel fire origin. This means:
- Third-person unzoomed → barrel fire
- Third-person zoomed → barrel fire
- First-person unzoomed → barrel fire
- First-person zoomed → vanilla aimer (mFirePointMatrix freezes in FP)

**PDB struct shift note:** The PDB places `mTargetInfo` at Controllable+0x144, putting
`mIsAiming` at +0x15C. In the modtools binary, there's an extra 4 bytes at +0x144
(unknown field, always value 1), shifting `mTargetInfo` to +0x148 and `mIsAiming` to
+0x160. Confirmed via byte dump diff across 4 states (3P/1P x zoomed/unzoomed).

---

## Lua API

### SetBarrelFireOrigin(enable)

Toggle barrel fire origin override for WeaponCannon.

```lua
SetBarrelFireOrigin(1)     -- enable
SetBarrelFireOrigin(0)     -- disable
SetBarrelFireOrigin(true)  -- enable
SetBarrelFireOrigin(false) -- disable
```

**Lua 5.0 truthiness note:** In Lua 5.0, the number `0` is truthy — `lua_toboolean`
returns 1 for it. The implementation checks `lua_isnumber` first and uses
`lua_tonumber` for numeric args, so `SetBarrelFireOrigin(0)` correctly disables.

### DumpAimerInfo(charIndex [, channel])

Diagnostic function. Dumps aimer and weapon data to `Bfront2.log`.

```lua
DumpAimerInfo(0)      -- dump player character, channel 0
DumpAimerInfo(0, 1)   -- dump player character, channel 1
```

Output includes: `Weapon::mZoom`, owner byte dump (0x100-0x300),
`Weapon::mFirePointMatrix`, `Weapon::mFirePos`, `Aimer::mFirePos`, `mMountPos`,
`mRootPos`, `mCurrentBarrel`, all 4 `mBarrelPoseMatrix` entries, `mMountPoseMatrix`.

---

## Address Reference (modtools exe, base 0x400000)

| Item | Address |
|------|---------|
| Weapon::OverrideAimer impl | 0x61CEE0 |
| Weapon::OverrideAimer thunk | 0x4068DE |
| WeaponCannon vtable OverrideAimer slot | 0xA524D8 |
| Aimer::SetSoldierInfo | 0x5EE9D0 (thunk 0x402702) |
| EntitySoldier::UpdateWeaponAndAimer | 0x52C980 (thunk 0x40283D) |
| Weapon::ZoomFirstPerson | 0x61B640 (static type check, NOT runtime) |
| MuzzleFlashRenderer::RenderFlash | needs address |
| sEyePointOffset (3x PblVector3) | 0xACE360 |
| sEyePointRelativeWeaponOffset | 0xACE384 |
| Character array base ptr | 0xB93A08 |
| Max character count | 0xB939F4 |
| Team array ptr | 0xAD5D64 |
| Global class def list | 0xACD2C8 |
| GameLog | 0x7E3D50 |
| HashString | 0x7E1BD0 |

---

## Steam crash — SOLVED 2026-07-10: was aim assist, NOT this hook

**Status: the hook is installed on modtools AND Steam.** The crash that got it
gated to modtools on 2026-07-10 was misattributed — this hook was innocent.

The crash:

```
AV READ 42C80088  at BattlefrontII.exe+0x255334  (Ghidra VA 0x655334)
  MOVSX EAX, byte ptr [EDX + ESI + 0x88]   ; EDX=0, ESI=0x42C80000 == 100.0f
```

**Real cause (found via the `BF2GameExt.dll+0x197AF` return address on the crash
stack):** the aim-assist **proximity friction** query in `aim_assist.cpp` called
`TeamManager::sGetObjectsInRange` (Steam `0x6552D0`, GOG `0x656370`) through the
modtools **cdecl** signature `(pos, radius, out, maxCount, team, flags, exclude)`.
On the release (LTCG) builds the function actually uses a custom register
convention:

```
ECX = pos, EDX = out, XMM1 = radius,
stack = (maxCount, team, flags, exclude), plain RET (caller cleans)
```

With the cdecl call every stack argument shifts one slot: the callee reads the
caller's `100.0f` radius constant (`0x42C80000`) as its `Team*` and faults on the
relations byte at `[team+0x88]` → AV address `0x42C80088`. The crash fires with a
gamepad connected + `[AimAssist]` + `ProximityFriction` enabled, regardless of the
barrel-fire-origin toggle — which is why gating this hook never actually fixed it.
Fixed by a marshalling thunk in `aim_assist.cpp` (`teamGetObjectsInRange_release_thunk`)
selected for Steam/GOG at install.

**Why the old "OverrideAimer returns true" theory was wrong:** the return value of
`OverrideAimer` is **discarded at every release call site**. The virtual invoker was
found by byte-scanning the Phantom build for `call [reg+0x70]`:
`EntitySoldier::Update` calls it at Phantom `0x584cc6` — `call [eax+0x70]` followed
by an unconditional `jmp`, no `test al,al` (likewise the two `MountedTurret::Update`
sites at `0x6740ee`/`0x674154`). Returning `true` unconditionally has no
second-order AI/lock-on effect. The earlier verification stands: vtable slot and
Aimer/Weapon offsets are correct and the mFirePos write is safe.

## Steam Port (2026-07-08) — superseded by the crash note above

Ported the origin-only fix (no direction convergence — that attempt was reverted).
Installed build-aware via `barrel_fire_origin_install()` (weapon/barrel_fire_origin.cpp),
called from dllmain's build-aware section, so it runs on Steam as well as modtools. GOG
not yet derived (no-ops there).

**Steam addresses** (base 0x400000), already present in `game_addrs::steam`:

| Item | Steam | Verified |
|------|-------|----------|
| Weapon::OverrideAimer impl (returns false) | 0x00677780 | slot content + decompile |
| Weapon::OverrideAimer thunk | 0x00677780 | no ILT thunk in release; == impl |
| WeaponCannon vtable | 0x007B057C | ctor `mov [esi],0x7b057c` |
| WeaponCannon OverrideAimer slot (vtable+0x70) | 0x007B05EC | contains 0x00677780 |

**Struct offsets** — all build-invariant EXCEPT `mIsAiming` (Controllable):

| Field | Modtools | Steam | Source |
|-------|----------|-------|--------|
| mIsAiming (TargetInfo+0x18) | 0x160 | **0x15C** | TargetInfo 0x148→0x144 (game_struct_reference.md) |
| mOwner/mClass/mAimer/mFirePointMatrix | 0x6C/0x64/0x70/0x20 | same | Weapon invariant |
| mTracker (Trackable+0x1C) | 0x34 | 0x34 | Trackable invariant |
| Tracker::mIsFirstPersonView | 0x14 | 0x14 | assumed engine-invariant (verify if FP-scope off) |
| WeaponClass +0x2B0 zoom bit | 0x2B0 | 0x2B0 | WeaponClass invariant |
| Aimer::mFirePos/mRootPos | 0x88/0x70 | same | Aimer invariant |

The hook selects `mIsAiming` via `s_barrelMisAimingOff` (set in the installer).

**Caveat — reflection guard:** the water-reflection Y reconstruction reads
`owner-0x11C` (soldier world Y = Entity+0x124, assuming Entity→Controllable=0x240).
That assumption is unverified on Steam; if it's off, only the muzzle-flash Y inside
water reflections is wrong (main pass uses trans[1] directly, unaffected; whole block
is under det<0 + __try). Verify if reflections look wrong near water on Steam.

---

## Address Reference (modtools exe, base 0x400000)

| Item | Address |
|------|---------|
| Weapon::OverrideAimer impl | 0x61CEE0 |
| Weapon::OverrideAimer thunk | 0x4068DE |
| WeaponCannon vtable OverrideAimer slot | 0xA524D8 |
| Aimer::SetSoldierInfo | 0x5EE9D0 (thunk 0x402702) |
| EntitySoldier::UpdateWeaponAndAimer | 0x52C980 (thunk 0x40283D) |
| Weapon::ZoomFirstPerson | 0x61B640 (static type check, NOT runtime) |
| MuzzleFlashRenderer::RenderFlash | needs address |
| sEyePointOffset (3x PblVector3) | 0xACE360 |
| sEyePointRelativeWeaponOffset | 0xACE384 |
| Character array base ptr | 0xB93A08 |
| Max character count | 0xB939F4 |
| Team array ptr | 0xAD5D64 |
| Global class def list | 0xACD2C8 |
| GameLog | 0x7E3D50 |
| HashString | 0x7E1BD0 |

---

## Files Modified

| File | Purpose |
|------|---------|
| `PatcherDLL/src/weapon/barrel_fire_origin.{hpp,cpp}` | OverrideAimer vtable hook + build-aware install/uninstall (moved here from lua_hooks 2026-07-08; no Lua dependency) |
| `PatcherDLL/src/lua/lua_funcs.cpp` | `DumpAimerInfo` diagnostic Lua function (modtools-only) |
| `PatcherDLL/src/core/dllmain.cpp` | reads INI toggle; calls `barrel_fire_origin_install` in the build-aware section |
| `PatcherDLL/src/core/game_addrs.hpp` | modtools + steam addresses |
| `docs/barrel-fire-origin.md` | This file |

**Note:** `SetBarrelFireOrigin` (the old live Lua toggle) no longer exists — control
is INI-only (`[Fixes] BarrelFireOriginFix`). The hook was historically parked in
`lua_hooks.cpp` because it began as a modtools Lua experiment; it has no Lua tie and
now lives in its own `weapon/` module.

---

## Known Issues / Future Work

1. **Water reflection — projectile origin:** Muzzle flash is correct (reads mFirePos)
   but bolts still originate from reflected position (projectile system reads
   mFirePointMatrix which gets re-contaminated by render pass). Possible fixes:
   - Find a pre-reflection copy of bone transforms (trace XREFs to weapon+0x20)
   - Hook the projectile spawn to force it to read mFirePos
   - Find a reflection region flag to detect and skip during reflection pass

2. **Animation timing — running to fire:** When firing during a running animation, the
   bolt originates from the barrel tip's current animated position (may be far to the
   side) before the fire animation transitions. `mFirePointMatrix` always reflects the
   current animation frame — there's no way to predict the post-transition barrel
   position. Lateral clamping (decomposing into aim-parallel/perpendicular) was tried
   but had no visible effect. Possible future approaches:
   - Only use Y from barrel, keep vanilla X/Z (loses lateral barrel positioning)
   - Fixed offset from rootPos along aim direction (consistent but less realistic)
   - Accept as cosmetic (current approach)

3. **Per-weapon-class control:** Currently only patches WeaponCannon. To extend:
   - Find vtable addresses for WeaponLaser, WeaponLauncher, etc. in Ghidra
   - Add their OverrideAimer vtable slots to `lua_addrs`
   - Patch each vtable in `lua_hooks_install()`

4. **Slight aiming offset:** The barrel position is ~0.5 units from the aimer
   position. At close range this causes a small parallax between crosshair and impact.

5. **AI units:** The vtable patch affects all WeaponCannon instances, including AI.
   This is generally desirable but may need a per-entity toggle if issues arise.

---

## Approaches Tried and Rejected

### Detours on Aimer::SetSoldierInfo

Intercepted all calls to SetSoldierInfo globally. Replaced the position parameter
with barrel position before calling the original.

**Problem:** Affects every aimer in the game (all weapon types, AI, vehicles). Caused
secondary weapons to break and aimers to get stuck. The vtable approach is scoped to
WeaponCannon only.

### Passing modified pos to original SetSoldierInfo

Called original SetSoldierInfo with barrel position as the pos parameter.

**Problem:** `SetSoldierInfo` sets `bDirect = true` and writes barrel position to
both `mFirePos` AND `mRootPos`. Corrupting `mRootPos` broke stance calculations and
next-frame positioning. Fixed by calling original first (unmodified), then overwriting
only `mFirePos` afterward.

### Lua truthiness bug (root cause of ALL toggle-off failures)

In Lua 5.0, the number `0` is truthy. `lua_toboolean(L, 1)` on the number `0`
returns `1`. So `SetBarrelFireOrigin(0)` was setting `g_useBarrelFireOrigin = true`,
not false. Every toggle-off attempt across every approach was a no-op. Fixed by
checking `lua_isnumber` first and using `lua_tonumber` for numeric arguments.

### Weapon::ZoomFirstPerson for zoom detection

Called `Weapon::ZoomFirstPerson(weapon)` to check if zoomed.

**Problem:** It checks a static class flag (`mClass->field_0x2b0 & 8`) and entity
type — returns whether the weapon TYPE supports first-person zoom (scoped weapons
only), not whether it's currently zoomed. Always returns the same value regardless
of zoom state.

### Weapon::mZoom (+0xBC) for zoom detection

Read the float at `weapon+0xBC` expecting it to change between zoomed/unzoomed.

**Problem:** The value is 2.5 (mZoomMax from ODF) in both states. The field at +0xBC
is the max zoom setting, not the current zoom level.

### XZ staleness check for zoom detection

Compared `mFirePointMatrix` XZ distance to `mFirePos` XZ. If > 3 units, matrix is
stale (zoomed + moved).

**Problem:** Only detects stale matrix if the player has moved since zooming. Standing
still while zoomed shows zero XZ distance and the check doesn't trigger.

### Controllable+0x15C for mIsAiming (PDB offset)

Tried reading `owner+0x144+0x18` (TargetInfo at PDB's 0x144, mIsAiming at +0x18).

**Problem:** Read values 110 and 20 — not booleans. The modtools binary has a 4-byte
struct shift: an extra unknown field at 0x144 pushes mTargetInfo to 0x148 and
mIsAiming to 0x160.

### Lateral clamping for running-animation barrel offset

Decomposed barrel offset (from rootPos) into aim-parallel and aim-perpendicular
components. Clamped perpendicular (lateral) offset to 0.5 units max.

**Problem:** Had no visible effect. The bolt visual still renders from the animated
barrel position regardless of where `mFirePos` and `mFirePointMatrix` trans are
written. The visual bolt start point may be determined by the rendering system's
bone positions rather than the aimer fields we write to.
