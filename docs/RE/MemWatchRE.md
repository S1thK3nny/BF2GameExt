# memwatch — Dynamic RE & Labeling Workflow

`memwatch` is a console command (in `PatcherDLL/src/debug_commands/mem_watch.cpp`) that
arms x86 **hardware data breakpoints** (DR0–DR3) on any memory address and records every
distinct instruction that accesses it, with a value/register snapshot and a best-effort
call stack. It is a **dynamic cross-reference finder**: unlike Ghidra's static xrefs, it
catches accesses made through computed pointers, vtables, and jump tables that static
analysis misses. We used it to prove `Team::mAggressiveness` is a dead field.

All reported addresses are **unrelocated** (imagebase `0x400000`) so they paste straight
into Ghidra.

## Command reference

```
memwatch [u]<hexaddr> [len 1|2|4] [r|w|rw]   arm a watchpoint (repeat for up to 4)
memwatch                                     report all hits + disarm everything
memwatch clear                               disarm silently (no report)
```

- **`u` prefix** — address is an unrelocated Ghidra address; it is auto-rebased to the
  running module. So to watch a global `DAT_00B9A3F0`, type `memwatch uB9A3F0`.
  Without `u`, the address is treated as an absolute runtime address (e.g. a heap object
  field whose pointer you printed earlier).
- **len** — watch 1/2/4 bytes. Address must be aligned to `len`. Default 4.
- **mode** — x86 has **no read-only** breakpoint. `r`/`rw` both arm read+write (`0b11`);
  `w` arms write-only (`0b01`). To isolate *pure readers*, see Workflow B.

### Report fields (per distinct accessor, captured on first sighting)
- `EIP(unreloc)` — instruction **after** the access (data bp is a trap; subtract the
  accessor instruction's length to land on it in Ghidra).
- `hits` / `DRn` — how many times it fired and which watchpoint matched.
- `val@addr` — the dword at the watched address at that moment.
- `EAX..EDI` — register snapshot (the base pointer / index / value often lives here).
- `callers(unreloc)` — heuristic call stack: stack words that point just past a `CALL`.
  Reliable enough to identify the calling subsystem; can include false positives because
  the optimized binary omits frame pointers (we scan ESP rather than walk EBP).

## Workflows

### A. Identify an unknown `DAT_` global
1. In Ghidra, note the global's address, e.g. `DAT_00B9A3F0`.
2. In game: `memwatch uB9A3F0`.
3. Perform **one** specific action (fire a weapon, capture a CP, open the map).
4. `memwatch` → read the accessor list. Map each `EIP`/caller into Ghidra.
5. "Written once at level load, read every frame by `RenderX`" effectively labels it.
   Rename the `DAT_` and the functions accordingly.

### B. Separate readers from writers
x86 can't tag a single hit as read vs write. Do two passes:
1. `memwatch uXXXXXX w` (write-only), exercise, `memwatch` → the **writers**.
2. `memwatch uXXXXXX` (read+write), exercise the same way, `memwatch` → **all** accessors.
3. EIPs present only in pass 2 are the **readers**.

### C. Confirm or kill a static hypothesis
Think `FUN_00653320` reads some field? Watch the field, trigger the relevant action, and
check whether that EIP appears. (This is how aggressiveness was proven dead: only the
setter ever fired.)

### D. Map a struct field's lifecycle
Watch `object+offset` (absolute runtime address) on a live object. Watch it get
constructed, copied (network sync / serialization), read by logic, and freed. The set of
distinct accessors outlines the entire subsystem that touches that field.

## Tips & limits
- Only **4** addresses at once (DR0–DR3). Arm them one at a time, then a bare `memwatch`
  reports and clears all of them.
- The watchpoint covers **all threads** of the process.
- For globals, prefer the `u` form so you never have to compute the runtime base.
- A hit that fires thousands of times = a hot read/write (e.g. per-frame). A hit that
  fires a handful of times = init / teardown / event. The count is a strong clue.
- `val@addr` + register snapshot are sampled on the **first** hit for each EIP — a
  representative sample, not a full trace.
- If a watched address never fires across the action you expect to use it, that is itself
  a finding (the field/global is not involved in that action).

## Where it lives
- Implementation: `PatcherDLL/src/debug_commands/mem_watch.cpp` / `mem_watch.hpp`
- Registered in `PatcherDLL/src/debug_commands/command_registry.cpp`
- Windows-x86 only (uses x86 debug registers + a vectored exception handler). It cannot
  attach to the PowerPC Mac builds opened in other Ghidra instances.
