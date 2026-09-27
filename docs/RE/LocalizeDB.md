# Localize DB (RedLocalizeDB)

How the engine stores and looks up localized strings, and the design for letting
scripts pass plain text where a localization key is expected.

Addresses are modtools (`BattlefrontII.Debug.FullScreen.1080.exe`, imagebase `0x400000`).
Steam/GOG/retail are **not ported yet**.

## Storage

| Item | Address | Notes |
|---|---|---|
| Hash table | `0x00D2D88C` | 16384 key slots followed by 16384 value slots (`_Find`/`_Store` are passed `size = 0x8000`, half is the slot count) |
| Entry count | `0x00D2D888` | Unreliable: incremented off a garbage register after `_Store` |
| Main blob | `0x00D2D870` | `RunTimeHeap` copy of the language `BODY` chunk |
| Patch blob | `0x00D2D874` | `RunTimeHeap` copy of the patch localize file |
| Language name | `0x00D2D868` | `"english"`, `"spanish"`, ... chosen by `0x007F3110` from `GetUserDefaultLangID` and overrides |

Record format inside a blob, packed back to back, terminated by a record whose size is 0:

```
+0  u32      PblHash of the key
+4  u16      record size in bytes (walk: rec += size)
+6  wchar_t  text[], NUL terminated
```

The table value is the record pointer. `Find` returns `record + 6`.

### Hash table (`PblHashTableCode`)

- `_Find` `0x007E1A40`, `_Store` `0x007E1A90`, `_Clear` `0x007E1A30`.
- Open addressing. Start slot `hash & (slots - 1)`, probe **downward**, wrap from slot 0 to the top.
- Key 0 marks an empty slot; `_Find(0)` returns NULL immediately.
- `_Store` never overwrites: if the key is already present it returns without writing.
- **Neither function has a full-table check.** With every slot occupied, a miss probes forever.

## Load and reset

- `RedLocalizeDB::Read` `0x007F3500` (from `ReadDataFileChunk`): walks child chunks, `NAME`
  selects the language, the matching `BODY` goes to `0x007F3400`.
- `0x007F3400`: allocates the blob from `RunTimeHeap`, then for each record
  `if (!_Find(hash)) _Store(hash, rec)`. **First key loaded wins.**
- `ReadPatchLocalizeFile` `0x007F3490`: same walk without the `_Find` check (still first-wins,
  because `_Store` does not overwrite).
- `0x007F36F0`, called from `PostStateCleanup`: `_Clear` on the 16384 key slots, zeroes the
  count and both blob pointers. The table is rebuilt on every game state change.

## Lookup

`RedLocalizeDB::Find(uint hash, char nullOnMiss)` `0x007F3720`, 172 call sites:

```c
rec = _Find(table, 0x8000, hash);
if (!rec) return nullOnMiss ? 0 : L"[NULL]";   // 0x00A73E0C
return rec + 6;
```

Callers hold on to the returned `wchar_t*` (the message line queue, for example), so any
pointer handed out must stay valid for at least the rest of the game state.

## Lua functions that take a localization key

All of these `PblHash` the Lua string at call time. Only the 32-bit hash is kept or sent.

| Function | Addr | Key args | Display path |
|---|---|---|---|
| `ShowMessageText(key [, team])` | `0x0046D590` | 1 | `0x006AEBF0` calls `Find` per viewport, synchronously inside the Lua call. In MP the host also sends reliable event 8 `(hash, teamMask)` via `0x006EF660` |
| `ShowObjectiveTextPopup(key)` | `0x0046D6F0` | 1 | `PopupText` type 2, only if profile `mbObjectiveDetails` |
| `ShowSelectionTextPopup(key)` | `0x0046D750` | 1 | `PopupText` type 3 |
| `AddMissionHint(key)` | `0x0046D050` | 1 | `PopupText::AddHint` |
| `AddMissionObjective(team, text [, popup])` | `0x0046CF10` | 2, 3 | Stored in `Objective` (`0x0067FEF0`), looked up later |
| `ActivateObjective` / `CompleteObjective` | `0x0046CFD0` / `0x0046D010` | 1 | Objective lookup by the same hash, no text shown |
| `ScriptCB_SetTeamNames` | `0x00459A00` | 1, 2 | Resolved by `GetTeamLocalizeNames` (`0x006549D0`, `nullOnMiss = 1`) |

Lua functions that call `Find` directly: `ScriptCB_getlocalizestr`, `ScriptCB_usprintf`,
`ScriptCB_IFText_SetString`, `ScriptCB_IndexMultipageText`, `ScriptCB_GetKeyBoardCmds`, plus
leaderboard/login/friend getters.

## Lua 5.0 string table (modtools)

Verified from `luaS_newlstr` (`0x007C2CE0`) and `lua_pushlstring` (`0x007B8580`):

- `G = *(L + 0x10)`; `strt.hash = *(G + 0x0)` (`TString**`), `strt.nuse = *(G + 0x4)`,
  `strt.size = *(G + 0x8)` (power of two).
- `TString`: `+0x0 next`, `+0x4 tt`, `+0x5 marked`, `+0x8 hash`, `+0xC len`, `+0x10 chars`.
- Every Lua string (literals in loaded chunks, concatenation results, pushed strings) is
  interned here until the GC frees it. The lua_State pointer is `g_lua_state_ptr`.
- `lua_Number` is `float` in this build.

## Design: plain-text fallback (proposed, not implemented)

Goal: `ShowMessageText("Hold the bridge!")` shows that text instead of `[NULL]`, without
breaking real keys.

### Rejected: inserting records into the engine table

- A string stored before its real key loads makes `0x007F3400` skip the real record, so the
  translation is shadowed for the whole state.
- Every insert moves the table toward the no-full-check infinite loop.
- Keys are wiped by `PostStateCleanup` and would need re-storing.

### Chosen: hook the miss branch of `Find`, resolve from Lua's own string table

No per-function Lua hooks are needed. Every key the scripts pass is already an interned
Lua string, and the miss hands us its hash.

1. Patch only the miss return of `Find`. Hits are untouched.
2. On a miss, look the hash up in a GameExt-owned `hash -> wchar_t*` cache. Hit: return it.
3. Cache miss, on the main thread only: walk `strt` (`size` buckets, `next` chains), compute
   `PblHash` of each `TString`, and on a match widen it (UTF-8, falling back to the ANSI code
   page), store it in the cache, return it.
4. Nothing matched: return the stock result (`[NULL]` or NULL) and log the hash once.
5. Cache entries are never freed, so returned pointers stay valid. Growth is bounded by the
   distinct strings that were ever looked up and missed.

Properties:
- Real keys always win, load order does not matter, the engine table is never written.
- Covers every consumer, including key-taking Lua functions not listed above, and
  `nullOnMiss = 1` callers.
- **MP clients work for literals.** The mission chunk loads on every machine, so a literal in
  the script is interned on the client too, and the host's hash from event 8 resolves.
  Runtime-built strings (`"x" .. n`) only exist on the host, so clients still get `[NULL]`.
  Clients without GameExt always get `[NULL]`.
- `ShowMessageText` resolves inside the Lua call, so even temporary strings are alive.
  Deferred consumers (`AddMissionObjective` text) resolve later; a literal stays alive as a
  constant of its function, a temporary concatenation may have been collected by then.

Open points:
- Negative cache: a HUD element that looks up a missing key every frame would walk `strt`
  every frame. Rate-limit rewalks per hash (for example once per second).
- Thread safety: confirm `Find` is only called from the main thread, or restrict the walk to
  the main thread and guard the cache with a lock.
- Collisions only matter on a miss; worst case an unrelated string replaces `[NULL]`.
- Port `Find`, the table and the Lua string layout to Steam, GOG and retail. Check each build
  for inlined copies of `Find` in the LTCG builds.
