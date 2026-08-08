# Tutorials

Step-by-step guides for the things BF2GameExt adds, each with munge-ready example
assets you can drop straight into a mod folder. For the full parameter lists, see
the reference pages linked from the [main readme](../../README.md).

| Guide | What it covers |
|-------|----------------|
| [Detecting BF2GameExt](Basics.md) | **Read this first:** The `GameExt` table, where to put the check, and how to keep a mod playable for people who do not have the extension installed. Calling an extension function without a guard is a hard error that kills the rest of your script. |
| [Custom loading screens](loading-screen.md) | Building your own `load.lvl` with a custom `load.cfg` and pointing a mission at it, plus the two features on top: spinning team models, and the BF1 loading screen sequence. |

## Example assets

The folders under [`GameAssets/Examples`](../../GameAssets/Examples) are complete,
munge-ready inputs for the guides above. Copy one into your mod's `data_XXX\Load\`
and run the Load munge.

| Folder | Used by |
|--------|---------|
| [`LoadingScreen-TeamModels`](../../GameAssets/Examples/LoadingScreen-TeamModels) | [Custom loading screens](loading-screen.md), step 4 |
| [`LoadingScreen-BF1`](../../GameAssets/Examples/LoadingScreen-BF1) | [Custom loading screens](loading-screen.md), step 5 |

## Reference pages

- [Features](../user/FEATURES.md) - everything the extension adds and fixes
- [Lua API](../user/LUA_API.md) - functions callable from mission scripts
- [Loading Screen](../user/LOADING_SCREEN.md) - every loading screen parameter
- [Configuration](../user/CONFIGURATION.md) - `BF2GameExt.ini`
- [Troubleshooting](../user/TROUBLESHOOTING.md) - it did not load, or a feature does nothing
