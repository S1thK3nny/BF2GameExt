#pragma once

#include <stdint.h>

// =============================================================================
// Flyer-to-flyer avoidance, a PROTOTYPE: every AI flyer the AI flies by physics
// steers clear of every other flyer in the air, AI or human. Modtools only.
// BF2GameExt.log says how often it steered.
//
// Stock, an AI flyer avoids barriers, danger regions and the ground
// (EntityFlyer::CalculateDest and RaiseHeight), never another flyer, and
// RaiseHeight's obstacle ray treats a flyer ahead as a building to climb over
// at 20 m/s. Here, for every AI flyer CalculateDest steers (free-flying AI,
// flyers on real flight, chasing ones, wingmen):
//
//   - Once a frame, every flyer taking off, flying or landing is read from the
//     list every EntityPathFollower joins (one in every EntityFlyer): where it
//     is, its velocity, its collision sphere's radius.
//   - After CalculateDest, the wingmen's stand-in (flyer_wingmen.cpp) calls
//     adjust: any flyer that will pass within the two radii plus 8 m inside 2 s
//     bends the point it steers at away, square to its heading, the harder the
//     nearer and the sooner, and a slower one ahead going its way eases its
//     speed (flyer_avoidance_core.hpp). A point CalculateDest lifted over the
//     terrain or an obstacle is never bent lower.
//   - RaiseHeight's ray passes over flyers (flyer_real_flight.cpp casts it
//     again with each flyer it hits ignored), so a flyer ahead no longer makes
//     it climb and brake: the avoidance keeps it clear instead.
//
// Placed flyers (on their paths' curves, not on real flight) are not steered
// at all and keep the stock behaviour.
//
// READ ON MODTOOLS (Steam and GOG: not yet):
//
//   the follower list      head node 0x00AD3524 (game_addrs flyer_follower_list):
//                          +4 the first node; a node's +4 the next, +0xC its
//                          follower; the head's +0xC null; linked in by the
//                          EntityPathFollower constructor (0x005E8AE3), which
//                          only EntityFlyer's constructor calls, with the
//                          follower at the entity's +0x3CC
//   a flyer's sphere       its TreeGridObject (entity +0xC): the stack +0x10
//                          (when set, the radius at stack + index * 16 + 0x4C,
//                          the index at +0x14), else its own radius at +0x24,
//                          as RaiseHeight reads them (0x004F4259)
//   EntityFlyer            mState +0x5A4 (1 taking off, 2 flying, 3 landing);
//                          the rest as flyer_wingmen.hpp
//   ControllableDestination as CalculateDest leaves its copy: mPos +4 (the
//                          point before RaiseHeight lifts it)
//
// AI flies on the host and in single player only.
//
// INI: [AI] FlyerAvoidance=1 (off by default).
// =============================================================================

extern bool g_flyerAvoidance;

namespace flyer_avoidance {

// From the wingmen's install, during the installers' writable window.
bool install(uintptr_t exe_base);
void uninstall();

// Whether avoidance runs: then RaiseHeight's ray passes over flyers too.
bool active();

// After EntityFlyer::CalculateDest steered an AI flyer, from the wingmen's
// stand-in: `point` bent away from flyers it would pass too close to, `speed`
// (the target, 0 to 1) eased behind a slower one. `dest` is the
// ControllableDestination CalculateDest copied.
void adjust(uint8_t* flyer, const uint8_t* dest, float* point, float* speed);

} // namespace flyer_avoidance
