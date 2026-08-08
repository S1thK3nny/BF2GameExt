# Detecting BF2GameExt from a mission script

[< Back to tutorials](README.md) | Reference: [Lua API](../user/LUA_API.md)

BF2GameExt adds Lua functions the stock game does not have. Calling one on a
machine without the extension is a **hard error**: Lua raises
`attempt to call global 'X' (a nil value)`, the script dies at that line, and
everything after it - your teams, your objectives - never runs. The map loads
to a black screen or drops back to the shell.

So a mod that wants to use the extension has to answer one question first:
*is it actually here?*

## The check

The extension sets a global table named `GameExt` alongside its function
registrations. In vanilla it is simply `nil`.

```lua
if GameExt then
    -- BF2GameExt is loaded, extension functions are safe to call
end
```

That is the whole mechanism. The table also tells you what you are running on:

| Field | Value |
|-------|-------|
| `GameExt.version` | Version string, e.g. `"1.0.0"` |
| `GameExt.build` | Which executable is patched: `"modtools"`, `"steam"`, `"gog"` or `"unknown"` |

```lua
if GameExt then
    print("BF2GameExt " .. GameExt.version .. " on " .. GameExt.build)
end
```

`GameExt.build` matters because not every feature exists on every executable -
see the [compatibility table](../../README.md#compatibility). If a feature you
depend on is modtools-only, test the build, not just the table.

## Where to put it

Both of these work, and they behave differently:

```lua
-- Script root: runs the moment the mission script is loaded, before anything
-- else. Fine for the loading screen redirect, though ScriptPreInit works too -
-- the engine builds the loading screen well after both of them.
if GameExt then
    SetLoadDisplayLevel("dc:LOAD\\load.lvl")
end

function ScriptPreInit()
    if GameExt then
        SetMemoryPoolSize("EntityCarrier", 10)
    end
end
```

## Checking for one function

`GameExt` tells you the extension is loaded. It does not tell you the version
you are running has the specific function you want - an older install may
predate it. Lua lets you test that directly, because an unregistered function
is just a nil global:

```lua
if SetLoadDisplayLevel then
    SetLoadDisplayLevel("dc:LOAD\\load.lvl")
end
```

Use whichever reads better. `if GameExt then` states the dependency; testing
the function name survives version drift. For a mod that has to run on installs
you do not control, test the function.

## Turning the extension's gameplay features off

Some of what BF2GameExt adds is intrusive by design - prone, for one, changes
how infantry move. A mod that wants the crash fixes and the Lua API but *not*
the gameplay changes can opt out per mission:

```lua
function ScriptPreInit()
    if GameExt then GameExt.disable = true end
end

-- later...
function ScriptInit()
    ReadDataFile("ingame.lvl")
end
```

`GameExt.disable` is write-only and must be set **before** the
`ReadDataFile("ingame.lvl")` call for it to take effect that mission. It
suppresses the gameplay-altering features and leaves the fixes and the API
alone.