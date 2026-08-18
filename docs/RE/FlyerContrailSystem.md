# Flyer Contrail System — RE Notes

`EntityFlyer::UpdateContrailEffects` and the two per-contrail ODF speed properties.

**Status: not shipped.** A fix was written (`entity/flyer_contrail_speed_fix.cpp`,
INI `[Fixes] FlyerContrailSpeedFix`) for all three builds and then removed before
release on 2026-08-18. Making the contrails obey the ODF is technically correct but
the vanilla assets do not survive it: the stock X-wing and TIE fighter ask for their
large contrails at every speed, so with the fix on they trail permanently instead of
only at high speed. Fixing that would mean editing vanilla ODFs, which is out of
scope for the extension. These notes are kept as reference in case the fix is
revisited (e.g. opt-in per class rather than global).

---

## The shipped function

| Build | Address |
|---|---|
| Phantom (dev, symbolized) | `0x00535320` |
| modtools | `0x004F7780` (LTCG thunk `0x00416C07`) |
| Steam | `0x004ABF20` |
| GOG | `0x004ABF20` |

Read back out of the disassembly, identical on all three:

```c
threshold = MaxSpeed + 0.1f * (BoostSpeed - MaxSpeed);
if (BoostSpeed <= threshold) BoostSpeed = threshold * 1.5f;
if (speed < threshold) { mTurbulance = 0; TurnOffContrailEffects(); return; }

t = clamp((speed - threshold) / (BoostSpeed - threshold), 0, 1);
mTurbulance = t;

for (i = 0; i < 12; i++) {
    if (ContrailEffect[i] == 0) break;
    if (ContrailEffectMinSpeed[i] >= 0.5f)              // literal 0.5
        ScaleSet(effect, lerp(MinScale[i], MaxScale[i], t));
    else
        destroy effect;
}
```

Constants verified against the images, not inferred:
`0.1` at Phantom `0x009E0B80` / modtools `0x00A2C074`, `1.5` at `0x009E0B88`,
`0.5` at Phantom `0x009E095C` / modtools `0x00A2A0CC`, `1.0` at `0x009E0964`.

## Three defects

1. **The cut-in speed is hardcoded.** It is always exactly 10% of the way from
   `MaxSpeed` to `BoostSpeed`, and no per-contrail property moves it. For the
   stock fighters (`MaxSpeed 95`, `BoostSpeed 150`) that is 100.5, i.e. 0.67 of
   the full speed range. Pandemic knew — `all_fly_xwing_sc.odf:423` carries the
   comment `//MaxSpeed = "95" = .63 Contrails at .69`.

2. **`ContrailEffectMinSpeed` is compared against the constant `0.5`**, not
   against the flyer's speed, so it is a boolean rather than a threshold. Any
   contrail authored below 0.5 is permanently dead — including the four large
   contrails on the stock X-wing and TIE fighter (`ContrailEffectMinSpeed = "0.0"`,
   scale 1.6-2.8), which have never rendered in the shipping game.

3. **`ContrailEffectMaxSpeed` is read by nothing.** It is parsed into
   `EntityFlyerClass` and defaulted in the ctor; the update loop uses its offset
   only as the loop terminator (`CMP EDI, 0x10C0` / `0xFF8`).

## What the properties were meant to mean

The ctor defaults (modtools `0x005220FA`..`0x0052211B`) are the evidence:

```
mContrailEffectMinScale[i] = 1.0
mContrailEffectMaxScale[i] = 1.0
mContrailEffectMinSpeed[i] = 1.1     (0x3F8CCCCD)
mContrailEffectMaxSpeed[i] = 1.6     (0x3FCCCCCD)
```

Both speeds are >1, so they cannot be thresholds on the clamped 0..1 boost ramp.
They read as multiples of `MaxFlyerSpeed`: `1.6` is `BoostSpeed/MaxSpeed` for
every stock fighter (150/95 = 1.58), and `1.1` sits just above the hardcoded
gate's own 1.058. So the intended curve is

```
ratio = speed / MaxFlyerSpeed
on when ratio >= ContrailEffectMinSpeed[i]
scale = lerp(MinScale[i], MaxScale[i],
             clamp((ratio - MinSpeed[i]) / (MaxSpeed[i] - MinSpeed[i]), 0, 1))
```

which is what the fix implements. `mTurbulance` is still computed with the
vanilla arithmetic, fallback included, because other systems read it.

## Addresses

| | modtools | Steam | GOG |
|---|---|---|---|
| `UpdateContrailEffects` | `0x004F7780` | `0x004ABF20` | `0x004ABF20` |
| `TurnOnContrailEffect` (RET 4) | `0x004F7560` | `0x004ABD40` | `0x004ABD40` |
| `TurnOffContrailEffects` (RET 0) | `0x004F7700` | `0x004ABED0` | `0x004ABED0` |
| `ParticleEmitterObject::ScaleSet` | `0x0066A0E0` | `0x0060BEC0` | `0x0060CF60` |
| `bRenderContrails` | `0x00ACDC5C` | `0x007E6323` | `0x007E7323` |
| `PblHandle<T>::operator->` | `0x0041445C` | `0x00428E40` | `0x00428E10` |

`ScaleSet` changes calling convention between builds: modtools is a plain
`__thiscall(this, float)` RET 4, the retail builds are LTCG leaves taking the
scale in **XMM1** with RET 0. Body is the same either way — `[this+0xD8]` is the
first emitter node, `[node+0x18]` is the scale, `[node+0x34]` the next node.

The retail functions were found from the `flyer.contrailsActive` string
(`0x0079B1D4` on Steam) via its cvar registration `0x00402AF0`, which names
`bRenderContrails`; that byte has exactly two `.text` readers,
`TurnOnContrailEffect` and `UpdateContrailEffects`. GOG was ported with
`tools/port_gog.py` (`code` at score 1.00, `data` with 3 concurring sites) and
then compared against Steam in lockstep across the whole function: only
relocated global operands and two callee VAs differ.

## Struct offsets

`EntityFlyer` (the `this` passed to `UpdateContrailEffects`):

| field | modtools | Steam / GOG |
|---|---|---|
| `mState` (2 == flying) | `0x5A4` | `0x564` |
| current speed | `0x5F8` | `0x5B8` |
| `EntityFlyerClass*` | `0x66C` | `0x62C` |
| `mTurbulance` | `0x1CD4` | `0x1C94` |
| `PblHandle mContrailEffect[12]` (stride 8) | `0x1D6C` | `0x1D2C` |

`EntityFlyerClass`:

| field | modtools | Steam / GOG |
|---|---|---|
| `mMaxFlyerSpeed` | `0x894` | `0x7CC` |
| `mBoostFlyerSpeed` | `0x898` | `0x7D0` |
| `mContrailEffectName[12]` | `0xF40` | `0xE78` |
| `mContrailEffectMinScale[12]` | `0x1030` | `0xF68` |
| `mContrailEffectMaxScale[12]` | `0x1060` | `0xF98` |
| `mContrailEffectMinSpeed[12]` | `0x1090` | `0xFC8` |
| `mContrailEffectMaxSpeed[12]` | `0x10C0` | `0xFF8` |

The usual retail split holds throughout — instance `-0x40`, class `-0xC8`.

`PblHandle<ParticleEmitterObject>` is `{ void* mObject; uint32 mSavedHandleId; }`;
a handle is live when `*(uint32*)((char*)mObject + 0x1C) == mSavedHandleId`.
Tearing an effect down is vtable slot `+0x1C` on the object, called with no args.

## Related

- `PatcherDLL/src/entity/flyer_boost_animation.cpp` — same EntityFlyer /
  EntityFlyerClass split, derived independently from `Render` and `TakeOff`.
- [EntityCarrierSystem.md](EntityCarrierSystem.md) — the rest of the
  EntityFlyer / EntityCarrier layout.
