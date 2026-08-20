#pragma once

#include <stdint.h>

// =============================================================================
// RedParticleRenderer batch-cache pool — slot-count lift and cache spill.
//
// The renderer batches particles into a static array of caches keyed by
// (texture hash, blend mode, render flags).  Stock: 15 caches, 200 particle
// entries each.  Two independent walls follow from that:
//
//   * the 16th distinct key finds no free slot, so SetCurrentCache stores NULL
//     into currentCache and EVERY SubmitParticle in the game silently no-ops
//     until the next RenderAllCaches.  Not thinning — a blackout.
//   * a cache that reaches 200 entries refuses the rest, and (before the
//     Particle Effect Skip Fix) took the whole emitter down with it.
//
// The "Particle Cache Increase" patch set redirects the cache array to the
// DLL's 120-slot g_sCaches_storage, but the base redirect alone leaves
// SetCurrentCache's allocation clamp at 15, so the extra slots are never
// handed out.  This module finishes the job: it lifts that clamp to match the
// live buffer and detours SubmitParticle so a full cache spills into another
// slot with the same key instead of dropping.
//
// WHY THIS IS ITS OWN MODULE: all of this used to live inside
// gc_visual_limits_install(), behind BOTH the GCVisualLimits INI key AND that
// function's verify_and_apply() gate over ~16 galaxy-map patch sites.  Neither
// has anything to do with world particles, so a user who turned GC limits off
// — or any build where one unrelated galaxy-map site failed to verify — also
// silently lost the batch-cache fix for every particle in the game, with
// nothing in the log to say so.  The GC map is a consumer of this, not its
// owner.
// =============================================================================

extern bool g_particleBatchSpillEnabled;

void particle_batch_spill_install(uintptr_t exe_base);
void particle_batch_spill_uninstall();

// Live slot count: RENDERER_CACHE_SLOTS when the Particle Cache Increase
// redirect is active, else the exe's stock 15.  Reported in the GC log line.
uint32_t particle_batch_cache_slots();

// Spill counters, for whoever wants to report them.
void particle_batch_spill_stats(uint32_t& spills, uint32_t& fails);
