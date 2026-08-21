# HUD BlendMode and why a Model3D never becomes Additive

Reported 2026-08-21: setting `BlendMode = Additive` on a HUD group has no effect when the group
contains a Model3D. It is an engine limitation, not a config error.

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
