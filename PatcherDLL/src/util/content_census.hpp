#pragma once

#include <stdint.h>

// =============================================================================
// Content budget report.
//
// A census of the things a MODDER AUTHORS, printed as occupancy against the
// ceiling -- because in BF2 those ceilings are invisible until you cross one,
// and crossing one is usually silent.  This is deliberately NOT a runtime
// profiler: voices, AI reservations and the particle caches are engine state
// the author does not control, so they are out of scope.
//
// The framing is "report against every ceiling BF2GameExt already knows how to
// raise", which makes the ceiling half nearly free -- we located those limits in
// order to patch them.
//
// TWO RULES THIS FILE FOLLOWS, both learned the hard way in this project:
//
//   1. WHERE A TABLE CAN BE SCANNED, SCAN IT, and report the scan ALONGSIDE the
//      engine's own counter.  Trusting a stored counter is how BranchRegionDebug
//      printed a hardcoded zero for its entire life -- it read `_head._pObject`
//      believing it was `_iCount`.  Two numbers that agree are evidence; one
//      number is a hope.  When they disagree the report says so rather than
//      picking a winner.
//
//   2. NOTHING IS REPORTED THAT WAS NOT VERIFIED PER BUILD.  A metric with no
//      address for the active build is omitted, not guessed.
//
// Triggers, in order of importance:
//   * Periodic, every [Diagnostic] ContentCensus seconds.  Runs on its own
//     thread, so it needs no per-frame hook and works identically on all three
//     builds -- which matters because the retail builds have no debug console.
//     The reads are aligned dwords, so a torn read is not possible on x86.
//   * Lua: `GameExtContentCensus()`, callable from any mission script, on ALL
//     builds.  Lua is the mission scripting engine and is present everywhere;
//     only the `~` console is modtools-only.
//
// =============================================================================
// EFFECT CLASSES -- the headline metric, and the one worth understanding.
//
// `FLEffect::s_EffectClasses` is a PblHashTable with 256 KEY slots followed by
// 256 value slots.  One slot is consumed per DISTINCT effect class name; reuse
// is free, because the registrar checks for an existing key first and the
// underlying _Store refuses a duplicate outright.  In practice that works out at
// one slot per loaded `.fx` file.
//
// It matters because the table has NO CAPACITY GUARD.  `FLEffect::Read` does
// `_Find` (duplicate check) then `_Store` then `_iNumEntries++`, with no compare
// against 256 anywhere.  And `PblHashTableCode::_Find` is an open-addressing
// probe with NO ITERATION CAP whose only exits are "key matches" and "slot is
// zero" -- so on a table with no zero slots, a lookup for an absent key never
// returns.  The consequences:
//
//   * Registering a 257th distinct class HANGS DURING LEVEL LOAD, inside the
//     duplicate-check `_Find` rather than inside `_Store`.
//   * A level that lands on exactly 256 loads cleanly with zero free slots, and
//     then the first RUNTIME lookup of an unregistered name hangs -- and those
//     lookups happen on firing a shot, taking damage, deflecting a bolt.
//
// Either way the symptom is a frozen main thread with audio still playing, so
// this number is worth watching well before it reaches the ceiling.
//
// The counter lives at the key array minus 4 (`{int _iNumEntries; uint
// _uiTable[512];}`), which is why the census names both addresses separately.
// =============================================================================

// Seconds between automatic reports. 0 disables the periodic thread entirely;
// the Lua entry point still works. [Diagnostic] ContentCensus.
extern int g_contentCensusInterval;

// Append a per-class listing -- ODF name, base class, id -- to the report.
// Fires only when the entity class count has CHANGED since the last listing,
// so it costs one block per level load rather than one per tick.
// [Diagnostic] ContentCensusNames.
extern bool g_contentCensusNames;

// Build and write one report to the log. Safe to call from any thread.
void content_census_report();

void content_census_install(uintptr_t exe_base);
void content_census_uninstall();
