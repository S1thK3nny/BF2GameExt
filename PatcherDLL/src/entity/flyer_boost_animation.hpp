#pragma once

#include <stdint.h>

// =============================================================================
// Flyer Boost Animation
//
// If a flyer's AnimationName bank contains an animation named "boost", it
// will automatically play when boosting — transitioning in smoothly and
// reversing out when boost ends.  Same convention as "takeoff" and "fins".
//
// Frame 0 = normal flying pose.  Final frame = full boost pose.
// =============================================================================

void flyer_boost_anim_install(uintptr_t exe_base);
void flyer_boost_anim_uninstall();
void flyer_boost_anim_reset();

bool flyer_boost_anim_render_prepare(char* structBase);
void flyer_boost_anim_render_restore(char* structBase);
