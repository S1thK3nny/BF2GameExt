# RedWater animated-texture arrays

How a world's animated water frame lists are parsed, where the engine stores
them, and the two ways a `.fx` file can turn that into a load-time crash on
retail while modtools shrugs it off.

## The properties

Animated water lives in the world's `.fx` file (not the `.wld`), in the
`Effect("Water")` block:

```
Effect("Water")
{
    ...
    NormalMapTextures("water_normalmap_", 16, 8.0);

    PC()
    {
        BumpMapTextures("water_bumpmap_",           16, 8.0);
        SpecularMaskTextures("water_specularmask_", 25, 4.0);
    }
}
```

Signature is `(prefix, frameCount, scrollSpeed)`. Frame names are built with
`sprintf(buf, "%s%d", prefix, i)` for `i` in `0 .. count-1` (format string:
Steam `0x007973A0`, GOG `0x00798340`), so the textures are `water_normalmap_0`
through `water_normalmap_15`.

Every stock world and every mod world in the local corpus uses 16 / 16 / 25.

PblHashes (`Hash.exe` ground truth):

| Property | PblHash |
| --- | --- |
| `NormalMapTextures` | `0xEAC587AC` |
| `BumpMapTextures` | `0x11D65039` |
| `SpecularMaskTextures` | `0xC0311180` |

## The parsed property block

`FUN_00727E30` (Steam) reads one `DATA` child and lays it out as:

```
+0x00  PblHash
+0x04  argument count          (dword, sign-extended from a byte in the chunk)
+0x08  arg0
+0x0C  arg1
+0x10  arg2                    (argCount dwords in total)
+8 + 4*argCount                the string data, memcpy'd directly after the args
```

String arguments are stored as a self-relative dword offset: the handler
recovers the pointer with `ESI + 8 + [ESI+8]`, and that offset is always
`4 * argCount`, i.e. "just past the last argument".

**That is the important part.** The string sits immediately behind the
arguments, so dropping arguments slides the string forward into the argument
slots.

Ground truth from `ConfigMunge.exe` on two one-line `.fx` files:

```
NormalMapTextures("water_normalmap_", 16, 8.0);
  hash 0xEAC587AC  argCount 3
  arg0 0x0000000C (string offset 12 -> block+0x14)
  arg1 0x41800000 = 16.0f      <- frame count
  arg2 0x41000000 = 8.0f       <- speed
  strlen 17 "water_normalmap_\0"

NormalMapTextures("water_normalmap_");
  hash 0xEAC587AC  argCount 1
  arg0 0x00000004 (string offset 4 -> block+0x0C)
  strlen 17 "water_normalmap_\0"
  ==> block+0x0C is the string: 'w','a','t','e' = 0x65746177 = 7.21e22f
```

## The handler

`RedWater::ReadConfig` (Steam `0x006D00D0`) handles a dozen water properties
inline and falls through to a second dispatcher for the rest (Steam
`FUN_0071FA60`, called at `0x006D0315`). All three texture-list properties land
there with the same shape:

```asm
CVTTSS2SI EAX,[ESI+0xc]        ; frameCount, straight off the property
XOR  EDI,EDI
MOV  [<count>],EAX             ; <- BF2GameExt patch site (A3 imm32)
TEST EAX,EAX
JZ   done
loop:
...sprintf("%s%d", prefix, EDI); texture lookup...
MOV  [EDI*4 + <array>],EAX     ; no bounds check
INC  EDI
CMP  EDI,[<count>]             ; bound re-read from memory every pass
JC   loop                      ; unsigned
done:
MOVSS XMM0,[ESI+0x10]          ; scrollSpeed
MOVSS [<speed>],XMM0
```

Nothing validates the argument count on retail. Modtools still carries the
original check and warns when `[ESI+4] <= 1`, but it warns only and runs the
same code afterwards.

## Where the tables live

Each table holds **50** entries (`0xC8` bytes). On both retail builds the three
blocks are packed with no slack at all, which pins the size exactly: array at
`+0`, count at `+0xC8`, speed at `+0xCC`, next array at `+0xD0`. The reset
routine (`FUN_0071F9B0` on Steam) zeroes exactly these globals and nothing
between them. Modtools spaces its arrays `0xD8` apart, consistent with 50
entries plus padding but not independently pinning it.

Steam:

| Array | Count | Speed | Property |
| --- | --- | --- | --- |
| `0x009CAB98` | `0x009CAC60` | `0x009CAC64` | `SpecularMaskTextures` |
| `0x009CAC68` | `0x009CAD30` | `0x009CAD34` | `BumpMapTextures` |
| `0x009CAD38` | `0x009CAE00` | `0x009CAE04` | `NormalMapTextures` |

GOG:

| Array | Count | Speed | Property |
| --- | --- | --- | --- |
| `0x009CC038` | `0x009CC100` | `0x009CC104` | `SpecularMaskTextures` |
| `0x009CC108` | `0x009CC1D0` | `0x009CC1D4` | `BumpMapTextures` |
| `0x009CC1D8` | `0x009CC2A0` | `0x009CC2A4` | `NormalMapTextures` |

Modtools (`BattlefrontII.Debug.FullScreen.1080.exe`):

| Array | Count | Speed | Property |
| --- | --- | --- | --- |
| `0x00EDE228` | `0x00EDE208` | `0x00EDE224` | `SpecularMaskTextures` |
| `0x00EDE300` | `0x00EDE210` | `0x00EDE3D0` | `NormalMapTextures` |
| `0x00EDE3D8` | `0x00EDE218` | `0x00EDE21C` | `BumpMapTextures` |

## Failure 1: the count argument is missing

This is the one seen in the wild, and it is what several `.fx` round-trip /
re-export tools produce:

```
NormalMapTextures("water_normalmap_");
BumpMapTextures("water_bumpmap_");
SpecularMaskTextures("water_specularmask_");
```

`argCount == 1`, so the string lands at `block+0x0C` and the handler reads
`'w','a','t','e'` = `0x65746177` = **7.21e22f** as the frame count. Both builds
read the same garbage float. They differ in how they convert it:

| Build | Conversion | Overflow result | Effect |
| --- | --- | --- | --- |
| Steam / GOG | SSE `CVTTSS2SI EAX,[ESI+0xc]` | `0x80000000` (32-bit integer indefinite) | nonzero, so `TEST/JZ` falls into the loop; the unsigned `JC` runs it 2^31 times |
| Modtools | x87 `FLD` + `_ftol` (`FISTP QWORD`, low dword kept) | `0x8000000000000000` -> low dword **0** | `TEST/JBE` skips the loop entirely; no animated normal maps, level loads |

Verified in the debug build's `_ftol` at `0x008D29B0`: it does `FISTP QWORD
[esp+0x10]` / `MOV EAX,[esp+0x10]`, and the overflow path at `0x008D2A0F` tests
only `EDX & 0x7FFFFFFF`, falls through, and returns with `EAX == 0`.

**That conversion difference, not the table layout, is why a broken `.fx` runs
under the modtools exe and takes the retail exe down during load.**

## Failure 2: a count above 50

Independent of the above, and equally unchecked. On retail the count global sits
at `array + 0xC8`, i.e. exactly where entry index 50 is written, so:

1. Iteration 50 stores a texture handle into `[<array> + 50*4]`, which *is*
   `<count>`.
2. The loop re-reads `<count>` every pass, so the bound becomes that handle.
3. The loop keeps storing dwords through the rest of `.data` and past the end of
   the image until it reaches an unmapped page.

Modtools puts the counts ahead of the arrays, so the same over-count corrupts
neighbouring water globals instead of the loop bound and the level still loads.

Either failure ends up at the same instruction. Observed crash (Steam, module
base `0x2F0000`):

```
EXCEPTION C0000005 at EIP=0060FD11 (BattlefrontII.exe+0x31FD11, va 0071FD11)
AV: WRITE addr 01E9D000
EAX=08853C72   EDI=005788B2
frames: RedWater::ReadConfig+0x24F <- LoadUtil::ReadDataFile <- Lua_Callbacks::ReadDataFile
```

`0x009CAD38` relocates to `0x008BAD38`; `0x008BAD38 + 0x005788B2*4 =
0x01E9D000`, the reported fault address. `EAX` is the texture handle that had
already replaced the loop bound.

## Consumers, and why the floor is 1 rather than 0

`RedWater::pcAccumulateNormalMap` (Steam `0x00720570`) and
`RedWater::pcSetupShaders` (Steam `0x00721F60`) pick the current frame with
`DIV dword ptr [<count>]` and index the array by the remainder.

`pcAccumulateNormalMap` has exactly one caller (Steam `0x00720142`), gated on
the water patch count `[0x009650E4]` and two renderer capability flags. **It is
not gated on the frame count**, so a count of 0 is an integer divide-by-zero at
`0x00720750`. `EDX` is zeroed before both divides, so no quotient overflow is
possible; zero is the only bad divisor.

That is why the sanitizer's floor is 1 and not 0: writing 0 would trade the
access violation for a divide error.

## The BF2GameExt fix

`PatcherDLL/src/render/water_texture_count_fix.cpp`.

The `MOV [<count>],EAX` store is `A3 imm32`, exactly 5 bytes on all three
builds. It is replaced with `E8 rel32` to a per-property naked stub that hands
`(propertyIndex, rawCount, ESI)` to a C sanitizer and writes the result:

| Condition | Result |
| --- | --- |
| `[ESI+4] < 2` (no frame-count argument; the slot is string data) | 1 |
| count `== 0` (`DIV` by it downstream) | 1 |
| count `> 50` (table capacity) | 50 |
| otherwise | unchanged |

The argument count is read signed, because `FUN_00727E30` sign-extends it from a
byte.

The count global's address is read out of the displaced instruction's own imm32
operand at install time, so it needs no entry in the address registry.

The stubs are register- and flag-transparent (`pushfd` / `pushad`, result written
over the saved `EAX` slot at `[esp+0x1C]`). `EFLAGS` matters: retail sets the
flags it needs *after* the store (`TEST EAX,EAX / JZ`); modtools sets them
*before* it (`TEST EAX,EAX / MOV / JBE`) and relies on `MOV` not disturbing them.
Sanitizing never turns a nonzero count into zero, so handing the original flags
back is correct on both.

Patch sites (unrelocated):

| Build | `NormalMapTextures` | `BumpMapTextures` | `SpecularMaskTextures` |
| --- | --- | --- | --- |
| Modtools | `0x00864CCB` | `0x00864A0F` | `0x00864B9C` |
| Steam | `0x0071FCD3` | `0x0071FAB6` | `0x0071FBF4` |
| GOG | `0x00720DA3` | `0x00720B86` | `0x00720CC4` |

Byte guard, verified before patching so a mismatched build silently no-ops:

| Build | Preceding bytes | Site | Following bytes |
| --- | --- | --- | --- |
| Steam / GOG | `F3 0F 2C 46 0C 33 FF` | `A3` | `85 C0` |
| Modtools | `33 FF 85 C0` | `A3` | `76 3E` |

One log line per property per session names the property and what was wrong, so
the modder can find it in the `.fx`.

Modtools is patched too. Neither failure crashes there, but both silently
degrade the water, and the log line is where a modder will actually see the
problem before shipping.

### Left alone

With exactly two arguments the count is real but the *speed* slot holds string
data. The engine multiplies it by elapsed time, overflows the same conversion to
`0x80000000`, and `DIV`s that by the count, which lands on frame 0 every pass:
the animation freezes but nothing faults. Not worth a second patch site; the log
line already points at the property.
