# HUD BlendMode and why a Model3D never becomes Additive

Reported 2026-08-21: setting `BlendMode = Additive` on a HUD group has no effect when the group
contains a Model3D. It is an engine limitation, not a config error.


## FIELD RESULT 2026-08-26: material state DOES reach the HUD model draw

Reported by the project owner from play, and it reframes the section below.

**Vertex colours work on a Model3D HUD element.** They had appeared not to only because the
material carried an emissive/unlit flag, which suppressed them. Turning that flag off restored
them.

The important consequence is not the vertex colours themselves. It is that **a material flag
changed what the HUD element drew**, which means the HUD model path runs the ordinary material
pipeline rather than a stripped-down one. So the "3D models take their blend state from material
data rather than from the interface element" reading below is not merely the explanation for why
`BlendMode` was never wired up on the element -- it is also the route that still works:

- A model's blend state should be settable in the `.msh` material, and reach the HUD.
- `BlendMode` on the ELEMENT remains dead (the stubs below are still stubs). The element was
  never in that loop, which is exactly why the material still is.

Practical consequence for authoring: an additive glow on a HUD model belongs in the mesh, as a
duplicated segment with an additive material, tinted by vertex colours. That follows the model's
own transform for free, which no layered bitmap can do -- a bitmap is a screen-space quad and
cannot track a perspective-projected mesh however it is positioned.

Caution, from the same report: whatever suppressed vertex colours will likely interact with the
blend flag too. Set additive WITHOUT the emissive/unlit flag, and verify both together.

Not yet confirmed by static analysis; this is a play observation, and it outranks the inference
it corrects.


### Element `Color` REPLACES vertex colours (field-tested 2026-08-26)

Determined in play with a discriminating test, not inferred:

| vertex colour | element `Color` | result | verdict |
|---|---|---|---|
| `(255,0,0)` red | `(0,255,0)` green | **green** | replace |
| `(255,0,0)` red | omitted | **red** | vertex colours live when `Color` is absent |
| `(0,0,0)` black | `(0,228,255)` | not black | consistent with replace |

Multiply is refuted by the third row (black would stay black). Addition is refuted by the first
(would be yellow). **The two are mutually exclusive: specifying element `Color` switches vertex
colours off for that element entirely.**

Authoring consequences, and they are sharp:

- A mesh with several segments carrying DIFFERENT vertex colours is flattened to one colour the
  moment element `Color` is set. Per-segment colour variation and element colouring cannot
  coexist on one element.
- Anything needing a RUNTIME tint (an `EventColor` binding, a lerp) must therefore live on its
  own element, separate from anything that needs static per-vertex colour.
- Conversely, an element whose colour is authored per-vertex must leave `Color` unspecified --
  not set to white, OMITTED. Whether a white `Color` is distinguishable from an absent one was
  not tested.

Element `Alpha` is a separate property and was not part of this test; whether it composes with
vertex alpha or replaces it is UNKNOWN.


### `MeshInfo` is an optional override, not a registration (field-tested 2026-09-03)

A `Model3D` element does NOT need a `MeshInfo` block for a mesh to display. Every block is a
per-mesh transform OVERRIDE; absence means identity. Confirmed in play by deleting all 118
blocks from `data_BF3/Common/hud/hudtransforms.hud` -- the HUD still works and weapons still
swap.

Consistent with the code: `HUD::ElementModel3D::ReadData` (modtools `0x006A3BD0`,
`HUDElementModel3D.cpp`) only allocates the array `if (count != 0)`, so zero MeshInfos is a
supported state rather than a degenerate one. The cap is **256** per element
(`"Maximum number of MeshInfos exceeded"`, `HUDElementModel3D.cpp:253`).

**Authoring consequence.** A `MeshInfo` block whose Position is `(0,0,0)`, Rotation `(0,0,0)` and
Scale `(1,1,1)` is doing nothing and can be deleted. In the file above, all 118 were identity and
removing them took it from 1419 lines to 580. By contrast `1playerhud.hud` and
`common_hud_tmp.hud` have 18 and 19 blocks with EVERY one customised -- those are using the
feature properly and must be left alone. Check before bulk-deleting.

**`InheritMeshInfo("<element>")`** shares one element's list with another, for the case where two
elements show the same meshes and both need the same per-mesh transforms. `WriteData`
(modtools `0x006A40F0`) skips emitting MeshInfo entirely when the inherit flag at `+0x250` bit 0
is set, and `ReadData` clears that flag when an element parses its own -- so it is a genuine
share, not a copy. It is pointless once the lists are empty, and it is wrong if the two elements
draw DIFFERENT meshes, since the list is keyed by mesh name.

## The property parses fine

`HUD::Element::ReadData` (Phantom `0x005F3E10`) handles hash `0xFA784EAB`:

    pcVar8  = PblConfig::Data::GetStringArg(data, 0);
    BVar10  = TranslateString(sBlendModeStrings, 2, pcVar8);
    if (BLEND_ADDITIVE < BVar10) return true;          // unknown string, SILENTLY ignored
    (*element->vftablePtr->SetBlendMode)(element, BVar10);

Two things to note. The table has only **two** entries, and an unrecognised string takes the
early return with **no warning at all** - so a typo is indistinguishable in the log from the bug
below. And the property is applied through a VIRTUAL, so what happens next is per element class.

## The base implementations are stubs

    RedInterfaceElement::SetBlendMode  0x004FC5D0   RET 4              (discards)
    RedInterfaceElement::GetBlendMode  0x004FC290   XOR EAX,EAX / RET  (always BLEND_NORMAL)

Classes that DO override it store the value, e.g.
`RedBitmapElement::SetBlendMode` (`0x005F77A0`) is `MOV [ECX+0x74],EAX`.
`RedVectorElement` (`0x008923C0`) is the same shape.

## Model3D inherits both stubs

`HUD::ElementModel3D` is built on `Red3DModelElementLite`, whose vtable is at `0x00A44F8C`
(installed by the ctor at `0x008DEB70`: `MOV dword ptr [ESI],0xA44F8C`):

| slot | value | resolves to |
|---|---|---|
| `+0x0C` SetBlendMode | `0x0040D544` (ILT) | **`0x004FC5D0`** - the `RET 4` stub |
| `+0x10` GetBlendMode | `0x004109FB` (ILT) | **`0x004FC290`** - returns 0 |

So the model element has no storage for a blend mode, discards any set, and always reports
Normal.

## The group only forwards

`RedGroupElement::SetBlendMode` (`0x008C3B20`) stores nothing on itself; it walks its child list
and calls each child's virtual:

    008c3b36  MOV  ECX,[ESI+0xC]     ; child
    008c3b3a  MOV  EAX,[ECX]         ; child vftable
    008c3b3c  CALL [EAX+0xC]         ; child->SetBlendMode
    008c3b3f  MOV  ESI,[ESI+0x4]     ; next

So a group has no blend mode of its own - the property means "apply to every child". A Model3D
child absorbs it into a `RET`, and if the Model3D is the ONLY child, setting the group's
BlendMode does nothing whatsoever.

## Fixing it is not a byte patch

It needs three things: storage on the element, an override that writes it, and the model RENDER
path to honour it. The third is the real work - 3D models take their blend state from material
data rather than from the interface element, which is a different mechanism from the bitmap path
and presumably why this was never wired up.

Workaround: bitmap and vector elements store blend mode correctly, so an additive bitmap layered
with the model achieves the usual glow effect.
