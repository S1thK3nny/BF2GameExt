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
// The fix has two parts:
//
// Part 1 — enlarge the buffers (beams 64 -> 256 [255 on Steam, the limit
// there is a CMP imm8], particles 128 -> 512):
//   - modtools: Detours hooks on the two Add() functions (reimplemented with
//     new limits) + patches of every count-field disp32 in the Render()
//     functions and PostLoadHack, plus the two operator-new alloc sizes.
//   - Steam: pure byte patches (the release-build Add functions use LTCG
//     register conventions, so the limit immediates are patched in place
//     instead of detouring).
//
// Part 2 — RedParticleRenderer cache spill: both Render() functions feed
// RedParticleRenderer::SubmitParticle, which batches into static caches keyed
// by (texture, blend, flags), each capped at 200 entries.  Every pathway beam
// costs 3-4 entries in the SAME cache (one shared texture), so rendering
// silently stopped at ~50-66 beams no matter how large the beam buffer was.
//
// That half now lives in render/particle_batch_spill.cpp and installs
// independently of this feature, because it is not a galaxy-map fix at all —
// it governs every particle in the game.  It used to sit behind this file's
// INI key and its verify_and_apply() gate, so turning GC limits off, or any
// one of ~16 unrelated galaxy-map sites failing to verify, silently took the
// world particle batching down with it.  GC is a consumer of that module now.
//
// =============================================================================

extern bool g_gcVisualLimitsEnabled;

void gc_visual_limits_install(uintptr_t exe_base);
void gc_visual_limits_uninstall();
