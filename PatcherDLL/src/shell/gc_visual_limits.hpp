#pragma once

#include <stdint.h>

// =============================================================================
// Galactic Conquest visual limit extensions
//
// The GC galaxy map uses two fixed-size per-frame buffers for rendering:
//
//   DrawAllBeamBetween  — pathway lines between planets (vanilla: 64 max)
//   DrawAllParticleAt   — planet halos, fleet icons, etc. (vanilla: 128 max)
//
// Both buffers silently drop entries when full.  With modded GC scenarios
// that exceed 13 planets, the limits cause:
//   1. Pathways stop appearing (>64 edges in the connectivity graph)
//   2. Fleet icons vanish (particles fill with planet halos first)
//   3. Planet highlights vanish (too many planets for even the halos)
//
// This module raises both limits via:
//   - Detours hooks on the two Add() functions (reimplemented with new limits)
//   - Runtime in-memory patches on the Render() functions and PostLoadHack
//     to update all count-field displacement values to match the larger arrays
//
// New limits:
//   Beams:     64  -> 256
//   Particles: 128 -> 512
//
// TODO: The beam limit extension is experimental and might not fully work yet.
//       The particle limit is confirmed working. The beam Add hook and Render
//       displacement patches are in place, but users report no visible increase
//       in rendered pathways — there may be an additional limit elsewhere.
// =============================================================================

void gc_visual_limits_install(uintptr_t exe_base);
void gc_visual_limits_uninstall();
