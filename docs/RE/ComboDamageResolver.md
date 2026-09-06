# Combo::Attack::_ResolveDamageData

Traced 2026-09-06 from a reproducible melee crash. This is the function that turns a combo
attack into the swept damage-ray samples `WeaponMelee::UpdateFire` later tests against.

## Addresses

| | modtools | Steam | GOG |
|---|---|---|---|
| `Combo::Attack::_ResolveDamageData` | `0x005FCD30` | `0x004727A0` | `0x004727A0` |
| `Combo::ResolveForWeapon` (sole caller) | `0x00600990`, call at `0x00600C70` | `0x00475640`, call at `0x004758FE` | `0x00475640`, call at `0x004758FE` |
| `SoldierAnimatorClass::GetUpperBodyAnimation` | `0x0057DD40` | `0x006439E0` | `0x00644A80` |
| lower-body variant (`idx*2+1`) | `0x0057DD80` | `0x00643A10` | `0x00644AB0` |
| per-map trailing field getter | `0x0057DEA0` | - | - |
| `SoldierAnimatorClass::sInstance` | `0x00B8D3C4` | `0x01EAFB1C` | `0x01EB0FD0` |
| animation index load site | `0x005FCEB3` | `0x00472933` | `0x00472933` |

modtools reaches the resolver through the ILT thunk at `0x00401799`; the retail builds call
it directly. There is **exactly one caller on every build**, `Combo::ResolveForWeapon`, and
it discards the return value.

Calling convention on all three: `__thiscall`, five stack args, `RET 0x14`, EBP frame.
The retail builds wrap it in an SEH frame; modtools does not.

## Struct layouts

`Combo` is the same on all three builds:

    +0x00  uint32   name hash
    +0x08  int      mMap          the soldier animation MAP
    +0x0C  ...      skeleton shared (passed to ZephyrSkeleton::Open)

`Attack` is **not**. The modtools debug build carries 0xC extra bytes ahead of these
fields:

| field | modtools | Steam / GOG |
|---|---|---|
| `mAnimIndex` (byte) | `+0x28` | `+0x1C` |
| flags dword (bit 19 = AnimatedMove) | `+0x34` | `+0x28` |

Do not port either offset across lineages. The engine's own read of the index is
`MOV r8,[EAX+disp8]` (`8A 48 28` modtools, `8A 40 1C` retail), which is the cheapest way to
prove the offset at runtime.

## The animation lookup, and why it is fatal

    modtools 0x005FCEB3  MOV CL,[EAX+0x28]     ; Attack::mAnimIndex
             0x005FCEB6  MOV EAX,[EDI+8]       ; Combo::mMap
             0x005FCEC9  CALL GetUpperBodyAnimation
             0x005FCECE  MOV [ESP+0x24],EAX    ; stored, never tested
             ...
             0x005FD17F  MOV EAX,[ESI]         ; SoldierAnimation+0, the Zephyr clip

`GetUpperBodyAnimation` is a bare table read with no rejection path, so an animation index
the map never populated comes back NULL. The resolver has **four unguarded uses** of it.
Its immediate sibling `Combo::ResolveForWeapon` performs the same lookup and does check
(`if (anim != NULL)`), which is why only the damage-ray path dies.

Two distinct null states both reach the crash:

- the table slot itself is NULL - dereferenced directly at `0x005FD17F`;
- the slot holds a `SoldierAnimation` whose clip pointer (`+0x00`) is NULL - survives to
  `ZephyrPoseDyn::SetAnimation` and dies one call later in `ZephyrPoseDyn::Update`
  (modtools `0x0082A5B0`, `MOVZX ECX,[EAX+8]` off `[pose+0x980]`).

`SoldierAnimation::GetFrameCount` (modtools `0x005701F0`, thunked at `0x004063F2`) treats a
NULL clip as zero frames, so the second state is one the engine half-expects.

**Reading a crash log for this:** `[Features] Prone` detours `0x005701F0` with its own null
guard (`soldier_prone.cpp`), so with Prone on a NULL animation survives four more
instructions and the AV lands on `0x005FD17F` with `ECX = EDX = 0xFFFFFFFF` (a `MOVZX/DEC`
of the guarded zero return) rather than inside the accessor. Same bug either way.

## The lower-body companion

A few instructions later the resolver fetches the `_lower` half of the same pair:

    if (attack->flags & (1<<19))  a2 = GetLowerBodyAnimation(map, idx);
    if (!a2)                      a2 = GetMovementAnimation(map, 0, 0, ...);   // unchecked

The fallback is the map's own idle clip rather than anything attack-specific, and it is only
reached once the primary animation is good. Left unguarded by
`combo_damage_anim_guard.cpp`.

## Per-map block geometry

The animator's per-map stride is `0x12E` dwords = `0x4B8` = 1208 bytes, and three
sub-arrays share it (`+0x24`, `-0x6C`, `+0x4B4`). Because the third starts at 1204, the
second holds indices 0..163 and `0xA4` (164) is the stock sentinel **because it is one past
the end**. See ROADMAP.md for what `ComboAnimIncrease` does to that bound.

## Related

- `PatcherDLL/src/entity/combo_damage_anim_guard.cpp` - the null guard.
- [[weapon_melee_damage_rays]] - what the resolved samples are used for.
- [[setcharacterweapon_melee_animmap]] - how a MAP of -1 gets into a shared weapon class.

## Update 2026-09-06: the fix is now a getter clamp plus this guard

The resolver guard alone was not enough. `_ResolveDamageData` is the only consumer that does
not test its lookup, but it is far from the only one that *receives* a bad value, and the
same bad values reach `UpdateActionAnimation`, `UpdateMovementAnimation`, `SetupPose` and
`EntitySoldier::Render`, all of which test for NULL and nothing else.

`combo_damage_anim_guard.cpp` therefore hooks the getters themselves, on all three builds:

| | modtools | Steam | GOG |
|---|---|---|---|
| `GetUpperBodyAnimation` | `0x0057DD40` | `0x006439E0` | `0x00644A80` |
| `GetLowerBodyAnimation` | `0x0057DD80` | `0x00643A10` | `0x00644AB0` |

Two values are turned into NULL there:

- **animation index >= 164** - out of range for the 164-slot map, so the read would land in
  the neighbouring map's block.
- **a returned `0xFFFFFFFF`** - the slot was never populated. A map is reset with
  `OR EAX,-1 / REP STOSD` over all `0x12E` dwords (modtools `0x0057E190`, `0x0057F573`), so
  -1 is the "empty" marker for animation slots, and no consumer tests for it. Nothing can be
  relying on -1 as a live value, because a consumer that got one would dereference it.

The upper- and lower-body getters are the same function apart from how they pick the slot
out of the pair (`SHL EAX,1` vs `LEA EAX,[EAX+EAX+1]`), which is what the install-time
signatures use to tell them apart. Retail's two are byte-identical to each other across
Steam and GOG, only shifted.

The index arrives as a dword whose low byte is the index - the engine reads it with `MOVZX` -
so the range test masks with `0xFF` before comparing and passes the argument through
untouched.

A signature miss no longer disables the whole file: the clamp and the resolver guard install
independently, and a miss logs the bytes actually found so an injected `E9` from another DLL
is distinguishable from a wrong address.
