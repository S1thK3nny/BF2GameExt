# AI hint nodes

Read 2026-10-14 in Ghidra (Phantom with PDB = **P**, modtools `BF2_modtools_MemExt.exe` = **M**) and
checked against the stock `.hnt` files under `C:\BF2_ModTools`. Read-only research; nothing is patched.
Steam and GOG addresses were not looked up.

See also `AISystem.md` ("BaseHint stances - the hint node prone bit the engine drops") and
`FlyerAI.md` (the fly agent's states).

## Short answer: why land hints do not work

`BaseHint::GetCommandPost` always returns NULL, and `RequestLandHints` throws away every land hint whose
command post is NULL. No land hint can ever survive, so no AI flyer ever lands at one. The command post
link was cut from the hint entirely: the setter is an empty stub too, the loader looks the post up and
throws the result away, and `BaseHint` has no field to hold it. Details and evidence below.

## Objects and storage

- `BaseHint` is 64 bytes (P struct `BaseHint`): a `TreeGridObject` header (+0x00..+0x23, the position is
  inline at +0x08 or in the grid node), a `ReserveObject mReserve` (+0x24), `PblVector3 mForward` (+0x30)
  and one ushort bitfield at +0x3C (type in bits 0-7, primary stance 8-9, secondary stance 10-11,
  sidestep 12-13; bits 14-15 unused, BF2GameExt's prone fix uses them). +0x3E..+0x3F is padding.
  **There is no command post field, no name, no radius, no mode and no team.**
- Subclasses `SnipeHint`, `PatrolHint`, `CoverHint`, `JetJumpHint`, `MinefieldHint`, `LandHint`,
  `FortificationHint`, `VehicleCoverHint` add nothing (all 64 bytes); each constructor only passes its
  type to `BaseHint::BaseHint` (P `0x005EBB00`, M `0x005C5C30`).
- All hints live in one `TreeGrid` (`HintManager::sHints`, made by `HintManager::Initialize`, P
  `0x005EC1A0`: world extents, 16 m cells; M global `0x00B8F2B4`).
- They come from `BaseHint::sMemoryPool` ("BaseHint", element 0x40; P pool `0x00B26C10`, M `0x00B8F2B8`).
  `AIUtil::Init` (P `0x004889B0`) sets it to 350; maps raise it in Lua with
  `SetMemoryPoolSize("BaseHint", n)` (stock scripts use 100 to 1000; Yavin, with 973 cover hints, uses
  1000). The pool's grow step is 0 (`MemoryPool::MemoryPool`, P `0x008B1710`); what happens to hints past
  the count was not followed through (`MemoryPool::Allocate`, P thunk `0x004188B8`, logs
  `Memory pool "%s" is full` and tries to grow by 0).

## Hint types

The type is a bitmask (P enum `BaseHint::Type`): `HINT_SNIPE` 1, `HINT_PATROL` 2, `HINT_COVER` 4,
`HINT_JETJUMP` 8, `HINT_MINE` 0x10, `HINT_LAND` 0x20, `HINT_FORTIFICATION` 0x40, `HINT_VEHICLECOVER` 0x80
(also `HINT_UNDEFINED` 0, `HINT_TERMINATE` 0x100). ZeroEditor numbers them 0-8 instead
(`data\Editor\Config\editor\HintNode\gzeditor_hintnode_types.cfg`) and writes the number as a string,
`Hint("name", "6")`; the loader matches the PblHash of that string.

Stock counts are from `C:\BF2_ModTools\assets` plus `data_BF3` (BF1 files left out).

| .hnt | Editor name | Type bit | Who asks (P) | Radius, max | What the unit does | Filters | Stock count |
|---|---|---|---|---|---|---|---|
| "0" | Snipe | 0x01 | `SnipeHelper::FindSnipeHint` `0x0074C5B0` via `RequestSnipeHints` `0x005EC6C0` (M `0x005C4B50`) | 100 m, 20 | goes there and snipes in the primary stance | nearest first, at most 11 tried; no valid enemy within 12 m of the line to it; reachable (`CanGetThereFromHere_Quick...`); `SnipeHelper::IsHintSuitable` | 480 |
| "1" | Patrol | 0x02 | `PatrolHelper::EnterState` `0x00712850` via `RequestPatrolHints` `0x005EC690` (M `0x005C4BB0`) | 300 m round the post, 10 | would walk to it and stand in its stance | must belong to the post being patrolled: **never true**, so the soldier always wanders 8-14 m round the post instead | 0 |
| "2" | Cover | 0x04 | `CoverHelper::FindHint` `0x004F3F90`; `Navigator::LookForCoverNodes` `0x00679810`; `DefendHelper::CheckEnterFortification` `0x004FA4A0` (fallback) via `RequestCoverHints` `0x005EC310` (M `0x005C4B80`) | CoverHelper: weapon range capped at 40 m, 30. Navigator: 5 m, 10. Defend: 50 m, 30 | takes cover, stands/crouches by the stances, sidesteps out by the sidestep bits | CoverHelper scores each (`IsHintSuitable_Combat/_ForwardCover/_Idle`); Navigator takes the nearest one ahead of a soldier on a path (hint facing within 60 degrees of the path, clear line) and detours through it, not while carrying a flag | 6407 |
| "3" | Access | - | nobody | - | - | the loader has no case for it: the hint is dropped at load | 0 |
| "4" | JetJump | 0x08 | `JetJumpHintHelper::FindHint` `0x00621DB0` via `RequestJetJumpHints` `0x005EC4E0` (M `0x005C4BE0`) | 40 m, 30 | jet-jumps from it | `SelectHint` | 59 |
| "5" | Mine | 0x10 | `MineHelper::HELPERIsMinePosNearby` `0x0066EE70` via `RequestMinefieldHints` `0x005EC5F0` (M `0x005C4C10`) | 100 m, 3 | lays mines there (`FindMinePos`) | not reserved (10 s); no enemy mines in it (`sContainsEnemyMines`). The command post test keeps a hint whose post is NULL, i.e. **every** hint: the post and team filter is never applied | 357 (223 name a post) |
| "6" | Land | 0x20 | `UnitFlyAgent::IsNearLandingPt` `0x00790AC0` via `RequestLandHints` `0x005EC510` (M `0x005C4F10`) | 80 m, 1 | AI flyer lands, pilot gets out at that post (below) | needs a post: **never true**, so never used | 9 (2 stock, 7 in data_BF3) |
| "7" | Fortification | 0x40 | `DefendHelper::CheckEnterFortification` `0x004FA4A0`, `DefendHelper::EnterState` `0x004FA900` via `RequestFortificationHints` `0x005EC340` (M `0x005C4CC0`) | 50 m, 30 | a defending soldier who has stayed within 25 m of one spot for over 10 s goes to one (cover hints if none) | `ChooseHint` | 311 |
| "8" | VehicleCover | 0x80 | `VehicleCoverHelper::FindHint` `0x0079BEC0` via `RequestVehicleCoverHints` `0x005EC6F0` (M `0x005C4CF0`) | weapon range capped at 100 m, 30 | vehicle takes cover there, with stances and sidestep like cover | `SelectHint` | 11 |

`HintManager::_NFINAL_DrawHintNodes` also asks for all types (mask 0xFF) for the debug draw (see below).
There are no other callers of `RequestHintsCommon` (P `0x005EC370`, M `0x005C4910`; thunk P `0x00416C43`,
M `0x00406217`).

### Filters every request shares

`HintManager::RequestHintsCommon(typeMask, UnitController*, pos, radius, max, out)`:

- If the asking unit carries **any MELEE weapon** it gets no hints at all (Jedi and other melee heroes).
- If its class sets the ODF key `IgnoreHintNodes` (`ControllableClass::SetProperty`, P `0x004EF090`, hash
  `0x848819E7`; `mIgnoreHintNodes`, ControllableClass +1 on M) it gets none.
- The search is an axis-aligned **cube** of half-size `radius` (int, metres), not a sphere.
- `GenerateHintListCallback` (P `0x005EBFD0`, M `0x005C48B0`) keeps a hint whose type is in the mask and
  that is not reserved (`AIUtil::IsHintAvailable`, P `0x00488B30`), and stops the grid walk once `max`
  are found.

No request filters by team; the only team test anywhere is the command post one in `RequestLandHints` and
`RequestMinefieldHints`, which never runs (below).

### Reservation

`BaseHint::Reserve` (P `0x005EC720`) stamps the time and the reserver. `BaseHint::IsReserved` (P
`0x005EC240`) is true for 2 s after the stamp, 10 s for mine hints (`GetReserveTime`, P `0x005EC110`),
for **every** unit including the reserver: the team argument and the reserver are not looked at
(`ReserveObject::IsReserved`, P `0x0073A230`). Users re-reserve while they hold a hint (the fly agent's
LAND state every update; the patrol, cover and navigator code on entering). `SetReserveDelay` (P
`0x0073A2F0`) pushes the stamp into the future: the fly agent blocks a land hint for 20 s (+2) after a
drop-off.

## Loading: what the .hnt fields become

ZeroEditor writes (`Hint("%s", "%d")`, `PrimaryStance(%i)`, `SecondaryStance(%i)`, `CommandPost("%s")`):

```
Hint("HintNode16", "6")
{
	Position(19.307739, 82.428764, -16.035088);
	Rotation(0.920467, 0.000000, 0.390819, 0.000000);
	PrimaryStance(3);
	SecondaryStance(1);
	Mode(3);
	CommandPost("CP4");
}
```

(`assets\worlds\MUS\world1\mus1_DESIGN.HNT`, one of the two stock land hints.) The munged `.world`
carries a `Hint` chunk per hint: `INFO` (`TYPE`, `NAME`, `XFRM`) then one `PROP` per property, as
(PblHash, string); checked in `data_BF3\_BUILD\Worlds\DESOLATION\MUNGED\PC\desolation_hints.world`, which
has `PROP`s for Mode, Radius and CommandPost.

`LoadUtil::ProcessHint` (P `0x00638310`, M `0x00451F00`) hands `INFO` to `LoadUtil::ProcessHintInfo`
(P `0x006383E0`, M `0x00451AE0`), passes each `PROP` to `BaseHint::SetProperty` (P `0x005EC750`, M
`0x005C45B0`), then calls `HintManager::AddHint` (P `0x005EBD00`, M `0x005C5DF0`).

| Field | Read? | Kept? | Notes |
|---|---|---|---|
| `TYPE` | yes | as the type bit | strings "0"-"8" hashed; "3" (Access) and anything else returns NULL and the hint is skipped |
| `NAME` | yes, hashed into `HintDesc::mNameId` | **no** | the constructor ignores it; hints have no name at run time |
| `XFRM` | yes | position and forward | rebuilt later by `BaseHint::BuildMatrix` (P thunk `0x00416BA3`) |
| `PrimaryStance` (`0xBF0F1CD1`) | yes | 2 bits | prone (4) dropped; see `AISystem.md` |
| `SecondaryStance` (`0x92C05A59`) | yes | 2 bits + sidestep | value bits 3-4 (8 = left, 16 = right) become the sidestep mask (`HasSidestep` P `0x005EC190`, `GetRandomSidestep` P `0x005EC080`, used by cover and vehicle cover) |
| `CommandPost` (`0x844CCDB4`) | yes | **no** | `CommandPostManager::FindPost(name)` is called, a missing post logs `Hint references nonexistant command post "%s"`, a type other than patrol/snipe/mine/land logs `... but is not patrol/snipe/mine/land type`, and the post is then **dropped**: nothing is stored |
| `Mode` (`0xEC6EE012`) | passed to `SetProperty` | **no** | no case; ignored. On every stock hint (0-3) |
| `Radius` (`0x0DBA4CB3`) | passed to `SetProperty` | **no** | no case; ignored. On 878 stock hints |

## The land hint path, step by step

1. **Load.** `CommandPost("CP4")` reaches `SetProperty`, which looks the post up and discards it (P
   `0x005EC87A`-`0x005EC93C`: `FindPost` result kept in a stack slot, used only for the warning).
   `BaseHint::SetCommandPost` (P `0x005EC740`) exists but is `RET 4` and has no callers; M `0x005C44C0`
   is the same lone `RET 4` (inferred to be it by position and shape).
2. **AddHint.** For a land hint whose `GetCommandPost()` is NULL (all of them), `AddHint` walks every
   command post and works out its distance to the hint, then throws the result away (P `0x005EBD00`;
   M `0x005C5DF0` keeps the nearest distance and never uses it). This reads like the remains of "link a
   land hint without a post to the nearest post" (inferred).
3. **Who asks.** Only `UnitFlyAgent`, only in its FLY state (4): `UpdateStatePostVision` (P `0x00790D20`,
   M `0x005AFCF0`) calls `IsNearLandingPt` first in that state. Other states never ask. For BF2GameExt's
   flyer wingmen the answer is always no (`flyer_wingmen.cpp` redirects the FLY state's call, M
   `0x005AFDD3`).
4. **IsNearLandingPt** (P `0x00790AC0`, M `0x005AED00`) runs every 3 s (`mNextHintCheckTime`) and calls
   `RequestLandHints(ctrl, flyerPos, 80, 1, out, mGotoNonOwnedCP)`: an 80 m cube, **at most one** hint.
5. **RequestLandHints** (P `0x005EC510`, M `0x005C4F10`) gets that one hint from `RequestHintsCommon(0x20,
   ...)`, then drops it unless all of these hold:
   - `BaseHint::GetCommandPost(hint)` is not NULL. **It is always NULL:** P `0x005EC030` is
     `XOR EAX,EAX; RET`, M `0x005C44B0` is `33 C0 C3` (thunk P `0x00418BDD`, M `0x00402347`). This is the
     failure. Everything after it is dead code in stock.
   - `CommandPost::CanTeamSpawnHere(post, team)` is false when the last argument is true (a post the
     team does not hold) or true when it is false. Team = the flyer entity's +0x234, low 4 bits signed.
   - The hint is not above the flyer (`hint.y > flyer.y` drops it).
6. **IsNearLandingPt** would then reserve the hint and return true; FLY goes to **LAND (5)**.
7. **LAND EnterState** (P `0x0078FD90`, jump table P `0x007902AC`, case P `0x0079008D`; M
   `0x005AF000`) reserves the hint, builds its matrix, gets a begin-landing point from the flyer class's
   landing numbers (`VehicleSpawn::gCalcBeginLandPos`, P thunk `0x00408D23`: `LandingTime`,
   `LandingSpeed`, `LandedHeight`, `MinSpeed`, `GetLandingForwardBias`, as for a vehicle spawn), sets a
   three-point path (an approach point back along the hint's facing, the begin-landing point, and the
   flyer) with `SetNavigator_FollowPath`, and sets bit 0x40 at controller +0x338 (the land request,
   inferred). The hint's **rotation sets the approach direction**.
8. **LAND update** re-reserves the hint each update; when the flyer's state word (+0x5A0) is 0 (landed,
   inferred) it goes to **DROPOFF (6)**.
9. **DROPOFF EnterState** (case P `0x007901F6`):
   - With `mGotoNonOwnedCP` false it only sets it true and goes to state 3 (wait, then take off).
   - With it true it calls `AIUtil::DeployCargo(flyer, post)` (P `0x00486930`: every AI passenger that is
     not the pilot gets out; the post is unused) and, for a transport (`IsTransport`, VehicleType 5),
     clears the flag and goes to state 3. For any other flyer it blocks the hint for 20 s, gets the pilot
     out with `Controllable::SetExitVehicle(post)` (the post again from `GetCommandPost`, so NULL) and
     blocks vehicles for 15 s.
   - `mGotoNonOwnedCP` starts true (state 1 sets it), so fighters look for land hints at posts their
     team cannot spawn at, land, and the pilot gets out (to capture it, inferred). Transports alternate:
     drop troops at a post they do not hold, then look for one they do. (The FLY state's random post
     pick, case P `0x0078FF4D`, tries up to 10 posts; for a transport it re-picks a post of the wrong
     kind only while that post is within 100 m.)

## Why land hints do not work

1. **The cause: the command post link is stubbed out.** `GetCommandPost` returns 0 (P `0x005EC030`,
   M `0x005C44B0`), `SetCommandPost` is empty (P `0x005EC740`), `SetProperty` looks the post up and drops
   it, and `BaseHint` has no field for it. `RequestLandHints` drops every hint with no post (P
   `0x005EC55F`: `TEST EAX,EAX; JZ` to the drop), so it always returns 0 and `IsNearLandingPt` is always
   false. This holds on Phantom and modtools; the same stub on Steam and GOG is likely (inferred, not
   checked). Linking the hint to a post in ZeroEditor cannot help.
2. **Same cause, other victims.** Patrol hints never work (`RequestCPHintsCommon`, P `0x005EC270`, M
   `0x005C4AC0`, keeps only hints whose post equals the patrolled post), and mine hints ignore their post
   and team (the NULL post is the "keep" case in `RequestMinefieldHints`).
3. **Smaller limits that would still bite after a fix:**
   - Only flyers in the fly agent's FLY state ask, only every 3 s, only within an 80 m cube.
   - `max` is 1: the grid walk stops at the first unreserved land hint, and if that one fails the post,
     team or height test the answer is "none", even if another land hint in range would pass.
   - The hint must not be above the flyer.
   - A fighter only lands at posts its team cannot spawn at (enemy or neutral), never at its own.
   - Melee-armed flyers and classes with `IgnoreHintNodes` never get hints.
4. **Little content.** Stock BF2 ships two land hints (Mustafar's design layer); BF1's shipped worlds in
   the mod tools have 33 (Bespin, Geonosis, Kamino, Naboo), which suggests the feature came over from
   BF1 and lost its post link in BF2 (inferred).

## What a fix could look like (not done)

- **Give hints their post back.** BF2GameExt already hooks `BaseHint::SetProperty` (M `0x005C45B0`) for
  the prone bit. For hash `0x844CCDB4` it could keep the post (pointer, or the name's hash to look up
  again) in a DLL table keyed by the `BaseHint*`, cleared when the level unloads. Then replace
  `GetCommandPost`: either point the thunk's `JMP` at M `0x00402347` (its only caller route; no direct
  calls to M `0x005C44B0`) at a DLL `__thiscall` function, or write a 5-byte `JMP` over M `0x005C44B0`
  (3-byte body followed by 13 bytes of `CC` padding). The stub touches only EAX, so a caller may keep
  ECX or EDX live across it: the stand-in must preserve every register but EAX (`verify-hook-contracts`;
  the five callers' listings were not checked for this).
- **Scope it to land hints** if stock behaviour must not change: returning a post for mine hints would
  make 223 stock mine hints usable only by teams that can spawn at their post, and patrol hints would
  come alive (none in stock BF2).
- **Hints with no post:** the dead loop in `AddHint` suggests linking a land hint without a post to the
  nearest post (inferred intent).
- **Optional:** raise `max` in `IsNearLandingPt` (M `0x005AED44`: `PUSH 0x1`, needs a bigger out array;
  the caller's array is one slot on the stack) so one failing hint does not hide the rest.
- Retail patches must move every absolute address with the exe (`retail-exes-relocate`).

## Debug draw

Console `ai.showHintNodes` (`AiShowHintNodes`) -> `AIUtil::OutputDebugInfo` (call at P `0x004F2492`) ->
`HintManager::_NFINAL_DrawHintNodes` (P `0x005ECA10`; M `0x005C5020`, found by its `PUSH 0xFF` / 50 /
150 call, inferred to be it). It draws every hint within a 50 m cube of the camera (at most 150): a
coloured disc per type (snipe green, patrol cyan, cover red, jetjump light grey, mine magenta, land mid
grey, fortification orange, vehicle cover green-yellow), an arrow for the facing (not for mines), and
within 10 m a label with the type; cover and fortification also show `P:` and `S:` stances (2-bit:
`St`, `Cr`, `St Cr`) and `Rt` for a right sidestep. Nothing shows the (missing) command post.

## Smaller findings

- `SnipeHelper::IsHintSuitable` (P `0x0074CC10`) counts enemies near a point 70 m (`gSnipeSuitabilityDist`)
  out along the hint's facing and then returns true whatever it found; only the end of a game makes it
  false. The facing of a snipe hint therefore does not matter.
- In `RequestHintsCommon` the compiler reuses the "me" variable as the melee loop's counter; it is set to
  the asking unit's entity afterwards, so the reservation check does apply (P `0x005EC370`, M
  `0x005C4910`).
- No Lua function or console command that reads or changes hints was found (only the debug draw); not
  searched exhaustively.
