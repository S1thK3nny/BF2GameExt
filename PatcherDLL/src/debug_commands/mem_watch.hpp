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
//   memwatch [u]<hexaddr> [len] [r|w|rw]
//        Arm a watchpoint. len = 1|2|4 bytes (default 4). Mode default rw.
//        Leading 'u' => address is UNRELOCATED (Ghidra imagebase 0x400000) and
//        is auto-rebased to the runtime module (e.g. 'memwatch uB9A3F0' to watch
//        a DAT_00B9A3F0 global). Without 'u', addr is an absolute runtime address.
//        NOTE: x86 has no read-only breakpoint; 'r' is treated as 'rw'. Use 'w'
//        (write-only) and diff against a 'rw' run to isolate pure reads.
//        addr must be aligned to len. Repeat to arm more (up to 4).
//   memwatch              -> print collected accessors and DISARM everything.
//   memwatch clear        -> disarm everything without reporting.
//
// For each distinct accessor the report shows (captured on first sighting):
//   - EIP (unrelocated, imagebase 0x400000) — the instruction AFTER the access
//   - hit count and which DR matched
//   - the dword value at the watched address
//   - EAX/ECX/EDX/ESI/EDI register snapshot
//   - a best-effort call stack (unrelocated return addresses)
// All addresses are unrelocated for direct Ghidra lookup.
// =============================================================================

class MemWatch {
public:
   static void install(uintptr_t exe_base);   // resolve + install VEH
   static void lateInit();                     // register console command
   static void uninstall();
};
