# Atmosphere: fog, clouds, light and Shader Patch

What BF2's renderer can already do for fog, clouds and light, what Shader Patch does with it, and how
GameExt could add atmosphere without clashing with Shader Patch. Researched 2026-10-11 by a read-only
pass over Ghidra, the stock assets and Shader Patch's source; nothing is built, and each address is to
be re-read before it is used. P = Phantom, M = modtools, S = Steam.

## The approach

Atmosphere belongs at the engine level. BF2 already ships height fog, sun-projected scrolling cloud
shadows, rotating cloud domes, a hidden cloud-cluster layer, ground mist, placed god rays and heat
haze, and every one of them draws through stock render types that Shader Patch re-implements. They look
the same with or without Shader Patch and cost almost nothing. GameExt's part is to expose and script
them, restore `FlatInfo` through an existing render type, and guard their edge cases.

Screen-space effects (per-pixel fog, sun shafts, soft particles, an analytic sky) belong in Shader
Patch, not in a GameExt Direct3D 9 pass: Shader Patch cannot run another mod's shaders, and stock
Direct3D 9 gives no depth texture to build them on.

## Shader Patch

Sources: the README, releases page and Effects System wiki of `PrismaticFlower/shaderpatch` (formerly
SleepKiller's), and the installed 1.10.1's `shader patch.yml`, `deso.spfx` and DLL strings.

- **Provides:** a Direct3D 11 renderer with per-pixel lighting, order-independent transparency and HDR
  targets; CMAA2, MSAA and supersampled alpha test; SSAO, depth of field, AMD CAS sharpening; bloom,
  colour grading and tonemapping, vignette, film grain; colour-grading regions; custom and PBR
  materials, a cubemap skybox material, water and refraction. Latest release 1.11.0 (7 October 2026);
  1.10.1 was the version read.
- **Does not provide:** fog, clouds, light shafts or soft particles (particles get near and distance
  fades only).
- **Its fog is the game's fog.** Its materials compute fog per vertex as the smaller of depth fog and
  height fog from the engine's constants (`shaders/src/vertex_utilities.hlsl`), and it reads only
  `FOGENABLE` and `FOGCOLOR` from the render state (`render_state_manager.cpp`). So changing the
  engine's fog carries straight into Shader Patch.
- **Another mod's shaders cannot work.** `file_hooks.cpp` strips the stock shader chunks from
  `core.lvl` and injects its own; `CreateVertexShader` keeps only the first DWORD, as an index into its
  own shader table, with no bounds check seen; `CreatePixelShader` and `SetPixelShader` ignore what they
  are given. Real Direct3D 9 bytecode would index garbage, most likely a crash (inferred).
- **Fixed-function draws** work only in five texture-stage patterns; anything else calls
  `std::terminate`, which GameExt has already met as an uncatchable crash (see
  [LoadDisplaySystem.md](LoadDisplaySystem.md)).
- **Draws through the game's own shader objects work**, which is how the HUD gradient plan reuses
  `RedMaskedBitmapElement`. Shader Patch hangs its SSAO and transparency work on the stock `skyfog`
  pass, so stock passes must not be reordered or removed.
- **For other mods** there is no API: only the Direct3D 9 entry points and the detection export GameExt
  already uses (`util/shader_patch_detect.hpp`), the Lua `shader_patch_api.script` it builds on the fly
  (`gSP_Installed`, `spVersion`, `SP_TestVersion`), and data in `.lvl` files (materials, textures,
  effects configs, colour-grading regions; an `.mspfx` effects config can be loaded with
  `ReadDataFile`). New post passes need code in Shader Patch itself.
- **RJP1992's fork** (`RJP1992/shaderpatch`, branch `clouds`) adds post-process fog in the SWBF3
  style (height, atmosphere, noise, sun inscatter), three procedural cloud layers and cloud volumes at
  the `skyfog` pass. Its notes name the blocker: by that pass the far scene sits at a depth of about
  1.0, so per-pixel fog cannot tell distant terrain from sky.

## Fog

`RedRenderer::internalUpdateSceneConstants` (P `0x00880AC0`, M `0x0080ACB0`) writes vertex constant c7:
depth fog from `FogRange` and height fog from `WorldFogRange`. Height fog is on only when its top is
above its bottom, clear above the first value and solid at the second. Fog is linear, per vertex, in
one colour, and `FogColor`'s alpha is forced to 255. Stock maps use height fog: Dagobah
`WorldFogRange(-1.7, -5.0)`, Mustafar `(60, -155)`.

`FLRenderer::Render` (P `0x0088E290`, M `0x0081EAF0`) pushes `FLRenderer`'s values again every frame, so
a change must go through `FLRenderer::SetWorldFogRange` (P `0x00890DC0`, M `0x0081B000`; its globals on
M at `0x00E5BA20` and `0x00E5BB58`), not into the constants. The sky file's parser is
`SkyManager::ReadSkyInfo_` (P `0x007480D0`, M `0x006C0B50`, S `0x006387F0`). The console has
`fog.range`, `fog.worldrange` and `fog.color` (P `0x00746EA0`, M `0x006BE500`). Stock Lua has no fog
functions; `SetFogEnable` and `SetFogRange` are GameExt's.

## Sky

`SkyDome::Read` (P `0x00745550`, M `0x006BE930`, S `0x006378A0`). A `DomeModel` takes
`rotationspeed(speed, axis)` (Felucia turns its cloud dome this way), parallax, and particle effects on
hardpoints. There is a hard limit of 10 dome models: an 11th only logs "Too many domes" and keeps
writing past the end of the array (an overflow seen in P only; inferred).

**A hidden cloud layer.** `CloudLayer { Odf(...); Height(...); }` builds 64 objects in an 8 x 8 grid of
250 m cells around the world's origin, at the given height plus up to 35 m. Its class label
`cloudcluster` (up to five `dusteffect` classes with a spread, drawn back to front) exists in M and S.
No stock data uses either.

`FlatInfo`, BF1's flat scrolling cloud layer, was cut from both the parser and the renderer; see
[SkySystem.md](SkySystem.md).

## World effects

`FLEffect::InitAll` (P `0x005C0160`) creates `LightBeam`, `LensFlare`, `SunFlare`, `FogCloud`,
`Precipitation`, `Wind`, `Lightning`, `Water`, `Godray`, `Blur`, `ColorControl`, `LightningBolt`,
`WorldShadowMap`, `MotionBlur`, `ScopeBlur`, `HeatShimmer`, `Decal`, `Shield`, `ScreenFilter`,
`SpaceDust`, `LightsaberTrail`, `HDR` and `Shadow`. Glow is `Effect("hdr")`; there is no glow class.

- **WorldShadowMap: moving cloud shadows, already in the engine.** It gives a named directional light
  a projected texture (`ReadConfig`: P `0x007D42D0`, M `0x00779F10`, S `0x006951D0`; `Update`: P
  `0x007D4680`, M `0x0077A200`), which `RedDirectionalLight::_RecalculateProjectionMatrix`
  (P `0x008E5080`) tiles across the world. With `AnimationFrequency(0)` the texture scrolls in a straight
  line at `AnimationAmplitude0` world units a second. Endor turns it on for PC. `RedLight::GetLightingState`
  (P `0x0089DB20`) allows one projected light per object (a later omni or spot light wins), and turns it
  off in split screen. Hoth's world shadow map is PS2-only data.
- **FogCloud** (P `0x007D1B20`, ground mist, used by 10 stock worlds) and **Godray** (placed `godray`
  ODFs) draw with the stock particle shader.
- **HeatShimmer** is implemented on PC (P `0x005EA840`, `0x00458E60`) but no stock world uses it.
- **ColorControl**'s PC reader is empty (P `0x0044DE30`), so a map's colour settings do nothing on PC.
- Lightning is commented out in the stock `.fx` files. `WaterInfo` and `FogRamp` are absent from P, and
  the low-resolution terrain's `FogNear` and `FogFar` are absent from P and M.

## Candidates, ranked

| | Effect | Value | Cost | Level | With Shader Patch | Effort |
|---|---|---|---|---|---|---|
| 1 | Height fog and scriptable fog (Lua `SetWorldFogRange`, `SetFogColor`, fog regions, timed changes) | high | none | engine | works | small |
| 2 | Moving cloud shadows (`WorldShadowMap`) | high | about a texture fetch per lit pixel | engine | works (projected textures on directional lights since 1.10.0) | data and docs |
| 3 | Cloud layers: rotating domes, `CloudLayer` and `cloudcluster`, `FlatInfo` restored | medium-high | low | engine | works | data, then medium |
| 4 | Placed god rays | medium | low | engine | works | data |
| 5 | Ground mist (`FogCloud`) and heat haze (`HeatShimmer`) | medium | overdraw | engine | heat haze untested | data |
| 6 | Sun glow in fog: tint the fog toward the sun's colour by where the camera looks, once a frame | medium-low | none | engine | works | small; quality inferred |
| 7 | Per-pixel exponential height fog | high | one full-screen pass | renderer | in the user's fork, blocked by depth | Shader Patch only |
| 8 | Screen-space sun shafts | high | low-medium | renderer | Shader Patch work | Shader Patch only |
| 9 | Soft particles | medium | low | renderer | needs depth bound for particles | Shader Patch only |
| 10 | Analytic sky | medium | low | renderer | perhaps a Shader Patch material as data (inferred) | Shader Patch only |
| 11 | Sharpening, AA, SSAO, bloom, grading, depth of field, grain | | | | Shader Patch ships them; ReShade without it | none |
| 12 | Raymarched volumetric clouds | | not minimal | | | skip |

## Open

- Play `WorldShadowMap` scrolling in a straight line on M, S and G, with Shader Patch on and off: on
  terrain, props and soldiers, and with a projected light nearby overriding it.
- Does per-vertex height fog band on large triangles?
- Do `CloudLayer` and `cloudcluster` draw on PC and under Shader Patch, what do 64 clusters cost, which
  ODF keys does `cloudcluster` take, and does the grid only cover the area round the world's origin?
- Does an 11th dome model crash M and retail? Guard it either way.
- Which pass does `HeatShimmer` use under Shader Patch, and does Shader Patch bind depth for particles?
- The retail addresses of `FLRenderer`'s world fog globals.
- Where must fog changes from Lua run online?
- Do the fork's fog and clouds go upstream?
