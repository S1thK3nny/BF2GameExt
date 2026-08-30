#pragma once

#include <stdint.h>

// Diagnostic only -- see branch_region_debug.cpp. Narrates region-factory
// dispatch, BranchRegion creation and BranchRegion lookup into the engine log,
// interleaved with the engine's own "Unable to find branch region" warnings so
// the ordering between them is visible. Changes no behaviour.
//
// [Diagnostic] BranchRegionDebug, default off. modtools only.

extern bool g_branchRegionDebugEnabled;

void branch_region_debug_install(uintptr_t exe_base);
void branch_region_debug_uninstall();
