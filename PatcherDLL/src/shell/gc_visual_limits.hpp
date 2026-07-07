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
// Part 2 — RedParticleRenderer cache spill (why beams alone never worked):
// both Render() functions feed RedParticleRenderer::SubmitParticle, which
// batches into 15 static caches keyed by (texture, blend, flags), each capped
// at 200 particle entries.  Every pathway beam costs 3-4 entries in the SAME
// cache (one shared texture), so rendering silently stopped at ~50-66 beams
// no matter how large the beam buffer was.  We detour SubmitParticle: when
// the current cache can't fit a new logical particle (submit type 0/1), we
// spill to another cache slot with the same key — reusing an earlier spill
// cache with room, else claiming a fresh slot.  RenderAll() draws each cache
// independently, so duplicate-key caches render fine.  Overflow beyond the
// cache pool degrades to the vanilla drop behaviour.
//
// The pool size depends on the "Particle Cache Increase" patch set: it
// redirects the cache array to the DLL's 120-slot g_sCaches_storage but never
// raised SetCurrentCache's 15-slot allocation clamp — when we detect the
// redirect is live we finish the job (clamp 15 -> 120) and spill across all
// 120 slots; with the redirect disabled we spill within the exe's 15.
// =============================================================================

extern bool g_gcVisualLimitsEnabled;

void gc_visual_limits_install(uintptr_t exe_base);
void gc_visual_limits_uninstall();
