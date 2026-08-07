# Custom loading screens

[< Back to tutorials](README.md) | Reference: [Loading Screen](../user/LOADING_SCREEN.md)

The stock game reads one hardcoded `load.lvl` and gives you no way to change it.
BF2GameExt lets a mission point at its own, and adds two bigger features on top of the
stock screen:

- **Team models** - two spinning 3D models, one per side. BF2 builds the feature
  and then switches it off; the extension turns it back on and makes the
  placement configurable.
  Example assets: [`GameAssets/Examples/LoadingScreen-TeamModels`](../../GameAssets/Examples/LoadingScreen-TeamModels)
- **The BF1 zoom sequence** - galaxy to planet to region to battlefield, with the
  crosshair frame closing on each target. BF2 dropped it entirely.
  Example assets: [`GameAssets/Examples/LoadingScreen-BF1`](../../GameAssets/Examples/LoadingScreen-BF1)

Steps 1-3 and 6 below are the same for any custom loading screen. You can merge steps 4-5 into your own `load.cfg` or use the example ones as a starting point.  

This does not go over all new parameters added by the extension; see the [reference](../user/LOADING_SCREEN.md) for that.

<!-- VIDEO: a custom loading screen running in BF2.
     Drag the mp4 into a GitHub issue/PR comment box, wait for the upload, then
     replace this whole comment with the URL GitHub hands back - the same
     https://github.com/user-attachments/assets/... form as the readme image:
     <video src="https://github.com/user-attachments/assets/..." controls muted></video> -->

## 1. Make the Load folder

A custom loading screen is its own little level. In your mod folder:

```
data_XXX\
    Load\
        load.req
        load.cfg
        msh\
```

`data_XXX\_BUILD\Load\munge.bat` already knows to look here.

## 2. Drop the art in

Copy your `.msh` files (and their `.msh.option`) and every `.tga` they use into
`Load\msh\`.

> ### Rename everything
>
> Loading screen assets and mission assets share one global, name-keyed table.
> **Any model or texture your load.lvl brings in will be missing from the
> mission if the mission uses the same name**, because the loading screen frees
> its own copy on teardown and takes the shared one with it.
>
> For the example, I took `com_icon_alliance.msh` and `com_icon_imperial.msh` as well as their textures, 
> renamed them accordingly with a _load suffix (and of course updated the mshs to use the new textures)
>
> If I would have used the same names as the mission, the loading screen would have freed them and
> you would clearly see the holo icons missing for the teams.

## 3. Write load.req
```
ucft
{
    REQN
    {
        "texture"
        "loading"
        "loadtipsbox_pieces"
    }

    REQN
    {
        "model"
        "com_icon_alliance_load"
        "com_icon_imperial_load"
    }

    REQN
    {
        "config"
        "load"
    }
}
```

The `"config"` entry with `"load"` is what pulls in `load.cfg`.  
Every texture you name in `load.cfg` has to appear in the `"texture"` block.

## 4. Team models

Start from an example `load.cfg`, whether stock or the one in the example folder, and add the team model keys if not already present:

```
LoadDisplay()
{
    // ... stock keys: TipsBoxTexture, ProgressBar*, RandomBackdrop, etc ...

    TeamModel("1", "com_icon_imperial_load");
    TeamModel("2", "com_icon_alliance_load");

    TeamModelScale(90);
    TeamModelRotationSpeed(2.0);

    TeamModelOffset("1", 0.125, 0.15);
    TeamModelOffset("2", 0.875, 0.85);
}
```

Important to know:

- **`"1"` and `"2"` are screen slots, not teams.** They do not track which
  faction is attacking. Slot 1 is whatever you put in slot 1.
- **The slot must be quoted.** A bare `1` is a number, not a string, and cannot
  be read as one. It will be logged as an error and ignored.
- **Offsets are fractions of the screen from the top left**, `+x` right and `+y`
  down, so `(0.5, 0.5)` is dead centre. They are resolution independent.
- **Scale and rotation speed are shared** by both slots. Only the model and the
  offset are per-slot. The two models counter-rotate.

One model with the other slot left empty is fine.  
This does not need to be in the `LoadDisplay()` block; it can be in a `Map()` block instead, so different maps can have different models and offsets.

## 5. The BF1 zoom sequence

You give it a stack of **levels**. Each level is one full-screen image plus the
rectangle on it that the next level lives inside. For each level in turn:

| Phase | Time | What happens | Sound |
|-------|------|--------------|-------|
| 1 | 1.2 s | Bands close in on the target's top and bottom edges | `YTrackingSound` |
| 2 | 0.4 s | Hold | - |
| 3 | 1.2 s | Bands close in on the left and right edges | `XTrackingSound` |
| 4 | 0.4 s | Hold | - |
| 5 | 1.5 s | The framed region expands to fill the screen, cross-fading into the next level | `ZoomSound`, then `TransitionSound` |

**4.7 seconds per level.** Three zooms is roughly 14 seconds, and the extension
holds the loading screen open until the sequence finishes rather than cutting it
off, so a fast-loading map waits for it. There is a 30 second safety cap. Budget
accordingly: five levels is 23 seconds of staring at a screen.

The last level is the destination. It has no target rect, so nothing zooms out of
it.  
The sequence ends there and holds on that image. While `EnableBF1` is on, the
stock `RandomBackdrop` is suppressed; your level images *are* the backdrop.

### The images

One full-screen image per level. In the example they are 1024x1024 24-bit TGAs directly from BF1, but any size and format the engine supports is fine. The example names are: 
- `load_galacticmap_0`
- `load_locale_Geonosis_1`
- `load_landsat_Geonosis_2`
- `load_level_Geonosis_3`

The numeric suffix is a naming convention but not a requirement. What matters is the index you pass to `PlanetLevel`.

The crosshair frame pieces (`load_edge_horz`, `load_edge_vert`,
`load_edge_corner`) are already in the example folder and its `load.req`. Ship them unchanged.

### Work out the rectangles

This is the part that takes some calculation. For each level you need the rect where the
*next* level's subject sits, as fractions of the image: `x`, `y` for the top-left
corner, `w`, `h` for width and height.

BF1's own numbers are pixels and cannot be pasted in - divide by the image size
first. For a target at `[1152, 108]` to `[1728, 594]` on a 1920x1080 image:

```
x = 1152 / 1920 = 0.600
y =  108 / 1080 = 0.100
w =  576 / 1920 = 0.300     (1728 - 1152)
h =  486 / 1080 = 0.450     ( 594 -  108)
```

Open the image in any editor with a pixel-coordinate readout, note the corners of
the thing you want to zoom into, divide, done.

The numbers are resolution independent. The level image is stretched to fill the
screen, so a fraction of the image is the same fraction of the screen on every
monitor, and a rect worked out once frames the same subject at any resolution or
aspect ratio.

### The config

`EnableBF1(1)` is the master switch. Without it, every key below is parsed but
thrown away, silently.

```
LoadDisplay()
{
    // ... stock keys ...

    EnableBF1(1);

    ZoomSelectorTextures("load_edge_horz", "load_edge_vert", "load_edge_corner");

    XTrackingSound("load_xtracking");
    YTrackingSound("load_ytracking");
    ZoomSound("load_zoom");
    TransitionSound("load_transition");
    BarSound("load_bar");
    BarSoundInterval(1);

    // OPTIONAL; It just looks better when the tips box is gone, but you can leave it if you want to keep the tips. (or remove even more from the screen)
    RemoveToolTips(1);
    RemoveLoadingBar(1);
}

Map("ABCc_con")
{
    PC()
    {
        PlanetLevel(0, "load_galacticmap_0",      0.380859375, 0.7197265625, 0.0361328125, 0.0419921875)
        PlanetLevel(1, "load_locale_Geonosis_1",  0.3515625,   0.5419921875, 0.037109375,  0.0361328125)
        PlanetLevel(2, "load_landsat_Geonosis_2", 0.3515625,   0.5419921875, 0.037109375,  0.0361328125)
        PlanetLevel(3, "load_level_Geonosis_3",   0,           0,            0,            0)
    }
}
```

- **The destination level gets a rect of all zeroes.** That is how the sequence
  knows where to stop - the extension counts levels with a real rect from the
  front and stops at the first zero one.
- **`PlanetLevel` goes in `Map()`**, usually inside a `PC()` block. It also works
  at `LoadDisplay()` top level if every map shares the sequence.
- **The BF1 sounds ship with the game.** The BF1 sounds used for the loading screen are still present in the `global.snd` inside `core.lvl`, which stays resident all session, so naming them is all it
  takes. If you would like to use your own sounds, use `LoadSoundLVL(lvlPath)` instead.

A `Map()` block overrides `LoadDisplay()` key by key and inherits the rest, and
its ID is the string your mission script is named after.  
So one `load.lvl` can serve a whole mod, with individual maps opting back out:

```
Map("ABCg_con")
{
    EnableBF1(0);          // this map gets an ordinary screen
    RemoveToolTips(0);
    RemoveLoadingBar(0);
}
```

## 6. Munge and hook it up

Run VisualMunge, ZeroMunge or even just the `data_XXX\_BUILD\Load\munge.bat` on its own.
The output lands at `data_XXX\_LVL_PC\LOAD\load.lvl`; Make sure it lands at
`addon\XXX\data\_LVL_PC\LOAD\load.lvl` so that `SetLoadDisplayLevel` can find it (unless you use a raw addon path).

Then point the mission at it with `SetLoadDisplayLevel`. Either **script root** or
**`ScriptPreInit`** works, both run well before the loading screen is built.
`ScriptInit` is too late. Guard the call, so the map still loads for players
without the extension:

```lua
-- Vanilla script root, only here for your orientation.
ScriptCB_DoFile("setup_teams")
ScriptCB_DoFile("ObjectiveConquest")

if GameExt then
    SetLoadDisplayLevel("dc:LOAD\\load.lvl")
    -- or a raw path (.lvl extension IS optional):
    -- SetLoadDisplayLevel("..\\..\\addon\\XXX\\data\\_LVL_PC\\LOAD\\load")
end
```

## Troubleshooting

- **Nothing appears / the screen looks stock.** 
  - Check `BFront2.log` for `[BF1Ext]` lines
  - A rejected key names itself and its reason. The usual causes are an unquoted
team model slot, a model name that is not actually in the lvl, a missing
`EnableBF1(1)`, or a `Map()` block whose ID does not match the mission.

- **Black screen where a zoom level should be.** 
  - The texture is not in the lvl. Check that every `PlanetLevel` texture name is in a `"texture"` REQN, then check
`BFront2.log` for the `[BF1Ext]` line naming the hash it could not resolve.

- **The model is enormous.** 
  - `TeamModelScale` is a multiplier and the
sensible value depends entirely on your mesh. Mess around with the scale, make sure your models are centered at (0.5, 0.5) for easiest debugging.

- **The model is not there.**
  - Again, for the easiest debugging, make sure your model is centered at (0.5, 0.5) and not offset to one side. 
  - Check the `BFront2.log` for `[BF1Ext]` lines about missing models.

- **The crosshair closes on the wrong spot.** 
  - Rects are fractions of the image, not pixels, and are measured from the top left. If it is nowhere near the subject,
suspect pixel coordinates that were never divided by the image size.

- **It worked, but now specific mshs/textures in the map are missing.** 
  - The name collision from step 2. Rename the loading screen copy.

- **It works on one map and not another.** 
  - `SetLoadDisplayLevel` is per mission script. A map that does not call it gets the stock screen. Annoying, but otherwise nothing tells the engine to use a different one.

- **Assets dropped.** 
  - The loading screen holds at most 10 models, 50
textures and 10 skeletons. BF2GameExt logs what it dropped, so you lose the
extra art instead of the session.
