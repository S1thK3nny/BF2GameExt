#pragma once

#include <stdint.h>

// =============================================================================
// PropGenerator (terrain foliage props) fixes.
//
// Stale layer LOD across maps. Each of the 4 prop layers owns a RedLodData* at
// +0x90, set only when a map's prp_ chunk defines that layer. PropGenerator::
// Cleanup resets the layer's mesh count but leaves +0x90 pointing into the old
// map's freed memory. When the next map paints that layer in its foliage mask
// without defining it, PropCluster picks the stale pointer as its LOD data and
// the renderer crashes in RedLodManager (AV READ 0 at Steam 0x7174A4), usually
// on the first spawn.
//
// Foliage without a terrain grid. PropGenerator::Read sizes the cluster table
// from RedTerrain::_fMetersPerPoint, which only the terrain INFO block sets. A
// map munged with FOLG but no INFO leaves it 0 (or stale), 100/0 converts to
// 0x80000000 clusters, and PropGenerator::Cleanup reads cluster[-1] as NULL
// when the map is left (AV READ 0x48 at Steam 0x629C14). SetupClusters is now
// skipped unless this map loaded INFO (RedTerrain::_bEnable) with a positive
// scale; props are left off, like a map without foliage, and the log says why.
//
// Always on. This is a crash fix, not a feature.
// =============================================================================

void prop_generator_fix_install(uintptr_t exe_base);
void prop_generator_fix_uninstall();
