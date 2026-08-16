# Barrel Fire Origin — Implementation Notes

## Overview

Overrides the projectile fire origin for `WeaponCannon` so bolts originate from the
weapon model's barrel hardpoint (`Weapon::mFirePointMatrix`) instead of the hardcoded
chest-level aimer position (`sEyePointOffset`).

Scoped to **WeaponCannon and WeaponLauncher** via vtable patching. See
[Patched classes](#patched-classes) for why those two and what it would take to add
more.

---

## Current Status

**Working:**
- Barrel fire origin override — bolts come from gun barrel in both first and third person
- Direction convergence while zoomed — the barrel origin stays active at every zoom
  level, and `Aimer::mDirection` is re-aimed at the point the vanilla ray would hit,
  found with `CollisionManager::RayHit` (see below)
- Reflection regions — the mirrored duplicate draw no longer contaminates
  `mFirePointMatrix`; see [Reflection regions](#reflection-regions)
- First-person zoom still reverts to the vanilla aimer (`mFirePointMatrix` goes stale)

**Known Issues:**
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

### Hook Mechanism — vtable patch

`Weapon::OverrideAimer` is a virtual method at vtable slot `0x70` (byte offset in
vtable). The base implementation at `0x61CEE0` simply returns `false`. No vanilla
weapon subclass overrides it except `WeaponMelee` (which adjusts direction, not
position). The method is called by the engine once per frame per weapon during the
aimer update cycle.

At DLL load, `barrel_fire_origin_install()` walks a small table of vtable slots,
checks each still holds the vanilla implementation, and swaps in
`hooked_cannon_OverrideAimer` via `VirtualProtect`, remembering what it displaced.

**Why vtable patch instead of Detours on SetSoldierInfo:**
We tried Detouring `Aimer::SetSoldierInfo` (`0x5EE9D0`) — this intercepts at the
exact moment `mFirePos` is written and can't be overwritten afterward. However, it
hooks **all** aimers globally (every weapon type, every AI unit, vehicles, turrets),
which caused regressions. The vtable approach stays scoped to chosen classes.

### Patched classes

| Class | ClassLabel | modtools vtable | slot `+0x70` | Steam vtable | slot `+0x70` |
|-------|-----------|-----------------|--------------|--------------|--------------|
| WeaponCannon | `cannon` | `0xA52468` | `0xA524D8` | `0x7B057C` | `0x7B05EC` |
| WeaponLauncher | `launcher` | `0xA53AE8` | `0xA53B58` | `0x7B12A4` | `0x7B1314` |

`WeaponLauncher` derives from `WeaponCannon` — its constructor (modtools
`0x62F6B0`) chains straight into the `WeaponCannon` constructor — but the compiler
still emits it a separate vtable, so the cannon patch does not reach it.

Diffing the two vtables, `WeaponLauncher` overrides seven slots: the destructor plus
`+0x14`, `+0x18`, `+0x1C`, `+0x2C`, `+0x30`, `+0x34`. Neither `OverrideAimer`
(`+0x70`) nor `Fire` is among them — there is no `WeaponLauncher::Fire` in the
binary at all, so launchers reach `WeaponCannon::Fire` (modtools `0x626490`), which
builds the ordnance through `Aimer::GetMatrix` on the same aimer the hook writes.
The hook therefore needed no changes for launchers; only a second slot in the table.

**Adding more classes is not automatic.** `WeaponLaser` (vtable `0xA538D0`) and
`WeaponGrenade` both derive from `Weapon` directly rather than from `WeaponCannon`,
and `WeaponGrenade`, `WeaponDispenser`, `WeaponMeleeThrow`, `WeaponRemote` and
`WeaponRepair` each define their own `Fire`. Before patching any of them, confirm
that its fire path actually reads the aimer's `mFirePos` — otherwise the patch is
a no-op at best.

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
        → reads Weapon::mFirePointMatrix trans (left there by last frame's draw)
        → overwrites aimer->mFirePos with barrel world position
        → re-aims aimer->mDirection at the vanilla ray's hit point while zoomed
        → returns true
  WeaponCannon::Fire builds OrdnanceDesc.pos straight from Aimer::mFirePos

Rendering:
  Weapon::Render bakes the matrix it is handed into mFirePointMatrix, so the
  fire point the hook reads next frame is whatever the LAST draw of this frame
  used — see Reflection regions below.
```

### Reflection regions

`Weapon::Render` (modtools `0x61DFA0`, Steam `0x679350`, GOG `0x67A3F0`, Phantom
`0x7AE8C0`) is the engine's **only** writer of `mFirePointMatrix`:

```
Weapon::Render(PblMatrix* world, RedPose* pose, RedColor* color,
               uint flags, bool highRes)
    node  = pose->Find(0x2B960099)      // 0xB7EC1D31 when WeaponClass flag 0x80
    if (!node) return;                  // no hardpoint -> matrix left alone
    world = node * world
    mRenderClass->mModel->Render(world, 0, color, flags, 0)
    if (color->a == 0) return;          // invisible -> matrix left alone
    mFirePointMatrix       = world
    mFirePointMatrix.trans = TransformCoord(mRenderClass->mFirePointOffset, world)
```

Dynamic entities get their planar reflection by being **drawn a second time in the
main pass with a mirrored world matrix**, not by the reflection-texture pass. Every
entity Render does the same thing (`EntityProp::Render` `0x560DC0` is the small
readable example, `EntitySoldier::Render` `0x575380` the one that matters):

```
if (!(flags & 0x200000) &&                      // not already the reflection scene
    FLRenderer::IsReflected(&worldPos, radius, &R, false))
    reflMatrix = world * R
    reflFlags  = flags & ~0x10000 | 0x10000100
```

`FLRenderer::IsReflected` (Phantom `0x88DF30`) walks `ReflectionRegion::s_VisibleList`
and returns the **first** region containing the point, handing back that region's
`m_reflectionMat`. `EntitySoldier::Render` then renders each weapon channel twice —
Phantom `0x57844E` with the real matrix, `0x57848B` with `reflMatrix`, and the same
pair at `0x5784CB`/`0x5784FE` for the offhand channel — so the **mirrored draw is the
last write of the frame** and `mFirePointMatrix` keeps the reflected fire point.

Reading it a frame later put the bolt at the mirror image: an error of twice the
shooter's height above the reflective plane. In dea1's `falconhanger` (a light region
overlapped by reflection region `hanger12`, `dea1_reflection_region.RGN`, centre
`-137.41, 58.01, 18.39`, size `34.5, 8, 32.4`) that is ~0 standing on the hangar
floor, a few units on the stairs, and tens of units from the walkways above — far
enough that the bolt's sound never reached the player, which first read as "no sound".

**Fix (2026-08-17):** hook `Weapon::Render` on vtable slot `0x8C` and bracket the
mirrored draw — save `mFirePointMatrix`, call the original, put it back. The engine
still sees the mirrored matrix for the whole call, so the reflected muzzle flash
(`WeaponClass::RenderFlash`) and the charge-up emitter (`FLEffectObject::AttachEffectToMatrix`)
are unchanged; afterwards the field holds what the real draw wrote.

The predicate is the handedness of the matrix about to be baked — a mirror is an
improper transform, so its 3×3 determinant is negative. This needs no knowledge of
where the mirror plane is, survives any number of overlapping reflection regions
(each mirrored draw is bracketed on its own), and works for a mirror of any
orientation rather than only a horizontal floor.

Neither `WeaponCannon` nor `WeaponLauncher` overrides `Render`, so both vtable slots
hold the same entry. `Weapon::Render` stays plain `__thiscall` on all three builds
(ECX = `this`, five stack args, `RET 0x14`) — it is virtual, so LTCG left the
convention alone; verified off the Steam and GOG prologue/epilogue.

**The general hazard:** anything that caches render-time state per object is wrong
inside a reflection region, because the object is drawn twice and the mirrored draw
wins. Suspect this first for any bug that only appears near reflective floors.

#### Approaches tried and rejected

- **Un-mirror across the soldier's feet** (`true_y = 2*Yw − trans.y`, `Yw` read at
  `owner − 0x11C`). Exact only while standing *on* the reflective floor and wrong by
  twice the height above the plane everywhere else, unbounded — this was the falcon
  hangar bug. It also rested on the unverified `Entity→Controllable == 0x240`
  assumption.
- **Carry the barrel-to-eye Y delta across frames** (cache `trans.y − mRootPos.y` on
  un-mirrored frames, rebuild Y from it on mirrored ones). Inside a reflection region
  the mirrored draw is the last write *every* frame, so there are no un-mirrored
  frames to sample: a weapon that first appears inside the region never gets a delta.
  It also only handles a horizontal mirror, and two overlapping regions can leave a
  doubly-mirrored (positive-determinant) matrix that the test never sees.
- **Bail on `det < 0`.** Surrenders the feature exactly where the region is.

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

`weapon/barrel_fire_origin.cpp` installs two vtable hooks per class:

| Slot | Hook | Job |
|------|------|-----|
| `+0x70` `OverrideAimer` | `hooked_cannon_OverrideAimer` | writes `Aimer::mFirePos` from `mFirePointMatrix` trans, and re-aims `mDirection` while zoomed |
| `+0x8C` `Render` | `hooked_weapon_Render` | brackets the mirrored duplicate draw so it cannot leave a reflected `mFirePointMatrix` behind |

`OverrideAimer` writes **only** `Aimer::mFirePos` — never `mRootPos` (that would
corrupt stance/LOS) and never `mFirePointMatrix` itself (the engine owns it). Its
guards, all falling back to the vanilla aimer: first-person zoom, scope texture up,
null aimer, `0xCDCDCDCD`/zero matrix, a mirrored matrix (a backstop for the Render
hook failing to install), and a fire point more than 5 units from `mRootPos` on any
axis.

`lua/lua_funcs.cpp — DumpAimerInfo` is the diagnostic: dumps aimer and weapon data
to `Bfront2.log`, including a wide byte range at `owner+0x100..+0x300` for struct
exploration.

---

## Zoom Detection — Resolution Chain

```
weapon + 0x6C                → Controllable* (mOwner)
Controllable + 0x160/+0x15C  → bool TargetInfo::mIsAiming (zoom toggle)
weapon + 0xBC                → float Weapon::mZoom        (current magnification)
weapon + 0x64 → +0xA0/+0xA4  → float WeaponClass::mZoomMin / mZoomMax
Controllable + 0x34          → Tracker* (via Trackable::mTracker)
Tracker + 0x14               → bool mIsFirstPersonView
```

Behaviour:
- Scope texture on screen → vanilla aimer, fix fully off
- Unzoomed, any camera → barrel fire origin, vanilla direction
- Zoomed without a scope texture → barrel fire origin + converged direction
- First-person zoom → vanilla aimer (`mFirePointMatrix` goes stale)

`mIsAiming` doubles as the "is this the local player" test: `CheckForZoom` returns
early for AI, so the raycast below runs once or twice a frame at most, however many
WeaponCannons the level holds.

### Scope texture detection

Convergence puts the shot on target but cannot fix how it *looks*: the bolt still
leaves from wherever the idle animation is holding the muzzle, so under a scope you
see a diagonal streak to the target instead of a line down the barrel. Since the
barrel origin buys nothing while you are looking through a scope, the hook switches
off entirely whenever the scope texture is up.

The condition is read from the engine, not rebuilt. `ScopeDisplay::Update`
(modtools `0x683D80`) computes visibility from five terms:

```
CameraManager::IsChaseMode(cameraId)
&& (EntityClass+0x1FC >> 3) & 1
&& Controllable::mIsAiming
&& (Weapon::ZoomFirstPerson(weapon) || Tracker::IsFirstPersonView(tracker))
&& RedCamera::_fZoom > 1.0
```

and stores the result in a bool on the instance. Reimplementing that predicate has
misfired twice in this file already — the `bit 3` test missed SniperScope weapons,
and the raw `Tracker+0x14` read ignores the class camera-mode override — so the
hook reads the engine's own answer instead.

| Build | `ScopeDisplay*` global | Visible flag | Hide |
|-------|------------------------|--------------|------|
| modtools | `0x00BA36D8` | instance `+0x4C9` | `0x683CB0` |
| Steam | `0x01EAF020` | instance `+0x4C9` | `0x633B30` |

The global is a one-element array indexed by camera; PC never allocates past index
0. The instance is `0x520` bytes on modtools and `0x500` on Steam, but only the
trailing `GameSound` members differ — `+0x4C9` and the `BinocularSound` at `+0x4CC`
sit at the same offsets on both.

Note the `mIsAiming` term: `ScopeDisplay::Update` reads it as
`*(byte*)(controllable + 0x58*4)` = `Controllable+0x160`, independently confirming
the modtools offset.

### The zoom stages (Weapon::CycleZoom)

`EntitySoldier::CheckForZoom` (modtools `0x528E00`) is the only writer of
`mIsAiming`, and it just stores what `Weapon::CycleZoom` (modtools `0x61B570`,
Phantom `0x7AD4E0`) returns. That makes zoom a three-state toggle:

| Press | mIsAiming | mZoom | Note |
|-------|-----------|-------|------|
| 1 | true | `mZoomMin` | stage 1 |
| 2 | true | `mZoomMax` | stage 2, only if `mZoomRate == 0` and `mZoomMin != mZoomMax` |
| 3 | false | `mZoomMin` | zoom off |

So `mZoom > mZoomMin` identifies the sniper's second scope stage exactly. When
`mZoomRate != 0` the weapon ramps smoothly instead (`Weapon::UpdateZoom`, driven
by `mControlMove`). Gating the fix on that test was an intermediate step, since the
error scales with magnification and stage 2 is where it screams; the raycast below
replaced it, because the error is present at *every* magnification.

`CheckForZoom` also returns early for AI (`mPlayerId < 0`) and clears `mIsAiming`
for any weapon with `mZoomMax <= 1.0`.

Zoom field offsets are identical on modtools, Steam and Phantom: `Weapon::mZoom`
`+0xBC`, `WeaponClass::mZoomMin/mZoomMax/mZoomRate` `+0xA0/+0xA4/+0xA8`.

### Direction convergence (2026-07-26)

```
O = Aimer::mRootPos   (+0x70)  vanilla fire position, untouched by us
D = Aimer::mDirection (+0x48)  vanilla direction
B = hp_fire world position (the new origin)

t = RayHit(O + D*1, D, 500, ...)      -> fraction of 500
P = O + D * (1 + t*500)               -> what the vanilla shot hits
Aimer::mDirection = normalize(P - B)
```

`mRootPos` is the clean source for `O`: `Aimer::SetSoldierInfo` writes `mFirePos`
and `mRootPos` from the same value and the hook only overwrites `mFirePos`.

The ray starts one unit forward so it clears the shooter's own collision volume,
which is cheaper and less fragile than building an exclude list (that would need the
`GameObject*` behind the `Controllable`, i.e. the unverified `-0x240` arithmetic).
The engine's own aim ray does the same thing, starting at
`mAimStart + eyeDir * (cameraTrackOffset.z + 2)`.

Guards, all of which fall back to "leave the direction alone" rather than throwing
the shot somewhere arbitrary: NaN or out-of-range fraction, hit distance under 2
units (muzzle contact, nothing to correct), degenerate `P - B`, and a correction
larger than ~25 degrees (`dot < 0.9`), which can only mean an input was garbage.

Two cheaper convergence targets were considered and rejected:

- **The reticle.** `ReticuleDisplay::Update` (modtools `0x683270`) draws the
  crosshair at the projection of `mAimStart + aimDir*1024`, not at a raycast. At
  1024 units the angular correction is `barrelOffset/1024`, i.e. nothing. Reading
  those fields in the fire path also produced wildly wrong directions (shots into
  the ground) — they are not a reliable per-frame eye ray there.
- **`TargetInfo::mAimPoint`.** The engine's own third-person convergence target,
  written by `UpdateWeaponAndAimer` in the same frame just before this hook runs,
  but `RayHit` caps it at 60 units from the camera. Converging there fixes close
  range and over-corrects past it, error growing as `barrelOffset * (dist/60 - 1)`
   — worse than doing nothing beyond ~120 units, which is exactly sniper range.

#### CollisionManager::RayHit

```
float RayHit(PblVector3* start, PblVector3* dir, float maxDist,
             CollisionObject** outHit, PblVector3* outNormal,
             GameObject** exclude, int excludeCount, int flags, bool)
```

Returns the hit fraction of `maxDist`; `1.0` means nothing was hit. `outHit` is
written on entry, so it must never be null. `flags == 0` is replaced with `0xBE`.

| Build | Address | Convention |
|-------|---------|------------|
| modtools | `0x42E230` (ILT thunk `0x407581`) | plain `__cdecl`, all nine on the stack, result in `ST(0)` |
| Steam | `0x45E3A0` | LTCG: `ECX` = start, `EDX` = dir, `XMM2` = maxDist, other six pushed, caller-cleans, result in `XMM0` |

Calling the release build through the debug signature shifts every stack argument
by one slot — the exact mistake behind the old aim-assist crash — so Steam goes
through `rayhit_release_thunk`, a naked marshaller.

Flag bits, as far as they are known: `0x80` = terrain, `0x100` = water, and the
per-object categories are filtered inside the TreeGrid callback. Observed masks:
`0x9A` for the engine's aim ray, `0x90` for `Ordnance::Update`'s world sweep,
`0xBE` as `RayHit`'s own default. The hook uses **`0x9A`** — the same mask the
engine uses to resolve `mAimPoint`, since that answers the same question. Anything
the mask does not see (a soldier may be one) converges on the geometry behind it,
which leaves a fraction of the barrel offset rather than all of it.

### Why the intermediate bail keyed on zoom level (2026-07-26)

Sniper shots landed far from the crosshair while scoped, worst when zooming
straight out of an idle pose and fine from a firing pose.

`EntitySoldier::UpdateWeaponAndAimer` (modtools `0x52C980`, Phantom `0x58AD80`)
takes its "direct aim" path only when

```
TargetInfo::mIsAiming && weapon && (Weapon::ZoomFirstPerson(weapon) ||
                                    Tracker::IsFirstPersonView(tracker))
```

Otherwise it uses the third-person camera path, which derives the aim direction
by **converging the fire position onto the aim point**:

```
TargetInfo::mAimPoint = <camera ray, CollisionManager::RayHit capped at 60u>
dir = normalize(mAimPoint - firePos)      // firePos = eye point + stance offset
Aimer::SetSoldierInfo(aimer, firePos, dir)
```

The hook then relocates only the **origin** to `hp_fire`, leaving that direction
untouched, so the shot becomes a ray parallel to the vanilla one, displaced by
the whole barrel-to-eyepoint vector. That displacement is large in an idle/hip
pose (rifle held low and to the side) and small in an aim/fire pose — matching
the reported symptom exactly. Unzoomed the offset is invisible; scope
magnification makes it obvious.

The old gate mirrored the engine's zoom condition, testing
`WeaponClass+0x2B0 bit 3` (`mZoomFirstPerson`). A sniper rifle typically sets
`SniperScope` (**bit 4**, `0x10`) and not `ZoomFirstPerson` (bit 3, `0x08`), so in
third person neither half of the engine condition held, the hook stayed active,
and it wrote an animation-driven origin under a converged direction.

Bit layout of `WeaponClass+0x2B0` (from the Phantom PDB):

| Bits | Field |
|------|-------|
| 0-1 | mFireAnim |
| 2 | mTriggerSingle |
| 3 | mZoomFirstPerson |
| 4 | mSniperScope |
| 5 | mMaxRangeDefault |
| 6 | mInstantPlayFireAnim |
| 7 | mIsOffhand |

The displacement is a fixed distance in world units, so what decides whether it
shows is magnification, not the fact of being zoomed. Two gates were tried and
superseded before the raycast:

1. Bail on `mIsAiming` alone — gave up the barrel origin from the first zoom press
   onward, which on a scoped weapon is most of the time it is aimed.
2. Bail on `mZoom > mZoomMin` — kept stage 1 and dropped only the deep scope, but
   stage 1 still magnifies, so the error was still visible there. Never fixed, only
   made smaller.

Both were treating a symptom. The origin move is what breaks the shot, at every
magnification, so the direction has to move with it.

Caveat on the first-person check: `Tracker::IsFirstPersonView` is not a plain
field read. It first consults the tracked object's class (`[cls+0x14C]`,
1 = forced third person, 2 = forced first person) and only falls back to
`Tracker::mIsFirstPersonView`. The hook still reads the raw `Tracker+0x14` field,
so it can disagree with the engine on objects that force a camera mode.

**Still open (unzoomed):** the parallel-offset error is still there when not
zoomed, since convergence is gated on `mIsAiming` to keep the raycast off every AI
weapon in the level. At 1x it is far below what anyone can see.

**PDB struct shift note:** The PDB places `mTargetInfo` at Controllable+0x144, putting
`mIsAiming` at +0x15C. In the modtools binary, there's an extra 4 bytes at +0x144
(unknown field, always value 1), shifting `mTargetInfo` to +0x148 and `mIsAiming` to
+0x160. Confirmed via byte dump diff across 4 states (3P/1P x zoomed/unzoomed).

---

## Lua API

Control is INI-only (`[Fixes] BarrelFireOriginFix`). The old `SetBarrelFireOrigin`
live toggle is gone — see [Approaches Tried and Rejected](#lua-truthiness-bug-root-cause-of-all-toggle-off-failures)
for the Lua 5.0 truthiness trap it left behind.

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

## Address Reference (base 0x400000)

Everything the installer resolves, plus the RE landmarks. All three retail-family
entries live in `game_addrs::{modtools,steam,gog}`.

| Item | modtools | Steam | GOG |
|------|----------|-------|-----|
| Weapon::OverrideAimer impl | 0x61CEE0 | 0x677780 | 0x678820 |
| Weapon::OverrideAimer thunk | 0x4068DE | *(none — == impl)* | *(none)* |
| WeaponCannon vtable | 0xA52468 | 0x7B057C | 0x7B14F4 |
| WeaponLauncher vtable | 0xA53AE8 | 0x7B12A4 | 0x7B221C |
| ↳ OverrideAimer slot (+0x70) cannon / launcher | 0xA524D8 / 0xA53B58 | 0x7B05EC / 0x7B1314 | 0x7B1564 / 0x7B228C |
| Weapon::Render impl | 0x61DFA0 | 0x679350 | 0x67A3F0 |
| Weapon::Render thunk | 0x4072BB | *(none — == impl)* | *(none)* |
| ↳ Render slot (+0x8C) cannon / launcher | 0xA524F4 / 0xA53B74 | 0x7B0608 / 0x7B1330 | 0x7B1580 / 0x7B22A8 |
| ScopeDisplay* global | 0xBA36D8 | 0x1EAF020 | 0x1EB04D4 |
| CollisionManager::RayHit | 0x42E230 (thunk 0x407581) | 0x45E3A0 | 0x45E3A0 |
| Aimer::SetSoldierInfo | 0x5EE9D0 (thunk 0x402702) | 0x43D290 | 0x43D280 |

Modtools-only landmarks (RE reference, not resolved at runtime):

| Item | Address |
|------|---------|
| EntitySoldier::UpdateWeaponAndAimer | 0x52C980 (thunk 0x40283D) |
| Weapon::ZoomFirstPerson | 0x61B640 (static type check, NOT runtime) |
| sEyePointOffset (3x PblVector3) | 0xACE360 |
| sEyePointRelativeWeaponOffset | 0xACE384 |
| Character array base ptr | 0xB93A08 |
| Max character count | 0xB939F4 |
| Team array ptr | 0xAD5D64 |
| Global class def list | 0xACD2C8 |
| GameLog | 0x7E3D50 |
| HashString | 0x7E1BD0 |

Phantom landmarks for the reflection path (`Battlefront2_Phantom.exe`):

| Item | Address |
|------|---------|
| Weapon::Render | 0x7AE8C0 (thunk 0x412BA7, vtable slot 0x8C) |
| WeaponCannon::Fire | 0x7B5680 |
| EntitySoldier::Render | 0x575380 |
| ↳ FLRenderer::IsReflected call + reflected flags/matrix setup | 0x576001 – 0x5760A6 |
| ↳ Weapon::Render, real / mirrored (main channel) | 0x57844E / 0x57848B |
| ↳ Weapon::Render, real / mirrored (offhand channel) | 0x5784CB / 0x5784FE |
| EntityProp::Render (small readable example of the same pattern) | 0x560DC0 |
| FLRenderer::IsReflected | 0x88DF30 (thunk 0x419F01) |
| FLRenderer::RenderReflections (the reflection *texture* pass) | 0x88F730 |

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

## Retail port (2026-07-08, GOG completed later)

Installed build-aware via `barrel_fire_origin_install()`
(`weapon/barrel_fire_origin.cpp`), called from dllmain's build-aware section, so all
three builds get the same hooks. Addresses are in the table above; the Render slots
were derived by reading the vtables (`+0x70` matching the known OverrideAimer entry
confirms the base) and `tools/port_gog.py code 0x679350` → `0x67A3F0`, score 1.00.

**Struct offsets** — all build-invariant EXCEPT `mIsAiming` (Controllable):

| Field | Modtools | Steam | Source |
|-------|----------|-------|--------|
| mIsAiming (TargetInfo+0x18) | 0x160 | **0x15C** | TargetInfo 0x148→0x144 (game_struct_reference.md) |
| mOwner/mClass/mAimer/mFirePointMatrix | 0x6C/0x64/0x70/0x20 | same | Weapon invariant |
| mTracker (Trackable+0x1C) | 0x34 | 0x34 | Trackable invariant |
| Tracker::mIsFirstPersonView | 0x14 | 0x14 | assumed engine-invariant (verify if FP-scope off) |
| WeaponClass +0x2B0 zoom bit | 0x2B0 | 0x2B0 | WeaponClass invariant |
| Aimer::mFirePos/mRootPos | 0x88/0x70 | same | Aimer invariant |

The hook selects `mIsAiming` via `s_misAimingOff` (set in the installer). The old
reflection guard's `owner-0x11C` read (soldier world Y, assuming
`Entity→Controllable == 0x240`) is gone with the un-mirror it served — the hook no
longer has any unverified build-variant offset.

---

## Files Modified

| File | Purpose |
|------|---------|
| `PatcherDLL/src/weapon/barrel_fire_origin.{hpp,cpp}` | OverrideAimer + Render vtable hooks and build-aware install/uninstall (moved here from lua_hooks 2026-07-08; no Lua dependency) |
| `PatcherDLL/src/lua/lua_funcs.cpp` | `DumpAimerInfo` diagnostic Lua function (modtools-only) |
| `PatcherDLL/src/core/dllmain.cpp` | reads INI toggle; calls `barrel_fire_origin_install` in the build-aware section |
| `PatcherDLL/src/core/game_addrs.hpp` | modtools + steam + gog addresses |
| `docs/RE/barrel-fire-origin.md` | This file |

**Note:** `SetBarrelFireOrigin` (the old live Lua toggle) no longer exists — control
is INI-only (`[Fixes] BarrelFireOriginFix`). The hook was historically parked in
`lua_hooks.cpp` because it began as a modtools Lua experiment; it has no Lua tie and
now lives in its own `weapon/` module.

---

## Known Issues / Future Work

1. **Animation timing — running to fire:** When firing during a running animation, the
   bolt originates from the barrel tip's current animated position (may be far to the
   side) before the fire animation transitions. `mFirePointMatrix` always reflects the
   current animation frame — there's no way to predict the post-transition barrel
   position. Lateral clamping (decomposing into aim-parallel/perpendicular) was tried
   but had no visible effect. Possible future approaches:
   - Only use Y from barrel, keep vanilla X/Z (loses lateral barrel positioning)
   - Fixed offset from rootPos along aim direction (consistent but less realistic)
   - Accept as cosmetic (current approach)

2. **Per-weapon-class control:** Patches WeaponCannon and WeaponLauncher. See
   [Patched classes](#patched-classes) — extending further needs a check that the
   class's `Fire` reads the aimer, not just a vtable address.

3. **Slight aiming offset:** The barrel position is ~0.5 units from the aimer
   position. At close range this causes a small parallax between crosshair and impact.

4. **AI units:** The vtable patch affects all WeaponCannon instances, including AI.
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
