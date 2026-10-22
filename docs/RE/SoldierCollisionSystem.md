# Soldier Collision System

What collision a unit actually has, why ODF-declared primitive collision does not reach
soldier classes, and what would have to change to support attached blocking geometry such
as a riot shield.

Research only. Nothing here is implemented, and the open question at the end is load-bearing
for whether it is worth implementing at all.

All addresses are from the **Phantom** build (`Battlefront2_Phantom.exe`, named PDB) unless
stated otherwise. Nothing has been ported to modtools/Steam/GOG yet.

---

## Summary

The engine already has everything a riot shield needs:

- collision models hold up to **64 bodies** with **8 independent per-body masks**, so a body
  can block ordnance without being targetable;
- soldiers get a **per-instance dynamic collision model** whose bodies from index 2 upward are
  copied straight out of a per-stance class collision model (six engine-built bodies per
  stance today, eight when prone, see [Infantry collision shapes](#infantry-collision-shapes));
- damage is already resolved **per body** through a multiplier table, which is how headshots
  work.

What is missing is any ODF route into those three things from a soldier class. The property
handlers exist, on `EntityGeometryClass`, and `EntitySoldierClass::SetProperty` never reaches
them. The one soldier path that does load named collision primitives is hardcoded to the
Acklay and bundled with four unrelated behaviours.

---

## Structures

`CollisionModel` (332 = `0x14C` bytes):

| Offset | Field |
|---|---|
| `+0x04` | `mDenyFlyerLand` |
| `+0x05` | `mDeathOnFlyerLand` |
| `+0x06` | `mIsFlyerFlag` |
| `+0x07` | `mUseVehicleCollisionAgainstFlyers` |
| `+0x08` | `CollisionBody* mBody[64]` |
| `+0x108` | `int mNumBodies` |
| `+0x10C` | `CollisionMask mSoftMask` |
| `+0x114` | `CollisionMask mRigidMask` |
| `+0x11C` | `CollisionMask mStaticMask` |
| `+0x124` | `CollisionMask mTerrainMask` |
| `+0x12C` | `CollisionMask mOrdnanceMask` |
| `+0x134` | `CollisionMask mTowCableMask` |
| `+0x13C` | `CollisionMask mTargetableMask` |
| `+0x144` | `CollisionMask mFlyingOrdnanceMask` |

`CollisionMask` is `uint[2]` - a **64-bit bitmask over the body array**, not a category enum.
A body participates in a collision class iff its bit is set in that mask, and the eight masks
are fully independent. That is the whole "blocks bullets but is not a target" mechanism:
set the body's bit in `mOrdnanceMask` and leave it clear in `mTargetableMask`.

`CollisionBody` (96 bytes): `mMatrix` `+0x10`, `mName` (hash) `+0x50`, `mMask` (uchar) `+0x54`,
`bIsMesh` `+0x55`, **`mParentHashName` `+0x58`**, `profileHashName` `+0x5C`.

`CollisionPrimitive` (32 bytes): `mName` `+0x04`, `mRadius` `+0x08`, `mHeight` `+0x0C`,
`mWidth` `+0x10`, `mDepth` `+0x14`, `mType` `+0x18`, `mMeshCollisionRadius` `+0x1C`.

`CollisionObject` (136 bytes): `mCollisionModel` `+0x48`, `mParent` `+0x4C`, `mMass` `+0x50`,
`mAABB` `+0x54`, `mProcedurallyAnimated` bit `+0x6C`, `mLastPosition` `+0x70`.

### CollisionObjectType

Values recovered from the `EntityGeometryClass::Add*Collision` handlers, each of which passes
the type to `CollisionModel::SetCollisionMask` (`0x4B98F0`):

| Value | Type | Mask written |
|---|---|---|
| `0x0002` | `COLL_SOFT` | `mSoftMask` |
| `0x0008` | `COLL_RIGID` | `mRigidMask` |
| `0x0010` | `COLL_STATIC` | `mStaticMask` |
| `0x0080` | `COLL_TERRAIN` | `mTerrainMask` |
| `0x0400` | `COLL_ORDNANCE` | `mOrdnanceMask` |
| `0x2000` | `COLL_TARGETABLE` | `mTargetableMask` |

`COLL_ASTEROID` also routes to `mRigidMask`; `COLL_TOWCABLE` to `mTowCableMask`. Their
numeric values were not derived. Anything unrecognised falls through to `mSoftMask`.

---

## The collision ODF properties, and why soldiers never see them

`EntityGeometryClass::SetProperty` (`0x538E80`) owns all of them. Each takes the **name of a
collision primitive**, resolves it to a body index with `CollisionModel::GetMaskId`
(`0x4B8E80`, a linear search over `mBody[]` by name hash), and ORs that bit into the mask:

| ODF property | PblHash | Handler | Mask |
|---|---|---|---|
| `SoldierCollision` | `0x5DFDC07F` | `0x537630` | soft |
| `VehicleCollision` | `0xDE5365A1` | `0x5378A0` | rigid |
| `BuildingCollision` | `0x9828F43F` | `0x537490` | static |
| `TerrainCollision` | `0xAFE693CE` | `0x5377D0` | terrain |
| `OrdnanceCollision` | `0xFB2BDF07` | `0x537560` | ordnance |
| `TargetableCollision` | `0xD23DBAF8` | `0x537700` | targetable |
| `HitLocation` | `0xCED77479` | inline `0x539820` | (damage table, see below) |

A value hashing to `0xADA7AFDB` (`"none"`, case-insensitive) clears the mask instead. A second
sentinel `0x5C6E1222` does the same; its string is unresolved.

An unknown name logs `Entity "%s" unknown <kind> collision "%s"` and returns false.

**`EntitySoldierClass::SetProperty` (`0x57ABC0`) never reaches this function.** Its default
branch (`0x57CE3E`) walks a chain of sub-object handlers and then tail-calls
**`GameObjectClass::SetProperty` (`0x5D5730`)** directly, skipping `EntityGeometryClass`
entirely. That is the root cause of "you cannot give units primitive collision" - not a
missing capability, a missing property route.

`EntitySoldier` *is* an `EntityGeometry` at runtime (`EntitySoldierClass::Build` calls
`EntityGeometry::BuildEffects` on the instance); it is only the class-side property dispatch
that skips the layer.

---

## Per-body damage: already live on soldiers

`GameObject::GetDamageMultiplier(bodyIndex)` (`0x5D47D0`, virtual, ~31 vtables) reads a table
on the class:

| Offset | Field |
|---|---|
| `class+0x2DC` | `{ uint bodyIndex; float multiplier; }[]`, stride 8 |
| `class+0x2E0` | entry count |

It returns the multiplier for the matching body, or `1.0` when `bodyIndex == -1` or no entry
matches. While walking, it checks each entry's body against `mOrdnanceMask` and, if the body
is not in it, draws `RedDebug::Printf3D("WARNING!\n Critical Hit Location primitive\n not
tagged as ordnance collision!")` in world space - so the engine considers "hit location" and
"ordnance collider" to be two tags on the same body.

`CombatUtil::HasCriticalHitLocation` (`0x4C8EB0`) simply reports whether any multiplier `> 1.0`.

`EntityGeometryClass::SetProperty` parses `HitLocation` with format `"%s %f"`
(primitive name, multiplier; multiplier defaults to `1.0`) and **caps the table at 10 entries**
(`cmp dword ptr [edi+0x2E0], 0xA` at `0x539842`).

**Soldiers already use this.** `EntitySoldierClass::PostReadSetup` at `0x573E19`:

```c
if (class->mNumHitLocations == 0) {                  // +0x2E0
    class->mHitLocations = malloc(8);                // +0x2DC, one entry
    class->mHitLocations[0].bodyIndex  = 0;
    class->mHitLocations[0].multiplier = 3.0f;       // 0x40400000
    class->mNumHitLocations++;
}
```

That is the headshot multiplier: body 0 of a soldier takes 3x damage. It proves the damage
path resolves a real body index for soldiers, which is what makes a **`0.0` multiplier** a
viable "blocks, but is not hittable" tag.

---

## Soldier collision models

`EntitySoldierClass` carries **three** adjacent `CollisionModel`s, one per stance
(stride `0x14C` = `sizeof(CollisionModel)`):

| Offset | Stance |
|---|---|
| `+0xD50` | stand |
| `+0xE9C` | crouch |
| `+0xFE8` | prone |

`EntitySoldier::SetCollisionStance(stance)` (`0x57A1C0`) builds the per-instance model:

```c
if (this->mAcklayData) return;                       // acklay skips the stance system
// bodies 0 and 1 = head and torso; only their positions are rewritten per stance
for (i = 2; i < GetNumBodies(&this->mDynamicCollisionModel); i++)
    SetBody(&this->mDynamicCollisionModel, i,
            GetBody((CollisionModel*)((char*)mClass + stanceOffset), i));
CollisionObject::SetCollisionModel(&this->CollisionObject, &this->mDynamicCollisionModel);
```

The head and torso writes are the translation rows of instance bodies 0 and 1. With
`s = mCollisionRootScale`:

| Stance | Head (x, y, z) | Torso (x, y, z) |
|---|---|---|
| 0 stand | `(0, s * 1.65, 0.16)` | `(0, s * 1.15, 0)` |
| 1 crouch | `(0, s * 1.2, 0.25)` | `(0, s * 0.7, 0)` |
| 2 prone | `(0, s * 0.28, s * 0.75)` | `(0, 0, 0)` |

These match the positions `PostReadSetup` gives the same bodies (below), except prone, where
`PostReadSetup` also offsets the head by `x = -0.07`. The sizes come from the class model and
are never changed here.

**Bodies 0 and 1 are the head and torso. Everything from index 2 upward is copied verbatim
from the class's per-stance model into every instance, per stance.** Every non-acklay soldier
class already has four or six bodies at index 2+ (next section). Extra attached geometry would
be appended after them and gets per-stance shapes for free.

### Infantry collision shapes

`EntitySoldierClass::PostReadSetup` (modtools `0x531880`) builds every body procedurally. No
ODF or mesh data is involved apart from three scale fields, all defaulting to `1.0` in the
class constructor:

| Field | Modtools offset | Symbol |
|---|---|---|
| `s` | `class+0x136C` | `mCollisionRootScale` |
| `H` | `class+0x11F8` | `m_fCollisionHeadScale` |
| `T` | `class+0x11FC` | `m_fCollisionTorsoScale` |

Stance models on modtools are at `class+0xCB0` / `+0xDFC` / `+0xF48` (stride `0x14C`), and the
dead-body model is at `+0x1094`.

Primitive conventions, from `CollisionPrimitive::SetAACylinder` / `SetBox` and
`CollideAACylinder`:

- **Cylinders** are upright (axis aligned). The second argument is the **full height**, centred
  on the body position. `CollideBoxCylinder` is passed `height * 0.5` as the half height.
- **Boxes** take **half extents** `(width, height, depth)`. The bounding radius is
  `sqrt(w*w + h*h + d*d)`.
- Positions below are body centres `(x, y, z)` relative to the unit origin. +Z is probably
  forward (prone head sits at `+0.75`), not verified.

Head radius = `0.17 * max(s, 1) * H`. Torso radius = `0.4 * s * T`. **No other radius is
scaled**: `s` stretches heights and Y positions only, so a large `CollisionRootScale` makes a
unit taller, not wider.

**Stand** (`s = 1` values in brackets)

| Body | Shape | Size | Centre |
|---|---|---|---|
| `coll_head` | sphere | r `0.17` | `(0, 1.65s, 0.16)` |
| `coll_torso` | sphere | r `0.40` | `(0, 1.15s, 0)` |
| `coll_body` | cylinder | r `0.35`, h `1.1s` | `(0, 0.55s, 0)` (spans 0 to 1.10) |
| `main_body` | cylinder | r `0.57`, h `1.65s` | `(0, 0.9s, 0)` (spans 0.075 to 1.725) |
| `coll_body_lower` | sphere | r `0.57` | `(0, 0.57, 0.15)` |
| `coll_body_upper` | sphere | r `0.57` | `(0, 1.9s - 0.42, 0.15)` [1.48] |

**Crouch**

| Body | Shape | Size | Centre |
|---|---|---|---|
| `coll_head` | sphere | r `0.17` | `(0, 1.2s, 0.25)` |
| `coll_torso` | sphere | r `0.40` | `(0, 0.7s, 0)` |
| `coll_body` | cylinder | r `0.35`, h `0.7s` | `(0, 0.35s, 0)` (spans 0 to 0.70) |
| `main_body` | cylinder | r `0.45`, h `1.45s` | `(0, 0.65s, 0)` |
| `coll_body_lower` | sphere | r `0.57` | `(0, 0.57, 0.15)` |
| `coll_body_upper` | sphere | r `0.57` | `(0, 1.6s - 0.42, 0.15)` [1.18] |

`coll_body_upper` falls back to `(0, 0.627, 0.15)` (lower sphere plus `0.057`) when
`1.9s - 0.57` (stand) or `1.6s - 0.57` (crouch) is `<= 0.57`. Its final position is also
copied into the camera safe point of the stance's camera settings.

**Prone** (`k = 0.8s - 0.1925`, `0.6075` at `s = 1`)

| Body | Shape | Size | Centre |
|---|---|---|---|
| `coll_head` | sphere | r `0.17` | `(-0.07, 0.28s, 0.75s)` |
| `coll_torso` | sphere | r `0.01` (effectively off) | `(0, 0, 0)` |
| `coll_body` | box | half `(0.25, 0.2, 0.87s)` = 0.5 x 0.4 x 1.74 | `(0, 0.2, -0.25s)` |
| `main_body` | cylinder | r `0.8`, h `0.45s` | `(0, 0.25s, 0)` |
| `coll_body_medium` | sphere | r `0.8` | `(0, 0.72, 0.333k)` |
| `coll_body_upper` | sphere | r `0.385` | `(0, 0.3465, k)` |
| `coll_body_lower` | sphere | r `0.385` | `(0, 0.3465, -k)` |
| `coll_body_medium2` | sphere | r `0.385` | `(0, 0.3465, -0.333k)` |

The prone `coll_body_medium` sphere reaches `y = 1.52`, well above a prone unit. That is what
the code builds.

The **dead-body model** reuses the four prone spheres (`coll_body_medium`, `_upper`,
`_lower`, `_medium2`) with the same sizes. It collides with terrain and statics only. Soft,
rigid and ordnance masks are cleared.

**Mask assignment** (per stance, by body name hash):

| Mask | Bodies | Meaning |
|---|---|---|
| ordnance (`0x400`) | `coll_head`, `coll_torso`, `coll_body` | what bullets hit |
| soft (`0x2`) | `main_body` | soldier/vehicle push volume |
| rigid (`0x8`), static (`0x10`), terrain (`0x80`) | `coll_body_lower`, `coll_body_upper`, `coll_body_medium`, `coll_body_medium2` | world collision |

| Body | PblHash |
|---|---|
| `coll_head` | `0xE8B9E01A` |
| `coll_torso` | `0xCA25B9F3` |
| `coll_body` | `0x34FCD074` |
| `main_body` | `0xFE858165` |
| `coll_body_lower` | `0x9458FB2E` |
| `coll_body_upper` | `0x2C31EC5B` |
| `coll_body_medium` | `0x0DB3CAEA` |
| `coll_body_medium2` | `0x6A085208` |

Body 0 (`coll_head`) is also the body the default `HitLocation` entry points at, which is the
3x headshot multiplier.

---

## `IsAcklay` - the one soldier path that does load primitives

`IsAcklay` = PblHash `0xC372B928`, stored as **bit 5 (`0x20`) of `EntitySoldierClass+0x95C`**,
written at `0x57C70A`. Nine readers, covering five unrelated behaviours:

| Site | Function | Effect |
|---|---|---|
| `0x572D38` | `EntitySoldierClass::PostReadSetup` | loads collision primitives into `class+0xD50` |
| `0x56B070` | `EntitySoldierClass::Build` | `AddSoftToCollisionManager` -> `AddRigidToCollisionManager` |
| `0x56702A` | `EntitySoldier::EntitySoldier` | allocates `AcklayData` (`0x13F0` = 5104 bytes) into `entity+0x45C` |
| `0x56AE32` | `EntitySoldier::BuffDefense` | immunity |
| `0x56AEA2` | `EntitySoldier::BuffHealth` | immunity |
| `0x56AF12` | `EntitySoldier::BuffOffense` | immunity |
| `0x56D604` | `EntitySoldier::DebuffDamage` | immunity |
| `0x587FEB` | `EntitySoldier::UpdateBuffTimers` | immunity |
| `0x5890EE` | `EntitySoldier::UpdateIndirect` | skips an impulse path |

### The primitive names are hardcoded

`PostReadSetup`, `0x572D91`-`0x572E15`:

```c
for (i = 0; ; i++) {
    snprintf(buf, 0x7F, "geo_inf_acklay%d", i);         // format string at 0x9F5504
    prim = Find(0xABD384, 0x800, PblHash(buf));         // global collision-primitive registry
    if (!prim) break;
    if (!CollisionModel::AddPrimitive(&class->mCollisionModel /* class+0xD50 */, prim)) {
        RedWarning(RED_SEVERITY_ERROR, "EntitySoldier.cpp", 10151);
        LogMessage("EntitySoldier:Acklay - too many collision primitives max=64");
    }
}
```

Note it writes only into `class+0xD50` (the **stand** model), and `SetCollisionStance`
early-returns for acklay entities anyway, so the acklay uses that model directly rather than
through the per-stance copy.

### The terrain alignment is a separate site

The behaviour that makes acklay units hug the terrain is the `AcklayData` allocation in the
**constructor**, not the collision loading in `PostReadSetup`. `AcklayData` (5104 bytes) is:

| Offset | Field |
|---|---|
| `+0` | `GameSound mFootSound[2]` |
| `+40` | `float mFeetElevation[4]` |
| `+64` | `ZephyrSkeleton<32> mZSkeleton` |
| `+2128` | `ZephyrPoseStatic<32> mZPoseStatic` |
| `+3028` | `RedPose mPose` |
| `+4060` | `RedPose mAnimatedPose` |
| `+5092` | `bool bHasAnimatedPose` |

Four feet, each with its own terrain elevation sample. Separately,
`SoldierAnimatorClass::SetupBodyMasks` (`0x75D4D0`) picks the acklay upper/lower body joint
split by testing for **joint hash `0xB70E2184`** in the skeleton, falling back to
`auiNormalLowerBodyBoneHash[10]` when absent - so the animation half keys off the *skeleton*,
not off `IsAcklay`.

**The collision sites and the terrain-alignment site are cleanly separable.** Setting the
collision bits without allocating `AcklayData` is a one-site distinction.

### Related: the engine already warns about soldier geometry collision

`EntitySoldierClass::SetProperty`, `GeometryName` (`0x47C86B4A`), at `0x57B884`: after loading
the model it looks it up in the collision registry and, if the geometry carries collision,
logs `"Soldier %s has geometry collision"` (severity 2, `EntitySoldier.cpp:10402`). It is only
a warning - the collision is not consumed by anything on the soldier path.

---

## Options

1. **New ODF property for custom primitives.** Mirror the acklay loop with a per-class name
   prefix instead of `geo_inf_acklay%d`, writing into all three stance models rather than just
   stand. Everything downstream (`SetCollisionStance`'s copy loop) is already wired.
2. **Route the mask properties to soldiers.** Either chain `EntitySoldierClass::SetProperty`'s
   default into `EntityGeometryClass::SetProperty`, or intercept the six collision hashes
   before the fallthrough. Gives per-body ordnance/targetable control, which is what makes a
   body block without being aimed at.
3. **Expose `HitLocation` to soldiers.** The table and its reader already work on soldiers; a
   `0.0` multiplier is the "non-hittable" tag. Watch the 10-entry cap and the fact that
   `PostReadSetup` seeds entry 0 with the headshot multiplier.
4. **Decouple `IsAcklay`.** A second flag bit taking only the `PostReadSetup` and `Build`
   sites, leaving `AcklayData`, buff immunity and `UpdateIndirect` on the real flag. Only
   needed if option 1 is not taken.

1 + 2 + 3 is a working riot shield, assuming the open question below resolves favourably.

---

## Open question that decides the whole thing

**Can a soldier collision body follow an animated bone?**

`CollisionBody` has `mParentHashName` (`+0x58`) and `CollisionObject` has a
`mProcedurallyAnimated` bit, so the engine has the concept. But every stock soldier body is
built with a fixed matrix relative to the unit origin, and `SetCollisionStance` only moves the
head and torso between three fixed per-stance positions. So the stock shapes are object-space
volumes, not bone-driven ones.

If soldier collision bodies do not track the skeleton, a shield volume would be a fixed box
relative to the unit's origin - acceptable for a unit locked in a shield-block stance, wrong
the moment the arm animates. **This should be settled before any of the options above are
built.** The obvious probe is `CollisionModel::GetWorldMatrix` (called from
`GetDamageMultiplier` at `0x5D48B7`) plus whatever consumes `mParentHashName`.

Three smaller unknowns:

- Whether the instance's `mDynamicCollisionModel` inherits the class model's masks.
  `SetCollisionStance` copies **bodies** via `SetBody`; the eight masks are separate fields on
  `CollisionModel` and nothing in that function touches them.
- Whether soft-vs-rigid registration (`EntitySoldierClass::Build`) matters once extra bodies
  exist, or whether bodies 2+ ride the existing soft registration fine. Making soldiers rigid
  is the acklay behaviour and its effect on movement, AI and pathing is untested.
- How a modder gets named primitives into the global registry at `0xABD384` that the acklay
  loop reads from - i.e. what the `.msh` / `.lvl` collision export has to emit for a name like
  `geo_inf_acklay0` to resolve.

---

## Address table (Phantom)

| Symbol | Address |
|---|---|
| `EntitySoldierClass::SetProperty` | `0x57ABC0` |
| `EntitySoldierClass::PostReadSetup` | `0x572A90` (modtools `0x531880`) |
| `EntitySoldierClass::Build` | `0x56AF70` |
| `EntitySoldier::EntitySoldier` | `0x565F80` |
| `EntitySoldier::SetCollisionStance` | `0x57A1C0` |
| `EntityGeometryClass::SetProperty` | `0x538E80` |
| `GameObjectClass::SetProperty` | `0x5D5730` |
| `GameObject::GetDamageMultiplier` | `0x5D47D0` |
| `CombatUtil::HasCriticalHitLocation` | `0x4C8EB0` |
| `CollisionModel::SetCollisionMask` | `0x4B98F0` |
| `CollisionModel::GetCollisionMask` | `0x4B8CB0` |
| `CollisionModel::GetMaskId` | `0x4B8E80` |
| `CollisionModel::GetNameFromMaskId` | `0x4B8EC0` |
| `CollisionModel::AddPrimitive` | `0x4B76A0` (thunk `0x403CC9`) |
| `CollisionPrimitive::SetAACylinder` | `0x4BFE80` |
| `CollisionPrimitive::SetBox` | `0x4BFEC0` |
| `CollisionPrimitive::CollideAACylinder` | `0x4BAAC0` |
| `SoldierAnimatorClass::SetupBodyMasks` | `0x75D4D0` (thunk `0x407D56`) |
| collision-primitive registry | `0xABD384` |
| `"geo_inf_acklay%d"` | `0x9F5504` |

---

## Related

- [`docs/RE/EntityCarrierSystem.md`](EntityCarrierSystem.md) - the `p_`-prefixed mesh primitive
  convention, the auto-generated bounding-capsule fallback, and `mFlyingCollisionModel`.
- [`docs/RE/barrel-fire-origin.md`](barrel-fire-origin.md) - `CollisionManager::RayHit`
  signature and the flags/exclude arguments.
