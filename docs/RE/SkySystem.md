# Sky system: the missing `FlatInfo()` block

BF1 had a `.sky` block, `FlatInfo()`, that BF2 does not parse at all. It was a scrolling flat
texture layer: a horizontal plane, separate from the dome, for a cloud or haze sheet at a fixed
world height.

## The schema survives in a stock asset

`assets/worlds/END/world1/end1.sky` still carries a `FlatInfo()` block that a porter left in and
nothing has read since:

- `Height(0,0)`
- `Texture`
- `Color`
- `Modulate`
- `TextureSpeed` - the UV scroll that makes it drift
- `TileSize`

Endor's copy is inert, so the file proves the schema, not the visual.

## Both halves were compiled out

Sky blocks dispatch by PblHash. `PblHash("FlatInfo") = 0x4B936222` occurs nowhere in the modtools
exe, as neither constant nor string, while every supported block occurs exactly once:

| Block | PblHash |
|---|---|
| `SkyInfo` | `0x06C0D3E6` |
| `DomeInfo` | `0x185AB6C2` |
| `DomeModel` | `0x82681057` |
| `SunInfo` | `0x3780C8F9` |
| `LowResTerrain` | `0xDDF0C29E` |

The parser case and the renderer are both gone, so restoring it needs a new sky config case plus
a renderable that draws one textured, scrolling quad at world height.

## BF1 reference

BF1's reader is `SkyDome::Read(PblConfig&)` at `0x001D10A0`, with `SkyDome::Render` the draw side.
**Neither has been read yet.** Still open:

- what the two `Height` values mean
- how `Modulate` selects the blend
- whether the plane draws before or after the dome

Ghidra has not applied BF1's symbols; resolve names through their Mach-O nlist entries, not by
name lookup.
