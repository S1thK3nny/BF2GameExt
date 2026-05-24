#pragma once

#include <stdint.h>

// =============================================================================
// MemWatch
//
// Generic x86 hardware-data-breakpoint console command. Watch up to 4 absolute
// memory addresses (DR0..DR3) and log the instruction pointer of every distinct
// thing that accesses them — across all process threads. Use it to answer
// "does anything actually read/write this field/struct/global?" without static
// guesswork.
//
// Console usage (~ console):
//   memwatch <hexaddr> [len] [r|w|rw]
//        Arm a watchpoint. len = 1|2|4 bytes (default 4). Mode default rw.
//        NOTE: x86 has no read-only breakpoint; 'r' is treated as 'rw'. Use 'w'
//        (write-only) and diff against a 'rw' run to isolate pure reads.
//        addr must be aligned to len. Repeat to arm more (up to 4).
//   memwatch              -> print collected accessor EIPs and DISARM everything.
//   memwatch clear        -> disarm everything without reporting.
//
// Reported EIPs are unrelocated (imagebase 0x400000) for direct Ghidra lookup,
// and point to the instruction AFTER the access (a data bp is a trap).
// =============================================================================

class MemWatch {
public:
   static void install(uintptr_t exe_base);   // resolve + install VEH
   static void lateInit();                     // register console command
   static void uninstall();
};
