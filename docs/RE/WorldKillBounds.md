# World kill bounds

The "invisible death zone" modders hit at the edge of a big map. It is a literal
box in two update functions, not `SetWorldExtents` and not anything in a `.lvl`.
GameExt exposes it as `SetWorldKillBounds` (`PatcherDLL/src/entity/world_kill_bounds.cpp`).

## The check

`EntitySoldier::Update` (decomp `EntitySoldier.cpp`, Steam `0x004E8920`, modtools
`0x00545B20`), skipped on MP clients:

```cpp
if (!(NetGame::IsEnabled() && NetGame::IsClient()))
{
    if (mVelocity.y < -60.0f || mMatrix.m[3][1] < -1000.0f || fabsf(mMatrix.m[3][0]) > 2500.0f ||
        fabsf(mMatrix.m[3][2]) > 2500.0f || mMatrix.m[3][1] > 1000.0f)
    {
        Kill();
        return false;
    }
}
```

So the limits are y in [-1000, 1000], |x| and |z| <= 2500, and a fall faster than
60 m/s. The often quoted "+/-1024 vertical" is wrong; it is 1000. modtools logs
`Unit flew out of world, x = %f, z = %f` / `Unit flew over world, ...` first when the
debug flag at `0x00CF6FE4` is set.

`EntityItem::Update` (Steam `0x004CB210`, modtools `0x005191A0`) removes a falling,
non-stationary pickup below y = -1000, same client gate.

No other class has the side or top test, which is why vehicles cross freely. Other
classes do have floors and ordnance has its own box, see below.

## Encoding per build

| | fall -60 | bottom -1000 | side 2500 | top 1000 | item -1000 |
|---|---|---|---|---|---|
| modtools (x87 `FCOMP D8 1D`) | `0x005465E0` | `0x005465F7` | `0x0054663C` (x), `0x00546662` (z) | `0x00546679` | `0x005192B5` |
| Steam/GOG (SSE) | `MOVSS 0x004E9672` | `MOVSS 0x004E968F` | `MOVSS xmm4 0x004E96B0` (x and z) | `COMISS 0x004E96D8` | `MOVSS 0x004CB340` |

GOG code VAs equal Steam (`port_gog.py code`, score 1.00); only the constants moved.
Every constant is a shared `.rdata` pool float (1000.0 has 33 other readers), so
the patch repoints each `[disp32]` at a GameExt float instead of editing it. The
install guard checks the ModRM is `[disp32]` and that the operand still reads the
stock value. The INI patch `SoldierHeightCeiling` jumps over the top test instead,
so the two coexist and `top` is moot while it is on.

## Other hardcoded limits

All host only except the bullet box. `SetWorldKillBounds` moves each to
`min(stock, bottom)`, and the box to `max(5000, horizontal)` on x/z and
`[min(-5000, bottom), max(5000, top)]` on y, so none gets tighter than stock.

| Limit | Function (Steam / modtools) | Steam/GOG | modtools |
|---|---|---|---|
| flyer y < -5000 | `EntityFlyer::Update` `0x004AC460` / `0x004FC930` | MOVSS `0x004AC770` | FCOMP `0x004FCCC7` |
| droideka y < -500 (or sink height) | `EntityDroideka::Update` `0x004A3F90` / `0x004EE5A0` | `0x004A47F9` | `0x004EEF33` |
| hover y < -500 | `EntityHover::Update` `0x004C2230` / `0x00512D60` | `0x004C245E` | `0x00512FE1` |
| walker y < -500 | `EntityWalker::Update` `0x005043B0` / `0x00557AF0` | `0x005048C5` | `0x00558077` |
| mine y < -500 | `EntityMine::Update` `0x004CF890` / `0x0051F340` | `0x004D0081` | `0x0051FA46` |
| ordnance y < -2000 | `Ordnance::Update` `0x005F7BD0` / `0x00604660` | `0x005F7C53` (GOG `0x005F8CF3`) | `0x00604734` |
| droid y < -500 (sink height delays) | `EntityDroid::Update` `0x0049AC60` / `0x004DEB80` | `0x0049ACD4` | `0x004DEBEE` |
| defense grid turret y < -500 | `EntityDefenseGridTurret::Update` `0x00497B40` / `0x004D85C0` | `0x00497BB5` | `0x004D863C` |
| portable turret y < -500 (two tests) | `EntityPortableTurret::Update` `0x004D8230` / `0x00520F60` | `0x004D82E1`, `0x004D84FE` | `0x00521009`, `0x005212A1` |
| powerup y < -500 | `PowerupItem::Update` `0x00623A60` / `0x00659F10` | `0x00623A98` (GOG `0x00624B28`) | `0x00659F44` |

The stock detpack ordnance is `ClassLabel = "mine"`, so detpacks are `EntityMine`.
`Ordnance::Update` ends in `return mPosition.y >= -2000`, which every ordnance type
reaches through its base class.

`OrdnanceBullet::Update` (Steam `0x005FAEE0`, modtools `0x0060A690`) ends with a
function-local `static const PblBox sWorldBox(-5000, -5000, -5000, 5000, 5000, 5000)`
and returns false (no collision, no effect, no damage) when the render position is
outside it. Laser, shell, grenade, missile, sticky in flight, tow cable, grappling hook
and fat ray all run it. The box is read from memory after the guarded init, so GameExt
writes it in place and sets the guard bit:

| | box | guard | bit |
|---|---|---|---|
| Steam | `0x01FAAB98` | `0x01FAABB0` | 1 |
| GOG | `0x01FAC048` | `0x01FAC060` | 1 |
| modtools | `0x00B919F4` | `0x00B91A14` | 2 (bit 1 is a profiler scope) |

Steam's first-call path takes max.y and max.z from the shared 5000 constant rather
than the box, another reason not to repoint operands there. Install checks that the
box is still zero (or already stock) and reads the bit from the code after the guard
load (`test al, imm8` on Steam/GOG, `mov eax, imm32` on modtools).

## What `SetWorldExtents` really does

`WORLD_EXTENTS` (default `0x400`) is reset to 1024 right before `ScriptPreInit` in
`GameState` (Steam `0x0053AF10`), then read once, later in the same function, by
`CollisionManager::Create`, `PathFinder::Initialize` and `HintManager::Initialize`
(via `AIUtil::Init`). It sizes three `TreeGrid`s and nothing else, so it only
works from `ScriptPreInit`; from `ScriptInit` it is already too late.

An object outside a `TreeGrid` lands in cell 0, which every query visits. So a map
bigger than its extents still collides, it just gets slower. The collision grid is
always about 64 cells across. The path and hint grids use a fixed 16 m minimum
cell, so their cell count grows with the square of the extents: 64x64 at 1024,
1024x1024 (about 5.6 MB of pointers each) at 10000, 2048x2048 (about 22 MB each)
at 20000.

## Other limits a huge map runs into

- **MP position quantisation.** `PositionXZConvert` is +/-10240, `PositionYConvert`
  is +/-1024 (21 bits each, full float only at `netLOD == 1`). Clients see positions
  outside that clamped. Single player is unaffected. Not patched.
- **Fall-speed kill.** Measured on Steam (stock rifleman class): a free-falling soldier
  levels off at about 39 m/s (19.8 m per 0.51 s) after ~4 s, so the stock 60 m/s is
  never reached by an ordinary fall. It fires on the fly-ceiling push below or on
  external pushes. Landing damage (`CollisionThreshold`/`CollisionScale`, quadratic in
  the excess speed, in the soldier collision callback) killed at ~39 m/s and spared
  ~30 m/s, so any drop long enough to hit terminal speed is fatal regardless of
  `fallSpeed`. Source of the terminal speed not traced.

## Test 2026-11-07 (modtools, VTRc_con)

Teleport via `SetEntityMatrix(GetCharacterUnit(p), CreateMatrix(0, 0,1,0, x,y,z))`.
Stock (3000, 50, 0) logged `Unit flew out of world` and died; with h = 10000 the same
spot did not. Stock (0, 1500, 0) logged `Unit flew over world`; with top = 6000 no
message, the soldier died later (landing). `GetWorldKillBounds` at load returned
`2500 -1000 1000 60`. Restart reset confirmed.

Steam, same map, position logged every 0.5 s: bottom = 900 killed at once; (0, 50, 3000)
with h = 10000 fell, stood at y = 1 for 7 s, alive (z works; x and z share one operand
there). Fall from 50 m: ~30 m/s at landing after ~2.5 s, survived. Teleports to y = 300
and 1500 with fallSpeed 30 / 60 / 1000 died 1.05 / 1.08 / 1.18 s after the teleport,
just after FALL starts: the fly-ceiling push above, the order matching the fall-speed
threshold. Rerun with `SetMaxPlayerFlyHeight(5000)`: fallSpeed 30 died 2.15 s after the
teleport at ~30 m/s; fallSpeed 60 and 1000 fell at the ~39 m/s terminal speed and died
on landing (9.2 s from 300 m, 40 s from 1500 m). GOG not tested (same code VAs as Steam).
- **Soldier fly ceiling.** In the FALL/FLY branch of `EntitySoldier::Update` (entered
  after 1 s ungrounded) `fCeiling = AIUtil::gMaxPlayerFlyHeight - y`; below 5 it adds
  `(5 - fCeiling) * -10 * dt` to `mVelocity.y` every frame. `gMaxPlayerFlyHeight` is
  200 by default (`AIUtil` init), set by stock `SetMaxPlayerFlyHeight`. A soldier more
  than 200 high is slammed down within a few frames of entering FALL, which trips the
  fall-speed kill (or the landing damage, if `fallSpeed` is huge). Tall maps need
  `SetMaxPlayerFlyHeight` raised along with `SetWorldKillBounds`.
- Map-placed boundary regions (`BoundaryManager`, the countdown and VO) are authored
  per map and unrelated. `Character::CheckOutOfBounds` (Steam `0x00453190`) kills the
  soldier, vehicle and remote 6.5 s after leaving (2.17 s for a guided missile flyer),
  and a player flyer outside the fly heights counts as out of bounds too.
- **Terrain is clamped at its edge.** `RedTerrain::GetInterpolatedHeight` clamps x/z to
  the terrain, so outside it the ground continues at the edge height forever. A
  disabled terrain reads -1000 from `GetHeight`.
