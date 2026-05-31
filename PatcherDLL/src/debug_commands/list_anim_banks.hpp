#pragma once

#include <stdint.h>

// =============================================================================
// ListAnimBanks — console debug command
//
// Enumerates every RedAnimation "animation bank" currently registered in the
// engine's global anim hash table (PblHashTableCode<RedAnimation> at
// 0x00D5B9E4, 0x800 slots) and prints, to the game log / console:
//   - one line per loaded bank: its name (e.g. "human_0", "human_rifle"),
//     name hash, and whether its ZephyrAnimBank data is actually resident;
//   - the total number of banks loaded right now;
//   - the number of DISTINCT bank names (sub-banks like human_0..human_5
//     collapsed to their root "human").
//
// Names: the engine only stores a RedAnimation's name string (at +0x20) when
// the g_bDumpGraphicsMemoryUsage flag is set during Read{Zaa,Zaf}. We do NOT
// set that flag globally — state-cleanup code (FUN_00450090) also reads it and
// would spam Dump{Texture,Model,Animation}MemUsage on every transition. So we
// hook the two bank loaders and enable the flag ONLY for the duration of those
// calls: names get stored, the dump sites always see it as 0, nothing dumps.
//
// Console usage (~ console):
//   listanimbanks            -> list every loaded bank + totals
//   listanimbanks names      -> list only the distinct root names + counts
// =============================================================================

class ListAnimBanks {
public:
   static void install(uintptr_t exe_base);   // resolve ptrs + enable name capture
   static void lateInit();                     // register console command
   static void uninstall();
};
