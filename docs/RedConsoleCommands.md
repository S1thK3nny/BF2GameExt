# RedCommandConsole - Built-in Console Commands & Variables

Reference for every **stock** engine console command/variable registered by BF2's
`RedCommandConsole`. This documents the game's own developer console (RedEngine),
**not** the custom Lua functions added by this project.

**TODO:** Go over most commands, check if they are actually functional in the retail build, and add better descriptions.

## How the console works

Two registration entry points, both taking a name string plus a target:

| Function | Registers | Meaning |
|----------|-----------|---------|
| `RedCommandConsole::AddCommand(name, callback)` | A **command** - runs a `int(__cdecl*)(RedConsole*, uint id, char* args)` callback | Type tag `9` |
| `RedCommandConsole::AddVariable(name, &multivar)` | A **variable** - reads/writes a backing global | Type tags below |

All entries are registered from C++ static initializers at startup, so the whole
table is present from launch. Names are case-insensitive in practice and are
matched by `FindCommand`. A command may be typed in the `~` console, and the same
strings are reachable from Lua via `ScriptCB_DoConsoleCmd("<name> <args>")`
(list them with `ScriptCB_GetConsoleCmds`).

### Variable type tags (`MultiVar` first dword)

| Tag | Type | Usage |
|-----|------|-------|
| `0x11` | **bool** | `name 1` / `name 0` (toggle flag) |
| `0x44` | **int** (signed 32-bit) | `name <int>` |
| `0x46` | **float** | `name <float>` |
| `0x00` | **stub** (null backing ptr) | Registered name with no backing var - value handled by a sibling command callback |

### `debugmenu.*` aliases

Many gameplay/render flags are registered **twice**: once under their normal name
and once under a `debugmenu.*` name pointing at the *same* backing variable, so the
in-game Debug Menu UI can drive them. Where an alias exists it is listed in the
"debugmenu alias" column. They are functionally identical to the primary entry.

---

## Cheats & gameplay

| Name | Type | Params | Description | debugmenu alias |
|------|------|--------|-------------|-----------------|
| `invincible` | bool | 0/1 | God mode for the local player (`GameLoop::gInvincible`). | `debugmenu.invincible` |
| `InvincibleAll` | bool | 0/1 | Invincibility for all units (`gInvincibleAll`). | `debugmenu.InvincibleAll` |
| `unlimitedammo` | bool | 0/1 | Never consume ammo (`GameLoop::gUnlimitedAmmo`). | `debugmenu.unlimitedammo` |
| `UnlimitedEnergyAll` | bool | 0/1 | Infinite energy for all (`gUnlimitedEnergyAll`). | `debugmenu.UnlimitedEnergyAll` |
| `enableheroes` | bool | 0/1 | Force-enable hero units (`gEnableHeroes`). | `debugmenu.enableheroes` |
| `EnableAllAwards` | bool | 0/1 | Grant/enable all award weapons (`gEnableAllAwards`). | `debugmenu.EnableAllAwards` |
| `NighInfiniteReinforcements` | cmd | - | Set both teams' reinforcement counts near-infinite (INT_MAX = 2.147.483.647) (`JSetMaxReinforcements`). | `debugmenu.InfiniteReinforce` |
| `SelfDestruct` | cmd | - | Kill/destroy the player's current unit (`SelfDestruct`). | `Debugmenu.SelfDestruct` |
| `ForceVictory` | cmd | - | Immediately win the match (`ForceVictory`). | `debugmenu.ForceVictory` |
| `ForceDefeat` | cmd | - | Immediately lose the match (`ForceDefeat`). | `debugmenu.ForceDefeat` |
| `damageobject` | cmd | `<amount>` | Apply a damage value to the targeted object (`SetDamageCallback`). | - |
| `skipplayer` | bool | 0/1 | **No-op.** `gSkipPlayer` is only ever initialized but not read, so toggling it does nothing. Leftover console knob from a cut "spawn/observe with AI only, no local player" feature (`gSkipPlayer`). | `debugmenu.skipplayer` |
| `debugmenu.ToggleUnlockAllClasses` | cmd | - | Unlock all unit classes (`ToggleUnlockAllClasses`). | (menu-only) |

## AI debug

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `aidiff` | cmd | - | Print AI difficulty info (`ShowAIDiffFunc`). |
| `aigoals` | cmd | - | Show AI goals overlay (`ShowAIGoalsFunc`). |
| `ai.cps` | cmd | - | Prints a table of each command post's spawn and strategic weighting values for every side. These are the numbers the AI uses to decide which posts to attack, defend, and spawn at (`ShowCPWeightsFunc`). |
| `aimode` | bool | 0/1 | AI debug text output (`gAIDebugOutput`). |
| `ai.cam` | int | mode | Master switch for the **AI spectator camera**: a developer mode that detaches the third-person chase camera from your unit and makes it follow AI-controlled bots so you can watch them play. `0` = off (normal, follows player, requires a respawn.); non-zero = spectate an AI unit. Consumed in `ChaseCamera::Update` (`gAICamMode`). |
| `ai.camup` | bool | 0/1 | Momentary "spectate the **next** AI unit" trigger for the AI spectator camera. Steps the selection forward through the team rosters (wrapping team 1 → 2 → 1) to the next bot the AI-debug system is tracking, then self-resets to `0`. Auto-enables `ai.cam` if it was off. Meant to be used with `aimode` (on-screen AI debug text) (`gAICamUp`). |
| `ai.camdown` | bool | 0/1 | Same as `ai.camup` but steps to the **previous** AI unit (backward through the rosters), then self-resets to `0` (`gAICamDown`). |
| `ai.aimaxflyheight` | float | height | Max AI flyer height (`gMaxFlyHeight`). |
| `ai.aiminflyheight` | float | height | Min AI flyer height (`gMinFlyHeight`). |
| `ai.playermaxflyheight` | float | height | Max player flyer height clamp (`gMaxPlayerFlyHeight`). |
| `ai.playerminflyheight` | float | height | Min player flyer height clamp (`gMinPlayerFlyHeight`). |
| `ai.renderdist` | float | dist | AI debug render distance (`gAIDebugRenderDist`). |
| `ai.QueueVisionRayTests` | bool | 0/1 | Queue vision ray tests instead of immediate (`mQueueVisionRayTests`). |
| `ai.showadrenaline` | bool | 0/1 | Draw AI adrenaline state (`gAIDebugShowAdrenaline`). |
| `ai.showallpaths` | bool | 0/1 | Draw all AI paths (`gAIDebugShowAllPaths`). |
| `ai.showconnectivitygraph` | bool | 0/1 | Draw pathing connectivity graph. |
| `ai.showHintNodes` | bool | 0/1 | Draw AI hint nodes. |
| `ai.showobstacles` | bool | 0/1 | Draw AI obstacles. |
| `ai.showobstaclesradius` | float | radius | Radius for obstacle drawing. |
| `ai.showtypemask` | int (bitmask) | `-1`=all (default), `0`=none, or a bit combination | Filters **which AI debug output is drawn** (not a render mask). Each AI unit carries a category tag, and some debug draws use fixed category bits (e.g. flyer strafe lines = bit `0x20`); an item is shown only if its bit is set in this mask. Only takes effect with `aimode` on, and also gates which units `ai.camup`/`ai.camdown` will stop on. Default `-1` shows everything; set a narrower value to focus on specific categories (`gAIDebugShowTypeMask`). |
| `ai.showunitpathingradius` | int | value | Draw unit pathing radius. |
| `showaimers` | bool | 0/1 | **No-op.** `gShowAimer` is only registered by the console and never read or written by any code path (confirmed in both modtools and release builds), so it draws nothing — not even in freecam. The aimer-node debug draw was cut, leaving only the console knob (`gShowAimer`). |
| `showtargets` | bool | 0/1 | Draw AI targets (`gShowTarget`). |
| `showflyerheights` | bool | 0/1 | Draw flyer min/max height planes (`gShowFlyerHeights`). |

## Camera / view

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `fov` | cmd | `<deg>` | Set field of view (`SetFovCallback`). |
| `SetFOV` | cmd | `<deg>` | Alternate set-FOV command (`JSetFOV`). |
| `aspectratio` | cmd | `<ratio>` | Set aspect ratio (`SetAspectRatioCallback`). |
| `nearplane` | cmd | `<dist>` | Set near clip plane (`SetNearPlaneCallback`). |
| `setviewrange` | cmd | `<dist>` | Set far view range (`SetViewRangeCallback`). |
| `Renderer.SetAspect` | cmd | `<ratio>` | Set renderer aspect ratio (`JSetAspectRatio`). |
| `Renderer.ScreenshotSetup` | cmd | - | Reconfigure camera aspect for screenshots (`JFuxorAspectRatio`). |
| `SnapCamera` | cmd | - | Casts a ray forward from the camera up to 500 units and snaps the free camera onto the surface it hits. This only works while you are in free-camera mode, and it prints a message otherwise (`SnapCamera`). |
| `GetCameraPos` | cmd | - | Print current camera position (`GetCameraPos`). |
| `DumpCamera` | cmd | - | Dump camera coordinates (`JDumpCameraCoords`). |
| `SetControls` | cmd | - | (Re)assign player controls (`SetPlayerControls`). |
| `Mouse.Invert` | bool | 0/1 | Invert mouse look (`gInvertMouse`). |

### Free camera

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `FreeCameraStop` | cmd | - | Stop/exit free camera (`FreeCameraStop`). |
| `FreeCameraInvertAxis` | cmd | - | Invert a free-camera axis (`FreeCameraInvertAxis`). |
| `FreeCameraQuakeMode` | cmd | - | Toggles the Quake-style free-camera control scheme, which gives you WASD and mouse noclip flight (`FreeCamera::sQuakeControl`). |
| `debugmenu.SetFreecamTarget` | bool | 0/1 | Set free-cam follow target object (`gSetTargetObj`). |
| `debugmenu.SetFreecamTetherPos` | bool | 0/1 | Set free-cam tether position (`gSetTetherPosition`). |
| `debugmenu.ToggleFreecamFollow` | bool | 0/1 | Toggle free-cam follow mode (`gIsFollowingObj`). |
| `debugmenu.ToggleFreecamTether` | bool | 0/1 | Toggle free-cam tether mode (`gFollowingTethered`). |
| `freecamlight.enable` | cmd | `<0/1>` | Enable the free-camera attached light (`SetFreeCamLightCallback`). |
| `freecamlight.color` | cmd | `<r g b>` | Free-cam light color. |
| `freecamlight.radius` | cmd | `<r>` | Free-cam light radius. |
| `freecamlight.freeze` | cmd | `<0/1>` | Freeze free-cam light in place. |

### Camera tension / tracking (3rd-person tuning)

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `aimtension` | cmd | `<x>` | Aim-tension X (`SetAimTensionCallback`). |
| `movetension.x` | cmd | `<v>` | Move-tension X. |
| `movetension.y` | cmd | `<v>` | Move-tension Y. |
| `movetension.z` | cmd | `<v>` | Move-tension Z. |
| `trackcenter` | cmd | `<v>` | Camera track center (`SetTrackCenterCallback`). |
| `trackoffset` | cmd | `<v>` | Camera track offset (`SetTrackOffsetCallback`). |
| `TrackOffsetMin` | cmd | `<v>` | Min track offset (`SetTrackOffsetMinCallback`). |
| `TrackOffsetMax` | cmd | `<v>` | Max track offset (`SetTrackOffsetMaxCallback`). |
| `tiltvalue` | cmd | `<v>` | Camera tilt value (`SetTiltValueCallback`). |
| `eyepointoffset` | cmd | `<v>` | Eyepoint offset (`SetEyePointOffsetCallback`). |

### First-person view

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `FirstPerson.OffsetX` | float | v | FP model X offset (`sFirstPersonOffsetX`). |
| `FirstPerson.OffsetY` | float | v | FP model Y offset. |
| `FirstPerson.OffsetZ` | float | v | FP model Z offset. |
| `FirstPerson.Scale` | float | v | FP model scale (`sFirstPersonScale`). |

## Time / frame / profiling

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `framerate` | bool | 0/1 | Show framerate counter (`PerformanceTimer::_bFrameRateEnabled`). alias `debugmenu.framerate` |
| `fixframerate` | cmd | `<fps>` | Lock to a fixed frame rate (`FixFrameRateCallback`). |
| `slowmo` | cmd | `<factor>` | Slow-motion time scale (`SlowMoCallback`). |
| `stepframe` | cmd | - | Single-step one frame while paused (`StepFrameCallback`). |
| `profile` | bool | 0/1 | Show the CPU profile view (`ProfileView::_bEnabled`). |
| `profileMP` | cmd | - | Show multiplayer packet profile (`ShowPacketProfile`). |
| `ProfileAverage` | float | coeff | Profile averaging filter coefficient (`PblProfileFilterCoeff`). |
| `ProfileMinTime` | float | ms | Min time (ms) to show in profile (`PblProfileMinTimeMs`). |
| `disablegroup` | cmd | `<group>` | Disable a profiler timer group (`PerformanceTimer::DisableGroup`). |
| `enablegroup` | cmd | `<group>` | Enable a profiler timer group (`PerformanceTimer::EnableGroup`). |

## Animation

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `anim.Play` | cmd | `<name>` | Play an animation (`g_fnAnim_Play`). |
| `anim.Show` | cmd | - | Show current animation state (`g_fnAnim_Show`). |
| `anim.Data` | cmd | - | Dump animation data (`g_fnAnim_Data`). |
| `anim.Trace` | cmd | `<0/1>` | Toggle animation tracing (`g_fnAnim_Trace`). |
| `anim.OutputSANM` | cmd | - | Output SANM animation data (`g_fnAnim_OutputSANM`). |
| `DumpAnim` | cmd | - | Dump animation info (`DumpAnim`). |

## Combo (Force-power / combo system)

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `combo.list` | cmd | - | List combos (`g_fnComboList`). |
| `combo.print` | cmd | - | Print combo state (`g_fnComboPrint`). |
| `combo.Damage` | cmd | `<0/1>` | Toggle combo damage debug (`g_fnCombo_Damage`). |
| `combo.Trace` | cmd | `<0/1>` | Toggle combo tracing (`g_fnCombo_Trace`). |

## Sound

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `snd.display` / `SoundDisplay` | cmd | `<0/1>` | Toggle on-screen sound debug display (`SoundDebugDisplayCommand`). |
| `snd.unused` / `SoundUnused` | cmd | - | Scan the loaded sound set for redundant/duplicate definitions and log them (`GameSound::CheckForRedundancies`). |
| `snd.set` | cmd | `<...>` | Sound editor: set a parameter (`SoundEditor::Set`). |
| `snd.view` | cmd | - | Sound editor: view (`SoundEditor::View`). |

## Rendering - collision / raycast debug

All bool; each has a `debugmenu.*` alias to the same flag.

| Name | Type | Backing var | debugmenu alias |
|------|------|-------------|-----------------|
| `RenderAABB` | bool | `gRenderRayHitAABB` | `debugmenu.RenderAABB` |
| `RenderNoCollision` | bool | `gRenderNoCollisionFlag` | `debugmenu.RenderNoCollision` |
| `renderhitlocations` | bool | `gRenderRayHitLocationsFlag` | `debugmenu.renderhitlocations` |
| `RenderOrdCollision` | bool | `gRenderRayHitOrdnanceCollisionFlag` | `debugmenu.RenderOrdCollision` |
| `RenderRigidCollision` | bool | `gRenderRayHitRigidCollisionFlag` | `debugmenu.RenderRigidCollision` |
| `RenderSoftCollision` | bool | `gRenderRayHitSoftCollisionFlag` | `debugmenu.RenderSoftCollision` |
| `RenderStaticCollision` | bool | `gRenderRayHitStaticCollisionFlag` | `debugmenu.RenderStaticCollision` |
| `RenderTerrainCollision` | bool | `gRenderRayHitTerrainCollisionFlag` | `debugmenu.RenderTerrainCollision` |
| `rendersphere` | bool | `gRenderRayHitSphereFlag` | - |
| `renderdamagearrows` | bool | `gRenderDamageArrowsFlag` | `debugmenu.renderdamagearrows` |
| `render_soldier_checking` | bool | `gRenderChecking` | - |
| `render_soldier_colliding` | bool | `gRenderColliding` | - |
| `render_soldier_ignores` | bool | `gRenderIgnores` | - |
| `render_cloth_connections` | bool | `sbDrawConstraints` (draw cloth constraints) | - |

## Rendering - lighting

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `Lighting.Enable` | bool | 0/1 | Master lighting enable (`sLightingEnable`). |
| `Lighting.Draw` | bool | 0/1 | Draw light debug (`gDrawLights`). |
| `Lighting.DrawShadowRegions` | bool | 0/1 | Draw shadow regions (`sDrawShadowRegions`). |
| `Lighting.ProjectionEnable` | bool | 0/1 | Enable projected lighting (`bProjectionEnable`). |
| `Lighting.UseBoxForModels` | bool | 0/1 | Use box (vs sphere) to collect model lights (`gUseBoxToCollectLights`). |
| `Lighting.BottomAmbientColor` | cmd | `<r g b>` | Set bottom ambient color (`SetBottomAmbientColorCallback`). |
| `Lighting.TopAmbientColor` | cmd | `<r g b>` | Set top ambient color (`SetTopAmbientColorCallback`). |
| `Lighting.SetGlobalDirColor` | cmd | `<r g b>` | Set global directional-light color (`SetGlobalDirColorCallback`). |
| `terrain.lightingEnable` | bool | 0/1 | Terrain lighting enable (`bLightingEnable`). |
| `terrain.drawnormals` | bool | 0/1 | Draw terrain normals (`sDrawNormals`). |
| `terrain.enablePerPixel` | bool | 0/1 | Per-pixel terrain lighting (`sEnablePerPixelLighting`). |
| `terrain.forcesimple` | bool | 0/1 | Force simple terrain lighting (`sForceSimpleLighting`). |
| `terrain.forceperpixel` | bool | 0/1 | Force per-pixel terrain lighting (`s_bForcePerPixelLighting`). |
| `terrain.forceperpixel` (cmd) | cmd | `<0/1>` | Setter callback for the above (`gSetForcePerPixelCommand`). |
| `terrain.ForceFullDetail` | bool | 0/1 | Force full terrain detail (`bForceFullDetail`). |
| `Terrain.Enable` | bool | 0/1 | Terrain rendering enable (`bTerrainEnable`). |

## Rendering - shadows

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `Shadow.Enable` | cmd | `<0/1>` | Enable shadows (`ShadowCommand`). |
| `Shadow.BlurEnable` | cmd | `<0/1>` | Enable shadow blur (`ShadowCommand`). |
| `Shadow.Intensity` | cmd | `<v>` | Shadow intensity (`ShadowCommand`). |
| `Shadow.DebugDraw` | bool | 0/1 | Debug-draw shadow volumes (`s_bDebugShadowVolumes`). |

## Rendering - reflections / water

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `render.Reflections` | bool | 0/1 | Master reflections toggle (`enableReflection`). |
| `Reflections.Enable` | bool | 0/1 | Reflections enable (`bReflectionsEnable`). |
| `Reflections.Draw` | bool | 0/1 | Draw reflections (`bReflectionsDraw`). |
| `Water.Enable` | bool | 0/1 | Water rendering enable (`bEnable`). |
| `Water.FresnelMinMax` | cmd | `<min max>` | Water fresnel range (`pcWaterConsoleCommands`). |
| `Water.ReflectionColor` | cmd | `<r g b>` | Water reflection color. |
| `Water.RefractionColor` | cmd | `<r g b>` | Water refraction color. |
| `Water.UnderwaterColor` | cmd | `<r g b>` | Underwater color. |
| `Water.SpecularEnable` | cmd | `<0/1>` | Water specular enable. |

## Rendering - visibility / occlusion (PVS)

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `vis.Enable` | bool | 0/1 | PVS active (`active_`). |
| `vis.DebugDraw` | bool | 0/1 | PVS debug draw (`gEnable`). |
| `vis.BackFaceCull` | bool | 0/1 | Backface cull in vis (`bBackFaceCull`). |
| `vis.ProximityTest` | bool | 0/1 | Vis proximity test (`bProximityTest`). |
| `vis.SaveCam` | bool | 0/1 | Freeze/save vis camera (`bSaveCamPos`). |
| `Occlusion.QueryEnable` | bool | 0/1 | HW occlusion queries (`bOcclusionQueryEnable`). |
| `Occlusion.ItemsEnable` | bool | 0/1 | Occlude items (`bOcclusionItemsEnable`). |
| `Occlusion.DebugDraw` | bool | 0/1 | Draw occlusion debug (`bOcclusionDebugDraw`). |
| `Occlusion.MaxObjectRadius` | float | r | Max radius eligible for occlusion (`MAX_OCCLUSION_RADIUS`). |
| `render.QueryWait` | bool | 0/1 | Wait on occlusion query results (`bQueryWait`). |

## Rendering - props / clusters / models

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `EntityProp.Enable` | bool | 0/1 | Entity prop rendering (`bEntityProp`). |
| `PropCluster.Enable` | bool | 0/1 | Prop cluster rendering (`bPropClusterEnable`). |
| `PropCluster.AlphaFade` | bool | 0/1 | Cluster alpha-fade. |
| `PropCluster.DistCheck1` | bool | 0/1 | Cluster distance check 1. |
| `PropCluster.DistCheck2` | bool | 0/1 | Cluster distance check 2. |
| `PropCluster.DistCheck3` | bool | 0/1 | Cluster distance check 3. |
| `PropCluster.RenderClipped` | bool | 0/1 | Render clipped clusters. |
| `PropCluster.TraceRenders` | bool | 0/1 | Trace cluster renders. |
| `PropModel.Enable` | bool | 0/1 | Prop model rendering (`bPropModelEnable`). |
| `PropModel.AlphaFade` | bool | 0/1 | Prop model alpha-fade. |
| `PropModel.DistCheck1` | bool | 0/1 | Prop model dist check 1. |
| `PropModel.DistCheck2` | bool | 0/1 | Prop model dist check 2. |
| `PropModel.DistCheck3` | bool | 0/1 | Prop model dist check 3. |
| `PropModel.RenderClipped` | bool | 0/1 | Render clipped prop models. |
| `PropModel.SphereCullCheck` | bool | 0/1 | Sphere cull check (`bSphereCullCheck`). |
| `PropModel.MidFrameFlush` | bool | 0/1 | Mid-frame flush (`bMidFrameFlush`). |
| `PropModel.FadeAdj` | float | v | Prop model fade adjust (`fPropModelFadeAdj`). |
| `PropGen.DelayedFlush` | bool | 0/1 | Prop-gen delayed flush (`bDelayedFlush`). |

## Rendering - vegetation / sky / particles / misc

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `SkyDome.Enable` | bool | 0/1 | Sky dome render (`bEnable`). |
| `GodRay.UpdateEnable` | bool | 0/1 | God-ray update enable (`bEnable`). |
| `GrassPatch.Enable` | bool | 0/1 | Grass patches (`bGrassPatchEnable`). |
| `LeafPatch.Enable` | bool | 0/1 | Leaf patches (`bLeafPatchEnable`). |
| `particles.Enable` | bool | 0/1 | Particle rendering (`bParticlesEnable`). |
| `Weapon.Render` | bool | 0/1 | Weapon model render (`bWeaponRender`). |
| `bWeaponMelee.Render` | bool | 0/1 | Melee weapon render (`bWeaponMeleeRender`). |
| `LightSaberTrail.Enable` | bool | 0/1 | Lightsaber trail effect (`bLightSaberTrail`). |
| `flyer.contrailsActive` | bool | 0/1 | Flyer contrails (`bRenderContrails`). |
| `SoldierModels` | bool | 0/1 | Controls character level of detail rather than hiding anything. It defaults to on (`1`), which uses the normal distance-based detail. Setting it to `0` forces every animated character, including soldiers, droidekas, and their addons, to draw at a fixed low detail level, so they appear as their low-rez variants even up close (`debug_models_on`). |
| `ToggleSoldierModels` | cmd | - | Flips the exact same flag as `SoldierModels`, so it has identical effect. Each toggle switches animated characters between normal detail and the forced low-rez variant (`debug_toggle_models_on`, which inverts `debug_models_on`). |
| `rendering.soldiers` | bool | 0/1 | Render soldiers (`bRenderSoldiers`). |
| `rendering.disableLod3` | bool | 0/1 | Disable LOD3 (`gDisableLod3`). |
| `rendering.showMissingLOD` | bool | 0/1 | Highlight missing LODs (`bShowMissingLOD`). |
| `rendering.maxVertexDensity` | float | v | Max vertex density (`rMaxVertexDensity`). |
| `rendering.minScreenSize` | float | v | Min on-screen size to draw (`rMinScreenSize`). |
| `render.FarScene` | bool | 0/1 | Far-scene rendering (`gEnable`). |
| `render.SortMaterial` | bool | 0/1 | Sort draws by material (`bSortMaterial`). |
| `Rendering.SortShaderFirst` | bool | 0/1 | Sort by shader first (`bSortShaderFirst`). |
| `render.UseStateManager` | bool | 0/1 | Use render state manager (`stateManagerEnabled_`). |
| `nearScene.PushInfinite` | bool | 0/1 | Push near-scene depth to infinity (`NearScenePushInf`). |
| `SoldierPolyCount` | bool | 0/1 | Show soldier poly count (`gShowSoldierPolyCount`). |
| `showpolycount` | bool | 0/1 | Show game-model poly count (`gShowGameModelPolyCount`). |
| `showsegmentcount` | bool | 0/1 | Show segment count (`gShowSegmentCount`). |
| `walker.ShowDamage` | bool | 0/1 | Show walker damage state (`g_bWalkerShowDamage`). |
| `warning_colors` | bool | 0/1 | Static warning colors overlay (`gStaticWarningColorsOn`). |

## Post-processing

### Blur

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `Blur.Enable` | cmd | `<0/1>` | Enable blur post effect (`BlurEnable`). |
| `Blur.ConstantBlend` | stub | - | **No-op.** Registered name, value driven by blur command path. |
| `Blur.DownSizeFactor` | stub | - | **No-op.** Downsize factor. |

### HDR (all → `HDRCommand`)

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `HDR.Enable` | cmd | `<0/1>` | Enable HDR/bloom. |
| `HDR.DownSizeFactor` | cmd | `<v>` | Bloom downsize factor. |
| `HDR.GlowFactor` | cmd | `<v>` | Glow factor. |
| `HDR.GlowThreshold` | cmd | `<v>` | Glow threshold. |
| `HDR.MaxTotalWeight` | cmd | `<v>` | Max total bloom weight. |
| `HDR.NumBloomPasses` | cmd | `<n>` | Number of bloom passes. |

### Color control (all → `ColorControlCommand`)

ColorControl is the game's color-grading post-process, which remaps the final image
through gamma, brightness, and contrast curves. The two live commands both accept an
optional integer argument, but in practice they ignore it and simply toggle the current
state each time you run them.

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `ColorControl.Enable` | cmd | `[0/1]` | Turns the color-grading post-process on or off at the platform level, toggling its current state on each call (`ColorControl::PlatformEnable`). |
| `ColorControl.Draw` | cmd | `[0/1]` | Shows or hides the on-screen color-curve graph, which is the visual display of the grading curve, toggling it on each call (the `ColorControl` graph element). |
| `ColorControl.GammaContrast` | stub | - | **No-op.** Registered with no backing storage, setting it from the console does nothing. |
| `ColorControl.GammaBrightness` | stub | - | **No-op.** Registered with no backing storage, setting it from the console does nothing. |
| `ColorControl.GammaCorrection` | stub | - | **No-op.** Registered with no backing storage, setting it from the console does nothing. |

## Fog (all → `SetFogCallback`)

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `fog.color` | cmd | `<r g b>` | Fog color. |
| `fog.range` | cmd | `<near far>` | Fog range. |
| `fog.worldrange` | cmd | `<near far>` | World fog range. |
| `fog.reflectioncolor` | cmd | `<r g b>` | Fog reflection color. |

## Immediate-mode scene callbacks

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `ImmediateMode.FarScene` | cmd | `<0/1>` | Toggle immediate-mode far scene (`SetImmediateModeFarSceneCallback`). |
| `ImmediateMode.NearScene` | cmd | `<0/1>` | Immediate-mode near scene. |
| `ImmediateMode.ReflectionScene` | cmd | `<0/1>` | Immediate-mode reflection scene. |
| `ImmediateMode.ZPrepass` | cmd | `<0/1>` | Immediate-mode Z-prepass. |

## Screenshots

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `Screenshot` | cmd | - | Request a screenshot (`RequestScreenshot`). |
| `screenshot.gammacorrection` | float | v | Screenshot gamma correction (`gamma_correction`). |
| `screenshot.samplesperpixel` | int | n | Supersample samples/pixel (`samples_per_pixel`). |
| `screenshot.sizeincrease` | int | n | Screenshot size multiplier (`size_increase`). |
| `CreateCubeMap` | cmd | - | Render a cube map (`CreateCubeMap`). |

## Dump / diagnostic tools

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `Hardware` | cmd | - | Dump hardware info (`DumpHardware`). |
| `Mem` | cmd | - | Dump memory usage (`DumpMemory`). |
| `DumpPools` | cmd | - | Dump memory pools (`DumpPools`). |
| `Strips` | cmd | - | Dump model strip info (`DumpStripInfo`). |
| `DumpModel` | cmd | - | Dump model info (`DumpModel`). |
| `DumpTexture` | cmd | - | Dump texture info (`DumpTexture`). |
| `ViewLod` | cmd | `<n>` | Force/view a LOD level (`ViewLod`). |
| `showtopcollision` | cmd | - | Dump the highest-cost collision-mesh entries (the "top collision" offenders) to the log/console (`PrintTop10`). |
| `debugbump` | cmd | `[normals \| s \| t \| uvs \| <n>]` | Visualise mesh surface data: `normals`, tangent-`s`, tangent-`t`, or `uvs` (or the equivalent index). Sets the `s_iDebugNormals` view mode; no arg reports the current mode (`SetDebugBumpCallback`). |
| `PrintPlayerCoords` | cmd | - | Print player coordinates (`JPrintPlayerCoords`). alias `debugmenu.PrintPlayerCoords` |
| `DebugMenu.PrintPostCoords` | cmd | - | Print command-post coordinates (`JPrintCPToDisplay`). |
| `Lua` | cmd | `<code>` | Execute a Lua string (`LuaDoString`). |

## Networking

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `net.nearobjects` | cmd | - | Request a one-shot dump of the networked objects near the player (sets `listNearObjects`, consumed next net tick). |
| `net.toggleqostext` | cmd | - | Toggles the network Quality-of-Service text overlay, which shows on-screen connection stats such as latency and packet health (`QosTextEnable`). alias `debugmenu.ToggleQosDisplay` |
| `Net.CloseJournal` | cmd | - | Close the net journal (`CloseJournalCommand`). |
| `profileMP` | cmd | - | MP packet profile (see Profiling). |

## Display / HUD toggles

| Name | Type | Params | Description |
|------|------|--------|-------------|
| `ToggleDisplay` | cmd | - | Shows or hides the entire HUD at once. It enables or disables every HUD viewport together, including the team, reticule, stat-icon, status, capture, message, boot, hero-message, change-class and spectator displays, the scope, and all HUD screen groups (`ToggleAllDisplay`). alias `debugmenu.ToggleDisplay` |
| `BackDrop` | cmd | - | Shows or hides a solid colored panel that covers the top half of the screen behind the console's text output. The panel is created hidden and lives inside the console's element group, so you only see it while the console overlay is actually on screen. Toggling it during normal play with no console showing looks like nothing happens (`RedConsole::ToggleBackDrop`). |
| `ToggleDudes` | cmd | - | Blanks the spawn and class-select screen when turned off, clearing the spawn-timer text, backdrop, unit preview, and team display. (`gDrawDudes`). |
| `debugmenu.ToggleHolos` | cmd | - | Toggles rendering of the in-world holograms, meaning the holographic projector displays such as command-post holo-tables and holo unit markers (`gRenderHolos`). |
| `debugmenu.ToggleFreeLook` | cmd | - | Enters free-look camera, and if you are already in free-look it drops back to the chase camera during play or to the map camera otherwise (`JToggleFreeLook`). |
| `debugmenu.ToggleForceConsoleOff` | cmd | - | Toggles whether warnings and errors automatically open the on-screen console, by flipping the severity threshold on the open-console log destination (`JToggleForceConsoleOff`). |
| `DrawEntityPaths` | cmd | - | Toggle debug lines showing the paths entities are currently following (`EntityPathFollower` debug lines). |
| `showreticuleinfo` | bool | 0/1 | Show reticle/aim info (`gShowReticuleInfo`). |
| `map.displayall` | bool | 0/1 | Reveal the whole map (`gMapDisplayAll`). |
| `FarSceneTint` | bool | 0/1 | Tint the far scene (`gTintFarScene`). |

---

## Notes / caveats

- Several commands share one callback and select behavior by the command **id** passed to
  the callback (e.g. all `fog.*` → `SetFogCallback`, all `HDR.*` → `HDRCommand`, all
  `Water.*` → `pcWaterConsoleCommands`, `freecamlight.*` → `SetFreeCamLightCallback`).
- `debugmenu.*` aliases exist only to let the in-game Debug Menu drive the same flag; they
  are not separate features.
- This list is the engine's built-in set. Project-specific Lua functions added by
  BF2GameExt are **not** included here by design.