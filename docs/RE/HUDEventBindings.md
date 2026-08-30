# HUD event bindings and ElementBar MinValue/MaxValue

How `player1.healthFraction` is registered, hashed and resolved, what number it produces, and
what an ElementBar does with its `MinValue`/`MaxValue` pair - including the inverted and
degenerate cases.

Investigated 2026-08-22 across all four builds. Section 4 marks what was read out of each
build's own instructions versus what is still open.

## 1. `player1.healthFraction`, end to end

### 1.1 Nothing parses `player1.`

The dot is not a separator and there is no prefix dispatcher. The whole string is one atomic key, built once at level load and hashed.

`HUD::GameEvents::Open` (Phantom `0x00611F90`, modtools `0x006AEF00`, Steam `0x0055E3A0`, GOG `0x0055F120`) makes ~171–173 static `EventClass::Create(Type, fmt, ...)` calls. The `healthFraction` one, raw bytes:

```
; Phantom Battlefront2.exe
00612105  6a 01          PUSH 0x1                ; vararg -- LITERAL 1, not a loop counter
00612107  68 0cfb9f00    PUSH 0x9ffb0c           ; "player%d.healthFraction"
0061210c  6a 04          PUSH 0x4                ; Type = type_Float
00612113  e8 8998dfff    CALL 0x0040b9a1         ; HUD::EventClass::Create (thunk)
0061211b  a3 b46fb200    MOV  [0x00b26fb4],EAX   ; gPlayerEvents.healthFraction (EventClass*)
```
modtools: `006af07a 6a 01` / `68 d0fea500` / `6a 04` / `CALL 0x0040a394` / `MOV [0x00ba3b74],EAX`.
Steam: `0055e518 6a 01` / `68 fc1e7a00` / `6a 04` / `CALL 0x0055de40` / `MOV [0x01e56c4c],EAX`.
GOG: `0055f298 6a 01` / `68 6c2d7a00` / `6a 04` / `CALL 0x0055ebc0` / `MOV [0x01e580fc],EAX`.

`EventClass::Create` `vsprintf`s the name, then `PblHash`es the *finished* string. So the object that exists at runtime is a single `EventClass` named `player1.healthFraction`, on a global linked list keyed by hash. There is no "player" namespace, no per-player table walk, no field lookup of `healthFraction` inside a `player1` object.

`Type = 4 = type_Float` — VERIFIED against the PDB enum `HUD/EventClass/Type` (`0 Invalid, 1 Bool, 2 Int, 3 Uint, 4 Float, 5 Model, 6 Texture, 7 Color, 8 String, 9 Vector3`). Sibling `player1.health` is `type_Uint(3)`, `player1.healthDisable` is `type_Bool(1)`.

The `%d` is always `1`. Verified three ways: the literal `PUSH 0x1`; there is no backward branch enclosing the block (Phantom `Open` has exactly three backward branches — weapons `00612420→00612619`, teams `00612820→006128f6`, statistics `00612900→0061293f`); and `gPlayerEvents` is a one-element array — `gObjectiveEvents` sits at Phantom `0x00B272CC`, immediately after `0x00B26FA8 + 0x31C`. PC has no splitscreen. **There is no `player2.healthFraction`.**

### 1.2 The one thing that *does* touch the prefix: `EventNameFilter`

`HUD::Item::GetFilteredEventName` (Phantom `0x00617540`, modtools `0x006B6270`, Steam `0x00564690`) substitutes the current event index into the digit run at the **first point of divergence** from the viewport's filter string. `HUD::ViewPort::ReadData` parses key `EventNameFilter` (hash `0x4EE68DBE`), and `PostReadSetup` resets it via `SetEventFilter(NULL, 0)`.

Two consequences worth knowing:
- Writing `player.healthFraction` (no digit at all) still resolves to `player1.healthFraction` under filter `player%d`.
- On PC the substitution is a no-op regardless, because `Open` registers the literal `player1.*` names and there is only one local player.

Asymmetry the notes miss: the **load-time** path filters, the **runtime** path does not. `Item::ReadData → ReadEvent` calls `GetFilteredEventName` then hashes; `HUD::Item::SetEvent` (modtools `0x006B6500`) — the `SetProperty("EventValue", …)` path — hashes the raw string with no filtering.

### 1.3 Resolution

```
Item::ReadEvent  (Phantom 00617E30 / modtools 006B6360 / Steam 00564740)
  -> GetFilteredEventName(name)
  -> PblHash::PblHash(filtered)        ; FNV-1a, basis 0x811C9DC5, prime 0x01000193, each byte |= 0x20
  -> EventClass::FindByHashID(hash)    ; Phantom 0060F4D0 / modtools 006AD940 / Steam 0055DEE0
  -> EventClass::RegisterEventHandler(ec, handler)
```

`hash("player1.healthFraction") = 0x1440D2CF` — COMPUTED by me, not read out of the binary; my hash implementation reproduces both known-good controls (`Effect` → `0x6E6E8D54`, `ParticleEmitter` → `0xCEBB215F`).

**A typo is silent on retail.** Steam `ReadEvent` (`0x00564740`, 28 instructions, zero string refs) falls straight through on `FindByHashID == NULL` and returns `true`:
```
0056477b  JZ  0x00564785      ; not found -> skip registration
00564780  CALL 0x0055dd50     ; RegisterEventHandler
00564785  MOV  AL,0x1         ; always true
```
The `RedWarning::LogMessage("HUD Element unable to find event %s")` exists **only** in Phantom and modtools (`HUDItem.cpp:0x304`). Debug your bindings in modtools, not in retail.

### 1.4 The value

`HUD::GameEvents::UpdateHealth` — Phantom `0x00614400`, modtools `0x006B4500`, Steam `0x005619F0`, GOG `0x00562770`. Steam disassembly, which is the same math in all four:

```
00561af1  MOVSS  XMM4,[EDI + 0x74]      ; playerData->health (previous value)
00561af6  MOVSS  XMM0,[EBP + -0x4]      ; max(mCurHealth, 0.0f)
00561afb  UCOMISS XMM0,XMM4 ... JNP     ; EDGE-TRIGGERED: unchanged -> send nothing
00561b0e  MOVSS  XMM3,[EAX + 0x148]     ; Damageable.mMaxHealth
00561b16  UCOMISS XMM3,XMM1 ... JNP     ; maxHealth == 0 -> fraction forced 0, no divide
00561b22  DIVSS  XMM2,XMM3              ; fraction = health / maxHealth   <-- NO UPPER CLAMP
00561b26  MOVSS  XMM3,[0x007b2104]      ; 1.0f
00561b2e  COMISS XMM2,XMM3 / JBE        ; frac <= 1 -> bonus = 0
00561b38  TEST   BL,BL / JNZ            ; isHero -> bonus = 0
00561b3f  SUBSS  XMM1,XMM3              ; bonus = frac - 1.0
00561b43  COMISS XMM3,XMM1 / JA / MOVSS ; bonus clamped to 1.0
00561ba4  MOV    EBX,[ESI + 0xc]        ; gPlayerEvents.healthFraction
00561bba  MOVSS  XMM2,[EBP + -0xc] / PUSH EBX / CALL 0x0055e000   ; Event(EventClass*, float)
```

So:

- **Numerator** `max(0.0f, GameObject+0x144)` = `Damageable.mCurHealth`, lower-bounded at 0. **Denominator** `GameObject+0x148` = `mMaxHealth`. Both offsets identical on all four builds; the `Damageable` subobject is at `GameObject+0x140`.
- **`mMaxHealth == 0` → exactly `0.0f`.** Guarded, no divide, no NaN from that route.
- **No upper clamp.** The `1.0f` compare only feeds `bonusHealthFraction`.
- **The unit is always the soldier.** `Update` passes `Character.mUnit->GetTrackable()`. Vehicle health goes through `UpdateVehicleHealth` (Phantom `0x00615930`) to `player1.vehicle.health*`.
- **Edge-triggered.** No change in `mCurHealth` → no event sent at all.

**Range: `[0, +inf)`, in practice `[0, 1.25]`.** Above-1 is a shipped path, not a theoretical one. `Character::Spawn` (Phantom `0x0049F220`, modtools `0x006457A0`, Steam `0x00451B90`, GOG `0x00451B70`) applies the **Combat Shielding** team bonus (`IsTeamBonusActive(team, 0xDDB212E1)`) as `SetCurHealth(mMaxHealth * 0.25f + mCurHealth)` with **no `min()` against max** — `sTeamBonusCombatShieldingPercentage` = `0.25` (Phantom `0x009E2F0C`, modtools `0x00A55874`). `Damageable::SetCurHealth` (Phantom `0x004F7560`) has no upper clamp. Result: `healthFraction = 1.25`, `bonusHealthFraction = 0.25`, on every spawn while that bonus is up. `GameObject::SetProperty` key `CurHealth` (`0xF2BF9D5F`) and `MaxHealth` (`0x19971F1B`) are likewise unclamped, so scripts and ODFs can push it anywhere. Every *other* gain path I read does clamp (`PowerupUtility::DoHealth`, `UpdateTeamBonusBactaTanks`, `EntityDroideka::UpdateBuffTimers`, `EntityRemoteTerminal::UpdateHealthRemote`).

**NaN passes through.** A NaN `mCurHealth` survives the `max(x,0)` floor in all four builds (Phantom `MAXSS` returns src on unordered; modtools `TEST AH,0x5 / JP`; retail `COMISS / JBE`).

### 1.5 When the binding goes quiet

`player1.healthFraction` **stops updating and holds its last value** whenever the value is routed elsewhere or the whole update is gated:

| condition | what happens |
|---|---|
| `Character.mVehicle != 0` | value goes to `player1.healthInVehicleFraction` (modtools string `0x00A5FE18`) |
| hero bit set (`EntityClass+0x36C` Phantom/modtools, **`+0x328` Steam/GOG**) | value goes to `player1.hero.healthFraction` (modtools string `0x00A5FDD8`) |
| `ScopeDisplay::sInstance[0]->mInBinocularMode` (object `+0x4C9`) | `UpdateHealth`/`UpdateJetpack`/`UpdateVehicleHealth`/`UpdateEnergy` are all skipped in `GameEvents::Update` |
| `PlayerData+0xd8 != 0` (shipping builds only; absent in Phantom) | health forced to 0, `healthDisable` / `hero.healthDisable` sent instead |

The full registered health family on modtools, read from that build's string table:

```
00a5ff08  player%d.health                       00a5fe18  player%d.healthInVehicleFraction
00a5feec  player%d.healthDisable                00a5fe40  player%d.healthInVehicleDisable
00a5fed0  player%d.healthFraction               00a5fe68  player%d.healthInVehicle
00a5feac  player%d.bonusHealthFraction          00a5fd98  player%d.vehicle.health
00a5fe88  player%d.healthRegenPulseRate         00a5fd70  player%d.vehicle.healthFraction
00a5fdfc  player%d.hero.health                  00a5fd48  player%d.vehicle.healthDisable
00a5fdd8  player%d.hero.healthFraction          00a5eb5c  player%d.lockOnHealth
00a5fdb4  player%d.hero.healthDisable           00a5eb38  player%d.lockOnHealthFraction
                                                00a5f734  player%d.weapon%d.target.health
                                                00a5f704  player%d.weapon%d.target.healthFraction
```
Note the PDB field names (`healthAsHeroFraction`) do **not** match the registered strings (`hero.healthFraction`). Use the strings.

Two more events on this path that are easy to miss: `player1.healthRegenPulseRate` is `0.5f` while health is rising under the **Bacta Tanks** bonus (`0xC10B9CDC`) and `FLT_MAX` otherwise; and `weapon%d` / `team%d` digits are **1-based** (`LEA EDI,[EAX+1]` at Phantom `00612420`), so it is `player1.weapon1.*`, never `weapon0`.

### 1.6 Hand-off into a bar

`ElementBar::EventValue` (Phantom `0x005F6690`):
```c
if ((T == type_Int) || (T == type_Uint) || (T == type_Float)) { f = GetDataFloat(...); SetValue(f); }
```
Raw float, no pre-scaling. Anything registered as `type_String` is silently inert here — which is why a bar bound to `player1.vehicle.hackingTimeFraction` (registered `type_String(8)`, Phantom `006123b7 6a 08`) does nothing at all despite the name.

---

## 2. Inverted `MinValue` > `MaxValue`

### VERDICT: **NO — qualified, and the two paths fail differently. Neither runs the bar backwards.**

Nothing is rejected at parse time. `ElementBar::ReadData` (Phantom `0x005F6780`) and `ElementBar::SetProperty` (Phantom `0x005F6830`) are bare hash switches with raw float stores and zero validation:

```c
if (hash == 0x7332c626) { mMaxValue = GetFloatArg(data,0); return true; }   // MaxValue
if (hash == 0xf1a5a54c) { mMinValue = GetFloatArg(data,0); return true; }   // MinValue
```
(`MinValue = 0xF1A5A54C`, `MaxValue = 0x7332C626`, `Value = 0x425ED3CA`, `EventValue = 0x8500ADD4` — recomputed independently, and a byte-scan for `4C A5 A5 F1` on Phantom returns exactly three hits, all inside `ElementBar`: `005f66ff`, `005f67e8`, `005f6882`. `ElementBar` is the sole consumer of the pair in the whole binary.)

No warning is logged for a bad *value*. `Item::Read` only warns on an unrecognized *key* hash — `RedWarning::LogMessage("Error reading parameter 0x%08x\n")`, `HUDItem.cpp:0x2b0`, and only on Phantom/modtools.

### 2a. ODF / `.hud` path → the range is **repaired at load by collapsing it**, and the bar reads empty forever

`HUD::ElementBar::PostReadSetup` — Phantom `0x005F6740`, modtools `0x00694EE0`, Steam `0x0054A3B0`, GOG `0x0054B100`:

```
; Phantom 005F6740
005f6740  MOVSS  XMM0,[ECX + 0x20]    ; mMinValue
005f6745  COMISS XMM0,[ECX + 0x24]    ; min ? max
005f6749  JBE    0x005f6750           ; min <= max -> skip
005f674b  MOVSS  [ECX + 0x24],XMM0    ; <== REPAIR: mMaxValue = mMinValue
005f6750  MOV    EAX,[ECX]
005f6752  MOVSS  XMM0,[ECX + 0x1c]    ; mValue
005f675d  MOV    EAX,[EAX + 0x4]      ; ElementBar vtable slot 1 == SetValue
005f6760  CALL   EAX
```
modtools x87 form is identical in semantics: `FLD [ECX+0x24] / FCOMP [ECX+0x20] / FNSTSW AX / TEST AH,0x5 / JP skip / MOV EAX,[ECX+0x20] / MOV [ECX+0x24],EAX`.

**It collapses, it does not swap.** `max := min`, so the range width becomes exactly zero.

This is reached unconditionally. `HUD::Item::Read` (Phantom `0x00617D10`, modtools `0x006B6B80`, Steam `0x00564560`) ends its `DATA`/`SCOP` chunk loop with a virtual call through the **primary Item vtable**:
```
; Phantom Item::Read tail
00617d60  MOV  EAX,[EAX + 0x20]   ; per-chunk virtual ReadData
00617dd5  JL   0x00617d38         ; loop
00617ddb  MOV  EAX,[EBX]
00617ddf  CALL dword ptr [EAX + 0x24]   ; <== UNCONDITIONAL virtual PostReadSetup
```
(Steam `005645ab` / `00564600` are the same two slots. Note `+0x20`/`+0x24` are the *primary* Item vtable; the `+0x14`/`+0x18` figures floating around refer to `ElementBar`'s secondary vtable — `ElementBar` is an MSVC secondary base at `ElementBarBitmap+0x220`, `ElementBarSegmented+0x200`.)

Both concrete subclasses chain into the repair — there is no escape hatch:
- `ElementBarBitmap::PostReadSetup` Phantom `0x005F7480` / modtools `0x00696340` / Steam `0x0054B320` / GOG `0x0054C085`
- `ElementBarSegmented::PostReadSetup` Phantom `0x005F88E0` / modtools `0x006976A0` / Steam `0x0054C7C0` / GOG `0x0054D510`

Then the zero-width range hits the divide guard in `ElementBar::SetValue` (Phantom `0x005F68D0`, modtools `0x00694D30`, Steam `0x0054A260`, GOG `0x0054AFB0`) on every subsequent update:

```
005f68e5  SUBSS  XMM3,XMM2      ; denom = max - min  == 0 after the repair
005f68ff  UCOMISS XMM3,XMM0
005f6902  LAHF
005f6903  TEST   AH,0x44
005f6906  JNP    0x005f691c
005f691c  MOV    dword ptr [EBP + 0x8],0x0   ; returns literal 0.0f, DIVSS never reached
```

**Net: `MinValue 1 / MaxValue 0` in a `.hud` gives a permanently empty bar. Not a backwards bar, not a step function, no divide-by-zero, no warning.**

### 2b. Runtime `SetProperty` path → the inversion survives, and produces a **forward threshold latch**

`SetProperty` stores raw and immediately calls virtual `SetValue`; it does **not** re-run `PostReadSetup`. So a script/editor write of `MinValue > MaxValue` after load does reach the remap unrepaired. It still cannot run backwards, because the input clamp fires first and its min/max roles are hardcoded:

```
; Phantom ElementBar::SetValue 005F68D0
005f68e5  SUBSS  XMM3,XMM2      ; denom = max - min      (NEGATIVE when inverted)
005f68e9  COMISS XMM2,XMM0      ; min ? v
005f68ec  JBE    0x005f68f3
005f68ee  MOVAPS XMM1,XMM2      ; v < min  -> clamped = min      (hardcoded lower)
005f68f3  MINSS  XMM1,XMM0      ; else     -> clamped = min(max,v) (hardcoded upper)
005f68fa  MOVSS  [ECX + 0x1c],XMM1   ; mValue = clamped  (DESTRUCTIVE)
005f6908  SUBSS  XMM1,XMM2      ; num = clamped - min
005f690c  DIVSS  XMM1,XMM3      ; t = num / denom
```
Steam/GOG spell the upper clamp `COMISS XMM0,XMM1 / JA / MOVAPS XMM1,XMM0`; modtools uses `FCOMP`/`TEST AH,0x5` then `FCOMP`/`TEST AH,0x41`. Same semantics.

With `min > max` the predicate is self-contradictory, so `clamped` can only ever land exactly on an endpoint:
- `v < min` → `clamped = min` → `num = 0` → `t = 0/negative = -0.0`
- `v >= min` (hence `v > max`) → `clamped = max` → `num = max - min = denom` → `t = +1.0`

The negative denominator's sign cancels exactly in both branches. An emulation of the exact `COMISS`/`MINSS`/`FCOMP` flag semantics over 200,000 random inverted pairs produced the distinct value set `{-0.0, 1.0}` and nothing else.

So `MinValue 1 / MaxValue 0` driven by `healthFraction` gives a bar that is **full at 100% health and empty at everything below** — a forward latch tripping at `MinValue`, which is the opposite of what you wanted. `MinValue 0.75 / MaxValue 0.25` trips at 0.75, again forward.

### 2c. `Min == Max`

**Guarded, returns literal `0.0f`.** The `UCOMISS`/`LAHF`/`TEST AH,0x44`/`JNP` test above is an *exact-equality* test on the already-computed difference, so it also catches underflow-to-zero. No `DIVSS` executes, no Inf, no NaN. modtools' x87 form is stack-balanced on the guarded exit (`FSTP ST0 / FSTP ST0 / FLD [0x00a2a06c]` where that constant is `00 00 00 00`), so no x87 stack leak either. The bar is permanently empty.

### 2d. The escape hatch: NaN / Inf

The clamp does **not** sanitize a NaN input, on any of the three code shapes — `MINSS` returns its source on unordered; `COMISS/JA` and `FCOMP/TEST AH,0x41` both fall to the value-preserving branch. `PostReadSetup` also skips the repair on unordered (`COMISS/JBE` taken; modtools `FCOMP/TEST AH,0x5/JP` taken since C0=C2=C3=1 → `AH&5 = 5` → even parity). `MinValue = -INF` reaches the same place arithmetically (`+INF / +INF` → NaN). Downstream, `ElementBarBitmap::SetValue`'s gate `COMISS XMM0,[1e-4] / JBE` is *taken* on unordered, so all geometry writes are skipped; `ElementBarSegmented` gates its loops on `0.0 < width*t`, also false. So a NaN bar is inert garbage, not a crash and not a backwards bar. Whether the HUD compiler can actually emit NaN/INF into the chunk is **untested** — the runtime clearly accepts it (raw 4-byte float from `GetFloatArg`, no validation anywhere), but I did not examine the toolchain.

### 2e. There is no other way to invert either

`ElementBarSegmented::SetValue` radial mode explicitly `fabs`es the sweep and always increments upward, so swapping `AngleStart`/`AngleEnd` is neutralised too:
```
; Phantom 005F8FF0, radial branch
005f908e  SUBSS  XMM2,XMM1              ; end - start
005f9097  ANDPS  XMM2,[0x009e0ba0]      ; FABS (verified: ff ff ff 7f x4)
005f90aa  ADDSS  XMM3,XMM1              ; endAngle = start + |span| * t
005f90b3  COMISS XMM3,XMM1 / JBE -> skip ; only sweeps if endAngle > start
```

And `t` cannot leak past `SetValue` in a form a downstream consumer could re-invert: the thunk at Phantom `0x0040792D` has exactly **three** code xrefs — `ElementBarBitmap::SetValue` (`005f7948`), `ElementBarSegmented::SetValue` (`005f901b`), `ProceduralBarBitmap::SetValue` (`0061bca1`) — plus the vtable slot at `0x009FD5B0`. All three use `t` linearly and none re-derives anything from `mMinValue`/`mMaxValue`. `SetValue` also writes the clamped value back to `mValue+0x1c`, so a direct field reader sees the same two-valued signal.

### 2f. The `(0, FLT_MAX, 0.01)` metadata is editor-only

`ElementBar::Factory::Setup` (Phantom `0x005F6940`) registers `Property::Init("MinValue", …, 0.0, 3.4028235e38, 0.01)`, and the callee (`0x0041BB08`) writes PDB-named fields `floatRange.minValue / maxValue / stepValue`. That is **verified by symbol names**, and it lives in base game code, not a modtools-only editor. But the only readers are `GetMaxFloatValue` (`0x006176E0`) and `GetMinFloatValue` (`0x006177A0`), each with exactly one xref — its own thunk — called only from `Editor::Update` (`0x005F05C0`). **Nothing in `ReadData`, `SetProperty`, or `SetValue` consults it.** It stops you typing a negative `MinValue` in the editor; it does nothing about inversion, and nothing at all at runtime.

---

## 3. The bigger trap: any range other than `0..1` corrupts an `ElementBarBitmap` at load

This is not about inversion specifically, and it is the thing most likely to bite you.

`ElementBarBitmap::PostReadSetup` latches its geometry constants **after** `ElementBar::PostReadSetup` has already dispatched the virtual `SetValue` that mutates that same rect and those same UVs. Verified byte-for-byte on Phantom (`0x005F7480`), Steam (`0x0054B320`), modtools (`0x00696340`):

```
; Phantom 005F7480
005f7495  CALL 0x004011c7      ; -> ElementBar::PostReadSetup  (runs virtual SetValue HERE)
005f74b2  CALL 0x0041c558      ; GetRect(&left,&top,&right,&bottom)   -- reads the MUTATED rect
005f74bf  SUBSS XMM0,[EBP-0x4] ; right - left
005f74d5  MOVSS [EDI + 0x47c],XMM0   ; mBarWidth  <-- LATCHED
005f74de  CALL 0x00407838      ; GetTexCoords(...,&mBarU1,...)  <-- LATCHED
```

Normally that ordering is harmless only by coincidence. The ctor (Phantom `0x005F6590`) sets `mValue = 1.0`, `mMinValue = 0.0`, `mMaxValue = 1.0`, and `ElementBarBitmap`'s ctor (`0x005F6B40`) sets `mBarWidth = mBarU1 = 1.0` with both stretch flag bits set at `+0x484`. With a `0..1` range, `t == mValue` identically, so `ElementBarBitmap::SetValue`'s change-detection is false and the geometry write is skipped:

```
005f792b  MOVSS XMM0,[EDI + 0x1c]   ; old mValue -- cached BEFORE the call
005f7948  CALL 0x0040792d           ; -> ElementBar::SetValue, returns t
005f795b  SUBSS XMM0,[EBP + 0x8]    ; old mValue - t     <-- TWO DIFFERENT SCALES
005f7960  ANDPS XMM0,[0x009e0ba0]
005f7967  COMISS XMM0,[0x009e1618]  ; 1e-4
005f796e  JBE   0x005f7b6d          ; skip all geometry writes
```
The epsilon compares the **old raw `mValue`** (in `[min,max]`) against the **new normalized `t`** (in `[0,1]`). Break the coincidence and the write fires at load, while `mBarWidth`/`mBarU1` are still at their ctor value of `1.0`:

```
newU1    = t * mBarU1(1.0)    + u0    -> SetTexCoords(u0, v0, newU1, v1)
newRight = t * mBarWidth(1.0) + left  -> SetRect(left, top, newRight, bottom)
```
and `PostReadSetup` then latches `mBarWidth = newRight - left` and `mBarU1 = newU1` from those wrecked values.

An inverted or equal pair does this **deterministically**: repair → `t = 0.0`, old `mValue = 1.0` → `|1.0 - 0.0| > 1e-4` → fires → rect collapses to zero width → `mBarWidth` latched at `0.0`. `mBarWidth` has no ODF key (`ElementBarBitmap::ReadData` Phantom `0x005F7520` handles only `0x79b5f340`, `0xf5b1a000`, `0x2b76908f`, `0x2deba703`, `0xf5989fb2` before delegating), so it is written only by the ctor and by `PostReadSetup`. **A later runtime `SetProperty("MinValue",0)/("MaxValue",1)` cannot restore it.**

Related genuine defect in the same function: `RedBitmapElement::GetTexCoords` (real impl Phantom `0x008C8BD0`) returns `param_3` as an **absolute** `u1`, and `ElementBarBitmap::PostReadSetup` stores it into `mBarU1` unsubtracted, while the adjacent rect path correctly does `right - left`. So the UV lerp is `u0 + t*u1` rather than `u0 + t*(u1-u0)`. Both stretch behaviours are gated by bits 0 and 1 at `+0x264` (`ElementBar`-relative), both default ON, keys `0x79b5f340` and `0xf5b1a000`.

---

## 4. Verified / uncertain

**VERIFIED — read out of the named build's own instructions:**

| | Phantom `Battlefront2.exe` | modtools `BF2_modtools_MemExt.exe` | Steam `BattlefrontII.exe` | GOG `BattlefrontII_MemExt.exe` |
|---|---|---|---|---|
| `"player%d.healthFraction"` | `009FFB0C` | `00A5FED0` | `007A1EFC` | `007A2D6C` |
| `GameEvents::Open` | `00611F90` | `006AEF00` | `0055E3A0` | `0055F120` |
| `EventClass::Create` | `0040B9A1`ᵗ | `0040A394`ᵗ | `0055DE40` | `0055EBC0` |
| `gPlayerEvents[0]` | `00B26FA8` | `00BA3B68` | `01E56C40` | `01E580F0` |
| `…healthFraction` EventClass\* | `00B26FB4` | `00BA3B74` | `01E56C4C` | `01E580FC` |
| `sizeof(PlayerEvents)` | `0x31C` | `0x328` | `0x328` | `0x328` |
| `GameEvents::UpdateHealth` | `00614400` | `006B4500` | `005619F0` | `00562770` |
| `Item::Read` | `00617D10` | `006B6B80` | `00564560` | — |
| `Item::ReadEvent` | `00617E30` | `006B6360` | `00564740` | `005654C0`* |
| `Item::GetFilteredEventName` | `00617540` | `006B6270` | `00564690` | `00565410`* |
| `Item::SetEvent` (unfiltered) | — | `006B6500` | — | — |
| `Item::SetEventFilter` | `00618080` | `006B6560` | `00564840` | `005655C0` |
| `EventClass::FindByHashID` | `0060F4D0` | `006AD940` | `0055DEE0` | — |
| `ElementBar::SetValue` | `005F68D0` | `00694D30` | `0054A260` | `0054AFB0` |
| `ElementBar::PostReadSetup` | `005F6740` | `00694EE0` | `0054A3B0` | `0054B100` |
| `ElementBar::ReadData` | `005F6780` | `00694F10` | `0054A3E0` | `0054B130` |
| `ElementBar::SetProperty` | `005F6830` | `00694DB0` | — | `0054B010` |
| `ElementBar::GetProperty` | `005F66B0` | `00694E60` | `0054A340` | `0054B090` |
| `ElementBar::EventValue` | `005F6690` | — | — | — |
| `ElementBarBitmap::SetValue` | `005F7920` | — | `0054B070` | — |
| `ElementBarBitmap::PostReadSetup` | `005F7480` | `00696340` | `0054B320` | — |
| `Character::Spawn` (CombatShielding) | `0049F220` | `006457A0` | `00451B90` | `00451B70` |

ᵗ = incremental-link thunk. * = carried from an earlier pass, not independently re-read.

`ElementBar` layout, verified identical on all four builds: `+0x04` `mEventValue` (EventHandler, 20 bytes), `+0x18` `mDefaultValue`, `+0x1C` `mValue`, `+0x20` `mMinValue`, `+0x24` `mMaxValue`. `ElementBar` vtable Phantom `0x009FD5AC`: slot 1 `SetValue`, 2 `SetProperty`, 3 `GetProperty`, 5 `ReadData`, 6 `PostReadSetup`.

**Calling-convention drift — do not port a hook signature across builds.** `SetEventFilter`, `GetFilteredEventName`, `Item::ReadEvent`, `EventClass::FindByHashID` and `GameEvents::UpdateHealth` are `__cdecl` on Phantom/modtools and genuine `__fastcall` on Steam/GOG (`Steam SetEventFilter 00564840: MOV [0x01e56fb8],ECX / MOV [0x01e56fb4],EDX / RET`). Retail's `GetFilteredEventName` has **no size parameter** — `0x1FF` is hardcoded inside. Retail passes floats to `Event::Event` in **XMM2**. Hero bit is `EntityClass+0x36C` on Phantom/modtools but `+0x328` on Steam/GOG.

**UNCERTAIN / could not establish — stated plainly:**

1. **Whether stock `.hud` files declare `EventNameFilter("player%d")`.** That is a data question; the binary only proves the key is parsed. Irrelevant on PC either way.
2. **The 12-byte `PlayerEvents` size drift** between Phantom (796) and all three shipping builds (808). Fields 0–60 (the entire health block) are byte-identical and verified; late offsets must **not** be ported from Phantom.
3. **What writes `PlayerData+0xd8`** (source `FLInputManager::s_instance+0x14` = modtools `0x00CAEB34`). Exactly one xref in the whole modtools binary — the read at `006b55a0`. No absolute write exists. Unresolved. Phantom has no such field at all.
4. **Whether the load-time geometry latch (§3) is visually observable in game.** The instruction order, the change-detection scale mismatch, and the absence of any other writer for `mBarWidth` are all verified in code. What I did **not** verify is whether a later layout/resolution pass re-`SetRect`s the element. If it does, `mBarWidth` is still latched at 0 and the bar is still permanently empty, so the conclusion is unchanged — but the mechanism is code-derived, not observed.
5. **Whether the HUD compiler can emit NaN/INF** into a `MinValue`/`MaxValue` chunk.
6. `ElementBarSegmented::SetValue` and `ProceduralBarBitmap::SetValue` on modtools and GOG — not located; quote no addresses for those.
7. GOG's `Open` loop structure, `GetFilteredEventName` and `ReadEvent` bodies.
8. Phantom is a **2026 recompile** (`RedWarning::SetLogData` → `Z:\Projects\BattleFront2-Source\main\…`, `"Jun 28 2026"`) versus modtools `C:\Battlefront2\main\…`, `"Feb  9 2006"`. Treat Phantom as authoritative for names only; behavioural divergences (like the missing `+0xd8` gate) are suspect, not "the original".

---

## 5. What to actually write

```
MinValue    0.0
MaxValue    1.0
Value       0.0          // or 1.0 -- must equal its own normalized t, see below
EventValue  "player1.healthFraction"
```

**Rules and traps, in order of how likely they are to burn you:**

1. **Never invert.** `MinValue 1 / MaxValue 0` in a `.hud` gives a dead bar (collapse → zero-width → `t = 0.0` forever) *and* permanently destroys the element's authored rect and UVs. Set from script instead and you get a forward on/off latch tripping at `MinValue`. There is no configuration in which the bar runs backwards.
2. **Never use any other range on an `ElementBarBitmap`.** `MinValue 0 / MaxValue 100 / Value 50` is not merely a scaling choice — it fires the load-time geometry write with un-initialised `mBarWidth`/`mBarU1` and latches the result. `0..1` is the only range where `t == Value` identically and the write is skipped. If you must use another range, the safe subcase is `MinValue 0` with `Value 0` (also a fixed point of the remap).
3. **`healthFraction` is not bounded above.** Combat Shielding gives `1.25` on spawn. The clamp pins `t` at 1.0 so the bar looks right, but any script reading the value, or any `MaxValue` you set above 1, will see the overspill. Use `player1.bonusHealthFraction` (clamped `[0,1]`, suppressed for heroes) if you want to render the overshoot.
4. **`healthFraction` freezes rather than zeroing** when the player enters a vehicle, becomes a hero, or raises binoculars. If your element is always visible you need three bindings, not one: `player1.healthFraction`, `player1.healthInVehicleFraction`, `player1.hero.healthFraction` — plus the matching `*Disable` events if you want to hide it. `player1.vehicle.healthFraction` is a different thing: the vehicle's own health, not the pilot's.
5. **Test bindings in modtools.** A misspelled event name is completely silent on Steam and GOG — `ReadEvent` returns `true` and simply never registers. modtools/Phantom log `"HUD Element unable to find event %s"`.
6. **Bars ignore `type_String` events.** `ElementBar::EventValue` accepts `type_Int`/`type_Uint`/`type_Float` only. `player1.vehicle.hackingTimeFraction` and `hackedTimeFraction` are registered `type_String` despite the names, so a bar bound to either is inert.
7. **To render a bar backwards there is no engine support.** No inverted health event is registered, `MinValue`/`MaxValue` cannot do it, and `ElementBarSegmented`'s radial `AngleStart`/`AngleEnd` is `fabs`ed with an always-ascending sweep. Authoring the element rect with `left > right` would make `mBarWidth = right - left` negative and `newRight = left + t*mBarWidth` sweep leftward — the arithmetic works, but I have **not** verified that the renderer accepts a reversed quad, and it interacts with the §3 latch. Treat it as an experiment, not a recipe.