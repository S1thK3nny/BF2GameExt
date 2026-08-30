#pragma once

#include <stdint.h>

// =============================================================================
// Command post registration overflow -- the 17th post writes into ControllerManager.
//
// Observed as a CTD when joining a multiplayer server (Steam, 2026-08-25 and
// 2026-08-26, both records identical):
//
//   EXCEPTION C0000005  AV: WRITE addr 00000033   ECX=ESI=00000007
//   EXCEPTION C0000005  AV: WRITE addr 00000039   ECX=ESI=0000000D
//   GameLoop::Update -> net -> CommandFlyerClass::vfunction2
//                    -> CommandFlyer::CommandFlyer -> FUN_0047AC80 -> FUN_0047AE00
//   FUN_0047AE00 is __thiscall and opens with  *(int*)(this + 0x2c) = param_1
//   so this = 7  faults on 0x33, and this = 13 faults on 0x39.
//
// THE MECHANISM, and it is 100% stock code.
//
// `sPostArray` is a FIXED 16-ENTRY array (Steam 0x01E308E0, sized by the
// `PUSH 0x40` memset at 0x0047AA40).  The registration function appends with no
// capacity check at all:
//
//     else if (hint < count) goto use_it;
//     *countPtr = hint + 1;          // <-- unbounded
//     idx = hint;
//   use_it:
//     if (array[idx] == 0) { p = operator new(0xB48); CommandPost::CommandPost(p, cls, idx); }
//     FUN_0047AE00(array[idx], entity);
//
// On the 17th registration `idx` is 16, one past the end.  Slot[16] is
// `0x01E30920` -- ControllerManager's parked phase counter, written on every
// LeaveClient and cycled 1..15 elsewhere.  It is NON-ZERO, so the allocate
// branch is skipped and the phase counter is handed straight in as `this`.
// That is exactly why the two crashes reported 7 and 13: both are legal values
// of a counter that cycles 1..15, not random garbage.
//
// WHY WE KNOW THE ARRAY WAS INTACT: the exception was a WRITE.  Had the count
// already been past 16 on entry, the search loop would have faulted first, on a
// READ, at `MOV EAX,[ECX+0x2C]`.  So the count was exactly 16 and slots 0..15
// were fine.  Nothing was corrupted -- a fixed array was indexed one past its
// end.
//
// WHY IT REACHES 16.  `~CommandPost` NULLs its slot WITHOUT decrementing the
// count, and the search loop skips NULL slots, so every destroy-then-recreate
// burns a slot permanently.  The count is a monotonic high-water mark reset only
// by PreStateInit.  A long-lived client session that cycles posts therefore
// climbs to 16 even on a map with far fewer than 16 posts.
//
// MODTOOLS WARNS AND CRASHES ANYWAY.  FUN_0064FDF0 carries
// `if (0xF < idx) RedWarning::LogMessage("Exceeded %d command posts!")` from
// CommandPost.cpp:279 -- and then uses the out-of-range index regardless.  Retail
// deleted the check entirely.
//
// THE FIX.  Wrap the registration function.  Intervene ONLY when the engine is
// about to append past the end (no forced index AND count >= 16):
//
//   1. Scan slots 0..15 for a NULL entry left behind by a destroyed post.  If one
//      exists, write that index to the forced-index global and delegate.  Stock's
//      own forced-index arm then sees an empty slot, allocates a fresh
//      CommandPost, and leaves the count alone -- so the leak is RECLAIMED using
//      the engine's own code path, with no reimplementation.
//   2. If all 16 are genuinely live, force the best-matching live slot: prefer one
//      whose CommandPostClass matches, else slot 15.  The entity binds to a
//      wrong-but-valid post instead of taking the process down.
//
// NEVER RETURN NULL.  Every caller dereferences the result unconditionally --
// immediately after the call site at 0x00477D33 comes
// `MOV [ESI+0x1D94],EAX / OR byte ptr [EAX+0xB40],1`.  Returning null would just
// move the access violation from +0x2C to +0xB40.
//
// The forced-index global is also what the network layer uses: it is filled from
// a 4-bit ReadBits, so it is always 0..15 and can never itself select slot 16.
// A hint of 15 can still set count = 16, which is why the clamp tests the COUNT.
//
// Per-build:
//   modtools  FUN_0064FDF0  __cdecl      hint 0x00AD5494  array 0x00AD5498  count 0x00AD549C  class +0x1A54
//   Steam     FUN_0047AC80  __fastcall   hint 0x007E6318  array 0x007E6314  count 0x007E631C  class +0x0B3C
//   GOG       not derived -- the fix omits itself rather than guessing.
//
// The calling convention DIFFERS between builds; it is read from each build's own
// prologue, not ported.
// =============================================================================


void command_post_overflow_fix_install(uintptr_t exe_base);
void command_post_overflow_fix_uninstall();
