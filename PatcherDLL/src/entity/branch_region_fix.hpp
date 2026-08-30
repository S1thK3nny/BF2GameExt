#pragma once

#include <stdint.h>

// EntityPath branch regions, part 2 -- see branch_region_fix.cpp.
//
// Part 1 (the vtable-slot repair) lives in patch_table.cpp as the "EntityPath
// Branch Region Fix" patch set and is what makes the engine call
// BranchRegionFactory::CreateRegion at all. This half registers each region
// under the id WITHOUT the engine's stray leading space as well, so both
// BranchRegion("id") and BranchRegion(" id") resolve.
//
// Shares the [Fixes] BranchRegionFix key with the patch set. modtools only.

extern bool g_branchRegionFixEnabled;

void branch_region_fix_install(uintptr_t exe_base);
void branch_region_fix_uninstall();
