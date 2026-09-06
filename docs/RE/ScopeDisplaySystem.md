# ScopeDisplay — the weapon scope overlay

How the zoomed-in scope image is built, where its texture comes from, and the spec
for two new ODF properties: `ScopeTextureFull` and `ScopeModel`.

Addresses are unrelocated (imagebase `0x400000`).

| Item | modtools | Steam | GOG |
|---|---|---|---|
| `ScopeDisplay*` global (1-elem array, PC only ever fills index 0) | `0xBA36D8` | `0x1EAF020` | `0x1EB04D4` |
| `ScopeDisplay::ScopeDisplay` | `0x6839D0` | *unresolved* | *unresolved* |
| `ScopeDisplay::Update` | `0x683D80` | *unresolved* | *unresolved* |
| `ScopeDisplay::Show` | `0x683C00` | *unresolved* | *unresolved* |
| `ScopeDisplay::Hide` | `0x683CB0` | `0x633B30` | *unresolved* |
| `ScopeDisplay` alloc (`new 0x520`) | `0x683C80` | *unresolved* | *unresolved* |
| `WeaponClass::SetProperty` impl | `0x61E6C0` | *unresolved* | *unresolved* |
| `WeaponClass` vtable (`SetProperty` = slot `+0x18`) | `0xA51DF4` | *unresolved* | *unresolved* |
| Texture find by hash | `0x66F3A0` | *unresolved* | *unresolved* |
| `Red3DModelElement::Red3DModelElement` | `0x837FE0` | *unresolved* | *unresolved* |
| `Red3DModelElementLite::SetModel(char*)` | `0x839F00` | `0x6D7010` | `0x6D80B0` |
| ↳ by-hash tail | `0x839EA0` | `0x6D6FC0` | *unresolved* |
| `RedInterfaceScreen::s_screenFull` W / H | `0xE5B508` / `0xE5B50C` | *unresolved* | *unresolved* |
| HUD master enable flag | `0xAD7224` | *unresolved* | *unresolved* |
| Zoom-blur alpha array (per viewport) | `0xCF6254` | *unresolved* | *unresolved* |
| Zoom-blur render pass | `0x789840` (vtable `0xA6A090`) | *unresolved* | *unresolved* |
| `RedModel` hash table (2048 entries) | `0xD4D964` | *unresolved* | *unresolved* |

The Steam/GOG column is deliberately mostly empty — only the two `SetModel` entries
were already resolved, by the loading-screen work. Everything else is a port task,
see [Per-build work](#per-build-work).

---

## The overlay is one quadrant, mirrored four ways

`ScopeDisplay` is a singleton (`0x520` bytes) holding one `RedScreenGroupElement`
`mGroupWeaponScope` (`+0x20`) and **four** `RedBitmapElement`s
`mBitmapWeaponScope[4]` (`+0xC0`, stride `0x100`).

The constructor sizes each bitmap to `screenW/2 x screenH/2`, anchors it to a
different corner of the group origin, then mirrors its UVs:

| element | offset | `SetSize` align (x, y) | `SetUV(uL, vT, uR, vB)` | screen quadrant |
|---|---|---|---|---|
| 0 | `+0x0C0` | 2, 2 | `0, 0, 1, 1` | top-left |
| 1 | `+0x1C0` | 0, 2 | `1, 0, 0, 1` | top-right, u mirrored |
| 2 | `+0x2C0` | 2, 0 | `0, 1, 1, 0` | bottom-left, v mirrored |
| 3 | `+0x3C0` | 0, 0 | `1, 1, 0, 0` | bottom-right, both mirrored |

`RedScreenElement::SetSize(w, h, alignX, alignY)` is vtable slot `+0x48`
(mt `0x838E80`). Align semantics, read off that function:

| align | rect spans |
|---|---|
| 0 | `0 .. +size` |
| 1 | `-size/2 .. +size/2` (centred) |
| 2 | `-size .. 0` |

The four quadrants therefore cover `[-W/2, +W/2] x [-H/2, +H/2]` around the group
origin, which makes the group origin the **screen centre**. The ctor never calls a
position setter, so that is the class default; do not re-derive it from
`RedScreenGroupElement`'s `m_fScreenRelativeX/Y = 0.0`, which belongs to a different
screen's convention — see [[loadscreen_team_model_icons]] for how that one bites.

**So the ScopeTexture is only the top-left quarter of the scope**, mirrored across
both axes. Verified against the art rather than inferred: `weapon_scope.tga` is
512x512 32-bit, opaque in its top-left corner with a quarter-arc sweeping down to a
fully transparent bottom-right corner — and element 0 is the unmirrored one, so that
transparent corner lands on screen centre.

Two consequences:

- Scope art must be symmetric about both axes. Asymmetric scopes are impossible.
- Each quad is stretched to half the screen **per axis independently**, so a circular
  aperture becomes an ellipse at 16:9. That is the source of BF2's oval scope, and it
  cannot be fixed from the texture.

### Vertex layout

`RedBitmapElement` is 256 bytes. `m_vertices` is `BitmapVertexT[4]` at `+0x88`,
stride `0x1C`: `x, y, z, u0, v0, u1, v1` at `+0, +4, +8, +0xC, +0x10, +0x14, +0x18`.

| vertex | position |
|---|---|
| 0 | left, top |
| 1 | left, bottom |
| 2 | right, top |
| 3 | right, bottom |

`SetUV` (mt `0x839220`) is a plain function, **not** a vtable slot, and writes only
`u0`/`v0`. `SetRect` (slot `+0x4C`, mt `0x838E20` → `0x839130`) writes only `x`/`y`,
with a `+0.5` texel-alignment offset. **Neither touches the other's fields**, so UVs
survive a resize and the call order does not matter.

That also means UVs can be written directly at `elem + 0x88 + i*0x1C + 0xC/0x10`
instead of through `SetUV`, which removes one address from the per-build port list.

`m_pInterfaceBitmapShader` is at `+0xF8`. `SetTexture` (slot `+0x50`, mt `0x8391E0`)
lazily creates it via slot `+0x44` (mt `0x839090`, a `pcRedShader::Create` with the
`"interface"` render type and flags `0x202`) and stores the texture hash at
`shader + 0x1C`.

---

## Where the texture comes from

`WeaponClass::mScopeTexture` is at **`+0x74`**, set in `WeaponClass::SetProperty`
(mt `0x61E6C0`) from the ODF property `ScopeTexture = "name"`:

```c
if (param_1 == ScopeTexture) {          // hash 0x32E2A37A
    PblHash::PblHash(&local_118, value);
    this->mScopeTexture = *hash;        // raw hash, NO validation at load time
}
```

The `WeaponClass` ctor (mt `0x621010`) defaults it to `0xF6BCFA8E`, which is
`PblHash("weapon_scope")`.

Resolution happens **per frame** in `ScopeDisplay::Update`:

```c
if (gHudEnabled && weapon) {                              // 0xAD7224
    found = TextureFind(weapon->mClass->mScopeTexture);   // 0x66F3A0
    hash  = found ? weapon->mClass->mScopeTexture : 0xF6BCFA8E;
    if (hash != this->mScopeTexture) {
        this->mScopeTexture = hash;
        for (i = 0; i < 4; i++) bitmap[i]->SetTexture(hash);   // slot +0x50
    }
    if (this->mScopeTexture) group->Enable(true);              // slot +0x04
}
```

`TextureFind` is `PblHashTableCode::_Find(table@0xD4F994, 0x2000, hash)` and also ORs
bit 0 into the hit, i.e. it re-marks the texture as in-use every frame while scoped.
A typo'd or unloaded `ScopeTexture` **silently** falls back to `weapon_scope` — there
is no warning anywhere in this path.

### Stock usage

Read `assets/sides`, not `data/Sides` — the modtools `data` tree only ships the `tur`
side, and reading it gives a badly wrong picture of stock usage.

| texture | count | who |
|---|---|---|
| `weapon_scope` | default when `ScopeTexture` is absent | standard blasters, pistols |
| `weapon_scope2` | 8 | bowcaster, rocket/mortar launcher, sabre throw, mind trick, invisibility, arccaster, gunship rapid ball |
| `weapon_scope3` | 4 | `com_weap_inf_sniper_rifle`, `com_weap_award_sniper_rifle`, both target pistols |
| `weapon_scope4` | 29 | every `tur_weap_*`, every `com_weap_veh_*`, ATAT chin cannon |

All four are 512x512 32-bit and munged via `Common/ingame.req`. The infantry ones
carry ~40% surround alpha (104/255); `weapon_scope4` is fully opaque (255), which is
why turret scopes black out completely.

`tur_weap_spa_imp_guided_rocket.odf:9` has a stray `"weapon_scope4"s`.

---

## The blur is a separate pass with its own textures

`Show`/`Hide` write `0xFF`/`0` into `gScopeBlurAlpha[viewport]` (`0xCF6254`). A render
pass at `0x789840` (vtable `0xA6A090`; Shader Patch calls this `fixedfunc_zoom_blur`,
triggered when it follows `filtercopy`) reads that alpha and composites:

- **`blurmask`** (`0xF745F270`) — `Common/effects/blurmask.tga`, 256x256 8-bit
  (`-8bit` option), the alpha mask deciding where the screen blurs around the scope.
- **`white`** (`0xDE020766`) — degraded fallback when `DevCaps2 & 0x10` is unset or
  shadow quality is 0; darkens instead of blurring.

Neither is settable per weapon, and neither is affected by the two new properties
below — the blur is driven purely by the visible flag, so a model-based scope still
gets the stock blur ring.

---

## Visibility predicate

Five terms, all required (mt `0x683D80`):

```
CameraManager::IsChaseMode(cameraId)
&& (GameObject+0x1FC >> 3) & 1
&& Controllable::mIsAiming (+0x160)
&& (Weapon::ZoomFirstPerson(w) || Tracker::IsFirstPersonView(t))
&& RedCamera::_fZoom > 1.0
```

**Correction to existing notes.** `barrel-fire-origin.md` and
`PatcherDLL/src/weapon/barrel_fire_origin.cpp` both call the `+0x1FC` term
`EntityClass+0x1FC`. It is not a class field. At mt `0x683DD5` the code calls
`Trackable::GetGameObject` — an `adjustor{600}` thunk (`0x52B7C0` →
`EntitySoldier::GetGameObject(this - 0x258)`) — and reads `+0x1FC` on the returned
`GameObject*`. That is the same `Damageable::mFlags` "alive" bit already documented in
`droideka_death_anim_fix.cpp:67` and `shield_channel_fix.cpp:89`. The barrel hook
reads the engine's cached bool, so its behaviour is unaffected; only the comment is
wrong. **Not yet corrected in those two files.**

`Weapon::ZoomFirstPerson` (mt `0x61B640`) returns true for the `ZoomFirstPerson` ODF
bit (`WeaponClass+0x2B0` bit 3) **or** whenever the owner's GameObject type is not 1
or 4. On foot that is always true, which is why every zoomable infantry weapon gets a
scope overlay, not just snipers.

The whole thing is gated on the HUD master flag (`0xAD7224`, toggled by `0x447DC0`),
so hiding the HUD hides the overlay — but not the blur, which reads its own alpha.

## Scriptable sounds

`Lua_Callbacks::SetSoundEffect` (mt `0x47F4E0`) has three keys targeting
`ScopeDisplay + 0x4CC` (`BinocularSound`):

| key | hash |
|---|---|
| `ScopeDisplayZoomIn` | `0x4D9F349B` |
| `ScopeDisplayZoomOut` | `0x414B5348` |
| `ScopeDisplayAmbient` | `0x651E07AD` |

Zoom in/out fire on `RedCamera::_fZoom` crossing, latched by `mIsZooming` (`+0x514`).

---

# Spec: two new ODF properties

Both live on `WeaponClass`, and both are **opt-in by data**. A weapon that declares
neither keeps the vanilla path byte for byte. Per [[hud_new_event_no_ini_toggle]] and
[[feedback_no_ini_for_crash_fixes]], **neither gets an INI toggle** — the ODF property
is the toggle. The hook must bail immediately when no class in the level declared
either property, so mods that do not use this pay one pointer test per frame.

| property | hash | type |
|---|---|---|
| `ScopeTextureFull` | `0xB7D234B3` | int, 0/1 |
| `ScopeModel` | `0x1C5079E8` | string, model name |

## Shared plumbing

**Storage.** `WeaponClass` is `0x304` bytes with no free field, so use a DLL-side
`std::unordered_map<const void* /*WeaponClass**/, ScopeExt>` where

```c
struct ScopeExt {
    bool     full_texture;      // ScopeTextureFull
    uint32_t model_hash;        // ScopeModel, 0 = unset
};
```

Weapon classes are per-game-state ([[weapon_class_factory]]), so **clear the map on
level unload** or it dangles.

**Property capture.** Detour the `WeaponClass::SetProperty` *impl* (mt `0x61E6C0`),
not a vtable slot. Subclasses (`WeaponCannonClass` and friends) chain into this impl,
so one detour catches every weapon class — the opposite of the barrel-fire case, which
needed vtable scoping precisely because it wanted *only* two classes. Match the two
hashes, tail-call the original for everything else.

**Current weapon.** `ScopeDisplay::Update` derives it as:

```
Character[localIdx]  (base 0xB93A08, count 0xB939F4, stride 0x1B0)
  -> +0x150 mRemote, else +0x14C mVehicle, else +0x148 mUnit   (Controllable*)
  -> ctrl->vtbl[+0x3C](0)   GetWeaponIndex
  -> ctrl->vtbl[+0x40](idx) GetWeapon                          (Weapon*)
  -> weapon->+0x64 mClass                                      (WeaponClass*)
```

Replicate that in the hook rather than trying to catch the class mid-function.

**Hook point.** Detour `ScopeDisplay::Update` as a wrapper: call the original first,
so the engine has already resolved the texture, set `mScopeTexture` and enabled the
group, then apply the override. Read the visible flag at `+0x4C9`; when it is 0 there
is nothing to do.

**Re-apply only on change.** Cache the last applied mode, the last applied model hash
and the last applied `s_screenFull` W/H in the DLL, and re-apply only when one of them
differs. Re-reading W/H each time also fixes a latent stock bug: the four bitmaps take
their size from `s_screenFull` **once, in the ctor**, so vanilla scopes are wrong after
a resolution change.

## 1. `ScopeTextureFull`

```
ScopeTextureFull = "1"
```

Draws `ScopeTexture` once across the whole screen with no mirroring. Everything else —
which texture, the `weapon_scope` fallback, the blur, visibility — is unchanged.

**Apply:**

```
elem[0].SetSize(W, H, 1, 1)            // slot +0x48, align 1 = centred both axes
elem[0] UVs = (0,0) (0,1) (1,0) (1,1)  // direct writes at +0x88 + i*0x1C + 0xC/0x10
elem[1..3].Enable(false)               // slot +0x04
```

**Restore**, when a weapon without the flag becomes current, is the ctor's own values:
element 0 `SetSize(W/2, H/2, 2, 2)` UV `0,0,1,1`; element 1 `(W/2, H/2, 0, 2)` UV
`1,0,0,1`; element 2 `(W/2, H/2, 2, 0)` UV `0,1,1,0`; element 3 `(W/2, H/2, 0, 0)` UV
`1,1,0,0`; `Enable(true)` on all four.

**Authoring note for the user docs:** a full-screen scope stretches one 512x512 image
across the entire backbuffer, where the mirrored version only stretched it to 960x540
at 1080p. Author at 1024x1024 or higher, or it will look noticeably softer than a
stock scope.

## 2. `ScopeModel`

```
ScopeModel = "my_scope_model"
```

Renders a model as the overlay instead of the bitmap quads. This is the reason to
bother: msh materials carry render types the bitmap path has no access to.
`BF2_materials.doc` documents both of the interesting ones —

> **Scrolling** — Scrolls the diffuse texture according to the scroll speeds
> specified. The scroll is unidirectional.
>
> **Animated** — used for animated textures. All the frames must be on the same
> texture. Your UVs should be mapped to the first cell.

— plus `Glow`, and several materials can be layered in one model. So a scope can
combine a static reticle, a scrolling rangefinder ring and a glowing element without
the extension compositing anything, and without inventing `ScopeScrollSpeed` /
`ScopeFrameCount` / `ScopeFrameRate` properties.

That materials work at all through a screen element is established, not assumed:
`docs/RE/HUDBlendMode.md` found that `Red3DModelElementLite` discards `SetBlendMode`
because 3D elements "take their blend state from material data rather than from the
interface element".

**Element.** Allocate **one** `Red3DModelElement` (ctor mt `0x837FE0`, vtable
`0xA86F40`, `0x150` bytes) lazily on first use, not in a `ScopeDisplay` ctor hook —
the ctor runs before the model tables are necessarily populated, and lazy allocation
keeps the vanilla path untouched for mods that never use the property. Parent it to
`mGroupWeaponScope` with the group's `AddElement` (vtable `+0x44`, the same call the
ScopeDisplay ctor uses for the four bitmaps) and `Enable(false)` it.

**Binding.** The `Red3DModelElementLite::SetModel` by-hash tail (mt `0x839EA0`) sets
`+0x74 m_uiModelHash` and `+0x78 m_Model`, resolving against the global `RedModel`
hash table (mt `0xD4D964`, 2048 entries). `+0x78` is **null when the name did not
resolve** — that is the validity test.

**Apply:**

```
SetModel(elem, ext.model_hash);
if (elem->+0x78 != null) {
    bitmaps[0..3].Enable(false)
    elem->+0x94 = 0.0f            // m_OmegaY, or it spins like the loadscreen icons
    elem->+0x90 = scale           // m_scale
    elem.Enable(true)
} else {
    // unresolved model: fall back to the vanilla bitmap path and log ONCE,
    // matching how ScopeTexture silently falls back to weapon_scope
}
```

**Scale and position.** Element space is **device pixels**
([[loadscreen_team_model_icons]]). Authoring contract for v1: model authored in a 2x2
unit square in XY centred on the origin, and the extension applies `m_scale = H / 2`
so it fills the screen height. Position, if it ever needs one, goes through the setter
at mt `0x8285A0`, which writes a homogeneous vec4 at **`+0x60`** — *not* Phantom's
`+0xAC`, which reads back zeros on modtools.

**Precedence.** If a weapon sets both, `ScopeModel` wins and `ScopeTextureFull` is
ignored. Log once.

## Known hazards

- **No blend mode from the element side.** `Red3DModelElementLite` inherits the
  `RET 4` `SetBlendMode` stub, so additive/alpha behaviour must come out of the msh
  material. A modder who gets it wrong gets no warning.
- **Aspect ratio changes.** The bitmap path stretches per axis and gives BF2's
  familiar oval scope; a model renders through a real frustum (1 world unit == 1
  device pixel) and stays circular. Arguably a fix, but a visible divergence from
  vanilla, so it must ride on the opt-in only.
- **Element +y is DOWN** in element space — measured in game; do not re-derive it from
  the frustum, that has been got wrong once already. A model may render flipped until
  this is confirmed for `Red3DModelElement` specifically. **Verify on first render
  rather than assuming.**
- **Element-internal offsets diverge between debug and release builds** even when the
  total size matches. Do not port `Red3DModelElement` field offsets from Phantom
  ([[reference_phantom_build]]).
- **Shader Patch.** The zoom-blur hook keys off `fixedfunc_zoom_blur` following
  `filtercopy`, which is the blur pass and not the overlay, so it should be
  unaffected — but that wants one test with SP on rather than an assumption.
- The model must live in a loaded `.lvl`; see [[loaddatafile_cannot_load_sounds]] for
  which chunk types `LoadDataFile` can and cannot bring in standalone.

## Per-build work

Resolve on Steam and GOG before this can ship on anything but modtools:
`WeaponClass::SetProperty` impl, `ScopeDisplay::Update`, the `Red3DModelElement` ctor,
`s_screenFull` W/H, the `RedModel` hash table, and the GOG `SetModel` by-hash tail.

`SetUV`, `SetSize`, `SetTexture`, `Enable` and `AddElement` do **not** need per-build
addresses — the first is replaced by direct vertex writes, and the rest are vtable
slots resolved off the live element.

## See also

- [barrel-fire-origin.md](barrel-fire-origin.md) — the other consumer of the
  `ScopeDisplay` visible flag; carries the `EntityClass+0x1FC` mislabel corrected above
- [HUDBlendMode.md](HUDBlendMode.md) — why a Model3D never becomes additive
- [LoadDisplaySystem.md](LoadDisplaySystem.md) — `Red3DModelElement` field layout
- [WeaponClassFactory.md](WeaponClassFactory.md) — the `WeaponClass` vtable and the
  `Derive` / `Build` / `SetProperty` lifecycle
