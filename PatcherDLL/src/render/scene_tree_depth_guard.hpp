#pragma once

#include <stdint.h>

// =============================================================================
// Static world tree depth guard.
//
// At the end of every level load the engine turns every active RedSceneObject
// into a quadtree (`RedScene::SetupStaticWorld` -> RedSceneAABox subdivide). A
// node holding more than ten objects allocates four children, hands each object
// whose radius is under three quarters of the node's largest half extent to the
// quadrant picked by comparing its x and z against the MEAN x and z of the
// node's objects, keeps the rest, and recurses.
//
// It never checks that the split separated anything, and it has no depth limit.
// So any object set where every object lands in the same child reproduces the
// parent exactly, and the recursion runs until the stack is gone:
//
//     EXCEPTION C00000FD STACK_OVERFLOW
//     frames (EBP chain): 32 x BattlefrontII.exe+0x2E3C5B
//
// Reaching that state needs content the splitter cannot separate. The case seen
// in the wild is a NaN: three animated command posts on a mod map came out of
// EntityGeometry's constructor with a NaN render radius, EntityBuildingAnimated
// ::Init divided by it and turned their positions into NaN too, and from then
// on every comparison in the split was unordered, so all 294 objects funnelled
// into quadrant 3 forever. Coincident objects and stacks at a single x/z do it
// as well, because the split ignores y entirely.
//
// This guard makes the last step impossible whatever the content does. It
// counts recursion depth and, past the cap, forces the engine down its own
// "this node holds ten or fewer objects" path, which leaves the node as a leaf
// holding its objects in a flat list. Nothing below the cap changes: a real map
// reaches depth five, and the deepest a terminating split can legitimately go
// is bounded by the object count, so a node that is still splitting at 32 is
// not making meaningful progress. The cost when it trips is slightly coarser
// culling for the objects in one node; the alternative is losing the level.
//
// Always on. This is a crash fix, not a feature.
// =============================================================================

void scene_tree_depth_guard_install(uintptr_t exe_base);
