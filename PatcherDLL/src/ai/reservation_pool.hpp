#pragma once

#include <stdint.h>

// =============================================================================
// AI reservation pool capacity.
//
// `ReserveManager::sList` is a `ListPool<ReserveManager::ReserveStruct,int>` of
// capacity 60 with a 24-byte element -- the only ListPool in the game with that
// capacity.  It holds every AI reservation claim: RT_REPAIR, RT_BOARD,
// RT_ATTACK, RT_FORMATION, RT_HeavyWeapons.  On a busy map 60 is not enough, and
// the log fills with
//
//     List pool is full; raise count from 60 to at least 2129
//
// Overflow is fail-safe.  Append is
// `mPeak++; if (mLength != mPoolSize) { store; mLength++ } else { RedWarning; return }`,
// so the claim is simply dropped -- nothing is written out of bounds.  The cost
// is AI that keeps re-requesting reservations it can never hold (IsReservedBy
// stays false, so Reserve retries the same claim next frame) plus the log spam.
//
// THE NUMBER IN THAT MESSAGE IS NOT A DEMAND FIGURE.  mPeak is incremented
// before the capacity test and decremented on every removal, and a dropped
// element never enters the list, so no Unreserve ever cancels its increment.
// `mPeak - mLength` is therefore net rejected adds accumulated since level load:
// it measures how LONG the pool has been starved, not how much is wanted.  Sizing
// to 2129 would be sizing to a stopwatch, and every query is a linear scan on
// mLength -- Reserve calls IsReservedBy first, so even insertion is O(n).
//
// 127 IS A HARD CEILING, not a chosen limit.  The count reaches the allocator
// through a `PUSH imm8` on every build, and `6A ib` is sign-extended.  At 0x80
// and above the count goes negative: the allocation request becomes ~4 GB,
// operator new[] returns NULL, and the ctor stores mElements = NULL -- after
// mPoolSize has already been written negative.  The first Reserve then writes
// through NULL.  Going higher means re-encoding that push as `PUSH imm32`, which
// is a 31-byte in-place rewrite on modtools (the 24 bytes of 0xCC at 0x005C6228
// are confirmed free) and needs three spare bytes on retail.  Not done here.
//
// The builds differ in how many places have to move:
//
//   modtools  ONE byte.  The ListPool ctor is out of line at 0x005C60F0 and
//             derives the allocation size, mPoolSize, the array cookie and the
//             construct loop count all from the single pushed argument.
//   retail    FOUR sites.  The ctor is inlined and constant-folded, and Init's
//             pushed count is DEAD (`PUSH ECX` on an uninitialised register).
//             Porting the modtools patch to "the push" alone is a silent no-op,
//             and writing only some of the four is worse than writing none --
//             an mPoolSize larger than the allocation is a heap overrun.  Every
//             site is verified to hold 60 before any of them is written.
//
// Note for replays: ReserveOffset feeds PblJournal::Record, so which reservations
// survive changes journaled formation offsets.  A patched build and a stock build
// diverge in replay playback.  Multiplayer is unaffected -- reservations are AI
// state on the host, not wire format.
//
// See docs/RE/EngineLimits.md for the full teardown.
// =============================================================================

// Desired capacity.  <= 60 (including 0) leaves the game stock; anything else is
// clamped into 61..127.  [LimitIncreases] ReservationPoolSize.
extern int g_reservationPoolSize;

void reservation_pool_install(uintptr_t exe_base);
void reservation_pool_uninstall();
