#pragma once

#include <stdint.h>

// =============================================================================
// Flyer wingmen, a PROTOTYPE: friendly AI flyers near the player's flyer fly in
// a vee on it. Modtools only, off unless the INI turns it on, no orders and no
// ODF keys yet. Its question is whether the game's own flight code can hold a
// slot at fighter speed; BF2GameExt.log says how well each wingman holds his.
//
// Stock BF2 has no way to do this. The squad key does nothing for a flyer in
// the air, and the game's squadrons put AI flyers on their AI leader's flight
// path (docs/RE/FlyerAI.md, "Wingmen for a player's flyer").
//
// An AI flyer that is not placed on a path flies by physics:
// EntityFlyer::UpdateIndirect runs its navigator (AILowLevel::UpdateIndirect),
// then EntityFlyer::CalculateDest turns AILowLevel::mDest into the point it
// steers at and the speed it asks for, avoiding barriers and keeping a safe
// height, and AIUtil::UpdatePhysics turns those into controls. Two places
// change, for wingmen only:
//
//   - UpdateIndirect is detoured. Once a second it picks the wingmen: the
//     player's flyer, flying, leads; the nearest friendly AI flyers within
//     300 m that are flying and can keep up (top speed at least 90% of the
//     leader's) join, and one is let go once dead, landed, further than 800 m
//     or flown by someone else. A wingman that flies paths (space maps) has
//     its free-flight time held above 0, so it flies by physics instead.
//   - Its CALL to CalculateDest goes to calc_dest_mt. For a wingman the AI's
//     destination is swapped for its slot, carried on half a second by the
//     leader's velocity, facing the leader's way, at the leader's speed plus
//     0.5 m/s for every metre the slot is ahead (flyer_wingmen_core.hpp); the
//     stock code then avoids barriers and keeps its height as for any point,
//     and the AI's own destination goes back afterwards. Round a turn it keeps
//     most of that speed, where the stock code would all but stop it.
//
// READ ON MODTOOLS (Steam and GOG: not yet):
//
//   EntityFlyer::UpdateIndirect     0x004F4C10, thiscall(float dt) on the
//                                   Controllable (entity +0x240), RET 4
//   its CALL to CalculateDest       0x004F4D4D (thunk 0x00401726)
//   EntityFlyer::CalculateDest      0x004F4630, thiscall(dt, dest*, point*,
//                                   speed*, myPos*) on the entity, RET 0x14;
//                                   reads UnitControllerPtr(mCtrl) + 0x2C4 +
//                                   0x1C, copies it out, steers at its mPos
//   GetFlyerMaxSpeed / MinSpeed     0x004F0950 / 0x004F0A10, thiscall(), ST0
//   EntityFlyer (from the entity)   mCtrl +0x308 (Controllable +0xC8), mState
//                                   +0x5A4 (2 flying), flags +0x5F4 (8 flies
//                                   paths, 0x10 sliding round a barrier, 3 a
//                                   trick), mFreeFlyTime +0x1CDC (GetFollowingPath
//                                   0x004D7960 places it on a path only while
//                                   that is not above 0)
//   AILowLevel::mDest               UnitController +0x2C4 +0x1C, a
//                                   ControllableDestination (0x34 bytes):
//                                   mHasDest +0, mPos +4, mOriginalPos +0x10,
//                                   mDirection +0x1C, mSafeHeightEnforce +0x28
//                                   (SH_FULL 0), mTaperFactor +0x2C, mMaxSpeed
//                                   +0x30, a share of the top speed
//                                   (AIUtil::GetTargetSpeedForRealSpeed)
//   GameObject                      IsRtti vptr +0x00, GetVelocity +0x44, the
//                                   matrix from its +0xC interface, vptr +0x40;
//                                   alive +0x1FC bit 3, handle id +0x204, team
//                                   the low 4 bits of +0x234
//   the player's flyer              NetGame::GetLocalPlayer(0)->mVehicle
//                                   (Character +0x14C), its Trackable (+0x18)
//                                   vptr +0x20
//   the AI flyers                   sAICtrls (game_addrs ai_controller_list),
//                                   as ai_squads.cpp walks it
//
// AI flies on the host and in single player only; the leader is the host's
// own player.
//
// INI (unregistered while a prototype): [AI] FlyerWingmen=1 turns it on,
// FlyerWingmenCount=3 (1 to 4), FlyerWingmenSpacing=25 (metres, 8 to 60).
// =============================================================================

extern bool  g_flyerWingmen;
extern int   g_flyerWingmenCount;
extern float g_flyerWingmenSpacing;

void flyer_wingmen_install(uintptr_t exe_base);
void flyer_wingmen_uninstall();
