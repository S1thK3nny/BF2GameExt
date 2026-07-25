# Configuration

All runtime options are controlled via `BF2GameExt.ini` (only used with the DInput8 Proxy method). If the INI file is absent, all features are enabled by default except those that require additional assets (e.g. Prone).

| Section | Purpose |
|---------|---------|
| `[General]` | Master enable switch, DLL path |
| `[LimitIncreases]` | Engine limit patches (heap, sound, objects, etc.) |
| `[Fixes]` | Bug-fix patches |
| `[Features]` | Optional gameplay features (e.g. Prone), diagnostics (game logging), and AI behavior toggles (dead-body shooting) |
| `[Controller]` | Gamepad enable and rumble toggles |
| `[Controller.*]` | Per-mode button/axis bindings (Unit, Vehicle, Flyer, Hero, Turret) |

The INI file is generated from the C++ source of truth. To regenerate after adding new features:

```
python generate_ini.py
```

Gameplay features are also configured through `load.cfg` parameters, ODF properties, and Lua functions.
