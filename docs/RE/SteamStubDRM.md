# SteamStub DRM on the 2006 Steam retail build

Notes from unwrapping `BattlefrontII.exe` out of the original 2006 Steam depot
(`Steam2Browser/steam2info/extracted/6061_1/GameData`). `testapp.exe` in the same
folder is a byte-identical rename of it, not a separate binary.

## Build identity

| field | value |
| --- | --- |
| md5 | `c1fca4cf1dcc3fab753a1bc5d2fe7803` |
| size | 4,960,256 |
| PE timestamp | 2006-01-31 19:25:56 |
| linker | 7.10 (MSVC 2003) |
| PDB path | `e:\Battlefront2\main\Battlefront2\Build\PC Final LTCG\Battlefront2.pdb` |
| imagebase | 0x400000, `.text` va 0x1000 vs 0x38b0ab |

This is a fourth build lineage, separate from the three already in use:

| build | config | timestamp |
| --- | --- | --- |
| this one | `PC Final LTCG` | 2006-01-31 |
| `BF2_modtools.exe` | `PC Release` | 2006-02-07 |
| `BattlefrontII.Debug.FullScreen.1080.exe` | `PC Modtools Release` | 2006-02-09 |
| Steam / GOG 2017 | stripped, linker 12.0 | 2017-10-23 |

It is the v1.1 patch level: retail shipped 2005-11-01, so a 2006-01-31 build is post-release,
and the depot's installer script records `Revision 10101` alongside `Update1_1.txt`.

> The string `Version     : 1.00` is a dead literal present in **every** build, Steam and GOG
> included. It is not a version indicator - do not use it to identify a build.

## Wrapper

`.text` ships encrypted: entropy 8.000 across every 64 KB chunk, zero `push ebp; mov ebp, esp`
prologues, zero `int3` padding. `.rdata` and `.data` are plaintext.

Steamless identifies the wrapper as **SteamStub Variant 2.1**. (The entry stub's byte
signature also matches the documented Variant 1.0 pattern, so do not identify this family
from the signature alone - trust the unpacker's classification.)

Layout: an extra `.bind` section at va `0x01FF3000`, size `0x56000`, with the PE entry point
redirected into it at `.bind+0x2ED`:

```
EB 04 DE C0 DE C0                          jmp +4 over marker 0xC0DEC0DE
53 51 52 56 57 55 8B EC 81 EC 00 10 00 00  push regs / frame / sub esp,0x1000
```

## Chain

Every key below is in the file. The Steam client supplies none of them - it only flips a
status byte.

1. **Header** at va `0x01FF5204`, `0x364` bytes. Chained XOR: each dword is XORed with the
   previous *ciphertext* dword, seeded `0xBD165878`. Contains the appid string `06060`, the
   Win32 API name table the stub resolves by hand, and the Steam error strings.

2. **Steam handshake.** `OpenEventA` + `OpenFileMappingA("Local\SteamStart_SharedMemFile")`
   + `MapViewOfFile`. Requires `shm[0x90] == 2` and `shm[0x94] == 0`, then writes its own PID
   to `shm[0x98]` and header fields to `shm[0xA0]/[0xA4]/[0xAC]`, sets `shm[0x94] = 1` and
   waits 5 s on an event. The result byte must come back `'0'`. Failures surface as the
   familiar `Application load error X:XXXXXXXXXX`. This is a gate, not a key source.

3. **Descriptor blob** at va `0x02047168`, `0x1000` bytes. Same chained XOR, seeded with the
   header checksum `0xB1CA0CC5`. `blob[0]` equals that checksum - use it to verify the decrypt.
   Fields: `blob[0xD4]` = payload ciphertext va `0x01FF5568` (immediately after the header),
   `blob[0xFE8]` = payload size `0x51C00`, `blob+0x1B8` = 128-bit payload key.

4. **Payload cipher: XTEA, not TEA.** The tell is the key schedule indexing by `(sum>>11)&3`
   and `sum&3`; a plain TEA implementation produces garbage. 32 rounds, delta `0x9E3779B9`,
   CBC-chained with IV `(0x55555555, 0x55555555)`. Key for this binary:
   `e6bc8e92 e67470a9 3953d86e 157ec0a9`.

   Reference decrypt:

   ```python
   M, DELTA, ROUNDS = 0xFFFFFFFF, 0x9E3779B9, 32
   def xtea_dec(v0, v1, K):
       s = (ROUNDS * DELTA) & M
       for _ in range(ROUNDS):
           v1 = (v1 - (((((v0 << 4) & M) ^ (v0 >> 5)) + v0) & M ^ ((s + K[(s >> 11) & 3]) & M))) & M
           s  = (s + 0x61C88647) & M          # i.e. s -= DELTA
           v0 = (v0 - (((((v1 << 4) & M) ^ (v1 >> 5)) + v1) & M ^ ((s + K[s & 3]) & M))) & M
       return v0, v1
   ```

5. **Payload** is Valve's own `steamdrm.dll` (Steamless calls it `SteamDRMP.dll`): 334,848
   bytes, PDB `d:\s3_main_2\bin\util\drm\steamdrm.pdb`, timestamp 2009-02-25, statically
   linked Crypto++ (`CBC_Decryption` RTTI present), one export `start`. Note the 2009 date -
   the wrapper was applied three years after the game was built.

6. The stub manually maps that DLL (VirtualAlloc, section copy, relocs, per-section
   VirtualProtect) and calls `start(blob, 0x1000)`. **`.text` decryption happens inside
   `steamdrm.dll`**, keyed from the descriptor blob. Its return value is the original entry
   point, reached via a single `jmp eax` at `0x01FF497D`.

## Recovering symbols without unpacking

Because `.rdata`/`.data` stay plaintext, the Lua registration tables survive an encrypted
`.text`. There are 15 contiguous `{const char* name, lua_CFunction fn}` arrays holding
**578 `ScriptCB_` pairs**, the largest at `0x007F64C0` with 150 entries; every function
pointer lands inside `.text`. Scan for dwords pointing at a `ScriptCB_` string whose
following dword falls in the `.text` range.

Spot-check after unpacking: `0x0041D720` decodes to `mov eax,[0x01D77F78]; push eax; call ...`,
the expected Lua callback prologue.

## Unpacking

Steamless v3.1.0.5 handles it directly:

```
Steamless.CLI.exe --verbose BattlefrontII.exe
```

Its step 4 scans `SteamDRMP.dll` for the `.text` key, which is the one step of the chain not
derivable from the outer stub alone. Output: `.bind` removed, entry point restored to
`0x00352049`, `.text` entropy 8.000 -> 6.569, 1,033 prologues and 26,897 `int3` padding runs
where there were none.

## Lua API delta

576 `ScriptCB_` names here vs 577 in the 2017 Steam build. The only difference is Steam's
`ScriptCB_LastSignInError`. Nothing was removed in eleven years.
