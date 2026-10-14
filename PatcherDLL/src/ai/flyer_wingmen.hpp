#pragma once

#include <stdint.h>

// =============================================================================
// Flyer wingmen, a PROTOTYPE: friendly AI flyers fly a vee on the player's
// flyer. Modtools only. BF2GameExt.log says how well each wingman holds its
// slot.
//
// Stock BF2 has no way to do this. The squad key does nothing for a flyer in
// the air, and the game's squadrons put AI flyers on their AI leader's flight
// path (docs/RE/FlyerAI.md, "Wingmen for a player's flyer").
//
// Who: the player's flyer, flying, leads. Friendly AI flyers within 300 m that
// its class lets follow it join, the nearest first: the classes its
// WingmanClass keys name, or with none any that can keep up (top speed at least
// 90% of the leader's). They join at takeoff (WingmanAuto, else the INI) and on
// the squad key, which then switches them between forming up and breaking off
// (to fly and fight as the AI will, until called back). One is let go once
// dead, landed, flown by someone else or, in formation, further than 800 m.
//
// An AI flyer that is not placed on a path flies by physics:
// EntityFlyer::UpdateIndirect runs its navigator (AILowLevel::UpdateIndirect),
// then EntityFlyer::CalculateDest turns AILowLevel::mDest into the point it
// steers at and the speed it asks for, avoiding barriers and keeping a safe
// height, and AIUtil::UpdatePhysics turns those into controls. For wingmen in
// formation only:
//
//   - UpdateIndirect is detoured: once a second it picks and lets go of
//     wingmen, every update it reads the leader's squad key, and it holds a
//     path flyer's free-flight time above 0 so it flies by physics.
//   - Its CALL to CalculateDest goes to calc_dest_mt, which swaps the AI's
//     destination for a point across and up to the wingman's slot, always ahead
//     of the wingman along the leader's heading, pushed clear of the leader,
//     at the leader's speed plus a catch-up and a slow trim
//     (flyer_wingmen_core.hpp). The stock code then avoids barriers and keeps
//     its height as for any point, and the AI's own destination goes back.
//   - GetFlyerMaxSpeed and GetFlyerMinSpeed are detoured: a wingman may fly up
//     to 30 m/s faster than its leader and down to 20 m/s slower, whatever its
//     class allows, so it can match a faster, slower or boosting leader.
//   - DoTrick is detoured: no rolls or flips of their own, which threw wingmen
//     out of formation and into the player. They copy the leader's instead:
//     once the leader is in a trick (flags +0x5F4 & 3 by the next update, else
//     it was refused), each wingman in formation does the trick it did
//     (mLastTrickPerformed +0x5E4) in slot order, kTrickRipple apart, a side
//     roll the same way (mSideRoll.x +0x5D8, which the game sets from the
//     strafe key before DoTrick). A copy calls past the stand-in and is free:
//     the energy (+0x61C) and the spent-out bit (+0x620 bit 0, which makes
//     SpendEnergy refuse) are put back round it. A class with no trick costs
//     still does none, and one with no double-tap cost rolls for a flip.
//     From the leader's trick until its own copy has ended and settled, a
//     wingman flies straight off its nose (a side roll is a sideways dodge: it
//     would otherwise turn for the slot the leader's dodge moved, then dodge
//     too). A flip turns the leader round, so the formation is then mirrored,
//     left for right, and each wingman falls back on the side it came out on.
//
// Real flight on paths (flyer_real_flight.hpp) rides on the same hooks and ODF
// listener: a flyer not in formation goes to it from the UpdateIndirect detour
// and the CalculateDest stand-in. Avoidance (flyer_avoidance.hpp) runs in the
// same stand-in, after the steering, for every AI flyer.
//
// AI squadrons (FlyerRealFlightSquadrons): the player's formation is one of up
// to 33. The game's squadrons ride splines, so a flyer on real flight never
// joins one; its leaders gather their own by the game's rule
// (EntityFlyer::UpdatePathFollower, CreateSquadronFromClosestEntities), flown as
// the player's wingmen are:
//
//   - A free AI flyer on real flight, on a path and not chasing counts its
//     mSquadronFormationTime (+0x1CE8) down, and on 0 sets it to 7 +- 1.5 s and
//     tries: a FlyerVsFlyer class (its path class +0x78) with SquadronFormation
//     names (its EntitySquadronClass at +0x6AC: the names, their count +0x28)
//     takes the same-side AI flyers on real flight within its reach (the
//     class's +0x1108, else its model's bounding radius times 30, squared) and
//     heading within its angle (+0x1104, a cosine), the best placed first, up to
//     its path's MaxSquadSize (+0x84), as many as one of its shapes holds.
//   - The shape is the map's EntityFormation of that name
//     (EntityFormation::Find): its points from its root node, the leader at the
//     root (or the first other point when the root is no slot), flown as drawn
//     (the game negates a path point's z on load, so it flies every formation
//     with its leader at the back), turned back by its own rotation on a map whose
//     formations were placed turned (decided per map: whichever way more of them
//     are led from the front), scaled until no two flyers are nearer than 22 m.
//     A class with WingmanSlot lines and no SquadronFormation flies those.
//   - A wingman turned on by an attacker hands it to its leader, which chases it
//     with the squadron behind it. The player may call wingmen away from an AI
//     squadron, never its leader.
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
//   GetFlyerMaxSpeed / MinSpeed     0x004F0950 / 0x004F0A10, thiscall(), ST0;
//                                   the flyer's own update and RecalculateSpeed
//                                   take its speeds from them
//   EntityFlyer::DoTrick            0x004F3D10, thiscall(Trick), RET 4; 6 is
//                                   SIDE_ROLL, the others flips
//   EntityFlyer (from the entity)   mCtrl +0x308 (Controllable +0xC8), mState
//                                   +0x5A4 (2 flying), flags +0x5F4 (8 flies
//                                   paths, 0x10 sliding round a barrier, 3 a
//                                   trick), mFreeFlyTime +0x1CDC (GetFollowingPath
//                                   0x004D7960 places it on a path only while
//                                   that is not above 0); the Controllable's
//                                   squad-key trigger +0x60, 3 when pressed
//                                   (EntityFlyer::Update reads it only landed,
//                                   0x004FE4AD)
//   AILowLevel::mDest               UnitController +0x2C4 +0x1C, a
//                                   ControllableDestination (0x34 bytes):
//                                   mHasDest +0, mPos +4, mOriginalPos +0x10,
//                                   mDirection +0x1C, mSafeHeightEnforce +0x28
//                                   (SH_FULL 0), mTaperFactor +0x2C, mMaxSpeed
//                                   +0x30, a share of the top speed
//                                   (AIUtil::GetTargetSpeedForRealSpeed)
//   a flyer's class                 GameObject +0x8; its mId +0x18, the PblHash
//                                   of its ODF name (EntityClass::Read looks a
//                                   ClassParent up by it, 0x004D08F0)
//   GameObject                      IsRtti vptr +0x00, GetVelocity +0x44, the
//                                   matrix from its +0xC interface, vptr +0x40;
//                                   alive +0x1FC bit 3, handle id +0x204, team
//                                   the low 4 bits of +0x234
//   the player's flyer              NetGame::GetLocalPlayer(0)->mVehicle
//                                   (Character +0x14C), its Trackable (+0x18)
//                                   vptr +0x20
//   the AI flyers                   sAICtrls (game_addrs ai_controller_list),
//                                   as ai_squads.cpp walks it
//   the squadron rule               UpdatePathFollower 0x004F30C0 (the time
//                                   +0x1CE8 at +0x134, the reach at +0x153 and
//                                   +0x17A, the angle at +0x1E6),
//                                   CreateSquadronFromClosestEntities 0x005E8F70,
//                                   SelectFormation 0x005EDC20
//   EntityFormation                 Find 0x005E2EB0 (cdecl(name)), walking the
//                                   list from 0x00AD33C0; its name +0x14, its
//                                   nodes +0x18 (0x1C bytes each: the position,
//                                   then the rotation x, y, z, s at +0xC), their
//                                   count +0x1C, the root's +0x28, +0x2C bit 0
//                                   the root is a slot; Read leaves the points
//                                   relative to the root (0x005E2CD4); every
//                                   node's z negated on load (0x00820470)
//
// AI flies on the host and in single player only; the leader is the host's
// own player.
//
// ODF keys on a flyer class, over the INI key by key, and a class that sets
// any has wingmen whatever FlyerWingmen says (flyer_wingmen_core.hpp):
// WingmanClass, WingmanCount, WingmanAuto, and WingmanSlot "x y z [class ...]"
// lines, written as a command vehicle's SpawnPointLocation lines are, for its
// own formation in place of the vee; a slot naming classes is kept for those
// of them that may follow. Without WingmanSlot lines the flight flies one of the
// map's formation shapes, as drawn: the first the class's WingmanFormation key
// names that the map has, else the first of its SquadronFormation shapes that
// holds all its wingmen (else the largest); else the vee. The free slots fill
// in order, each with the nearest flyer that may take it.
//
// INI: [AI] FlyerWingmen=1 gives every flyer
// wingmen, FlyerWingmenCount=3 (0 to 8), FlyerWingmenSpacing=25 (metres, 8 to
// 60, the vee's), FlyerWingmenAuto=1 (join at takeoff; 0: on the squad key
// only); FlyerRealFlightSquadrons=0 for no AI squadrons (on by default, with
// FlyerRealFlight).
// =============================================================================

extern bool  g_flyerWingmen;
extern int   g_flyerWingmenCount;
extern float g_flyerWingmenSpacing;
extern bool  g_flyerWingmenAuto;
extern bool  g_flyerRealFlightSquadrons;

void flyer_wingmen_install(uintptr_t exe_base);
void flyer_wingmen_uninstall();
