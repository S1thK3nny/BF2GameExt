#pragma once

#include <stdint.h>

// =============================================================================
// Walker footsteps go silent at high frame rates.
//
// With StompDetectionType = 1 (stock AT-AT, AT-TE, spider walker) the engine
// detects a step from how far each foot fell since the previous update:
//
//   drop = lastHeight - height
//   if (!latched) { if (drop < 0.1) { step; latched = true; } }
//   else if (drop > 0.1) latched = false;
//
// The 0.1 is metres per FRAME, so re-arming needs the foot to fall faster than
// 0.1 * fps m/s: 3 m/s at 30 fps, 14.4 m/s at 144 fps. At high frame rates a
// foot never re-arms, so a walker plays one step per foot and then goes quiet
// (boost speeds the anim up, which is why it sometimes still works).
//
// The fix points that compare at a threshold of 3 m/s * dt, which keeps 30 fps
// exactly stock and makes every other frame rate behave the same.
//
// All three builds.
// =============================================================================

void walker_footstep_fix_install(uintptr_t exe_base);
void walker_footstep_fix_uninstall();
