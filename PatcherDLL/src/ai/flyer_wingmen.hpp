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
// Who: the player's flyer, flying, leads. Friendly AI flyers its class lets
// follow it are called up, the nearest first, however far: the classes its
// WingmanClass keys name, or with none any that can keep up (top speed at least
// 90% of the leader's). They are called on the squad key, or at takeoff where
// WingmanAuto (else the INI, off by default: the user, "no wingman squad is
// formed until the player presses F4") says so; from then on losses are made up
// whenever they happen. One further than 150 m flies in, and has 20 s plus 1 s
// for every 20 m it started off (3 minutes at most) to reach its slot. One is let
// go once dead, landed, flown by someone else or, in its slot, further than 800 m.
//
// The player may also pick them one at a time: the squad key with a friendly AI
// flyer in the air under the reticule (Controllable::mReticuleTarget, either
// channel) takes that one into the first free slot that takes its class, if the
// flight has room and it may fly with this leader as a call-up would take it,
// else does nothing. A flight picked from empty is the player's own: it starts
// in form up and its losses are not made up. Held for a second (the trigger's
// bit 0, as EntitySoldier::Update's squad key holds to let its followers go:
// its hold time against 1.0 at 0x00A2A290), the squad key breaks the flight
// off: every wingman let go ("breakoff"), and none joins on its own, WingmanAuto
// or not, until the squad key calls a flight up again, in form up whatever it
// would start with, or picks one; a script's CallWingmen undoes it too. A
// flight the mission locked is neither picked into nor broken off.
//
// Markers (FlyerWingmenMarkers): the game's own target markers
// (TargetManager::AddMarker, as objectives', Lua's MapAddEntityMarker's and the
// comm keys' requests' are), a steady cyan hud_objective_icon_circle over each
// of the player's wingmen on the map (HUD::ElementMap::UpdateMarkerIcons) and,
// as a "common" HUD target (LockOnManager::AddTargetCommon, display only, not
// a lock), a bracket on screen in the same colour (HUD::ElementTarget), for the
// player's team, from joining to leaving; and a red one on the flight's attack
// target while it attacks one. The game holds 32 markers between them all and
// adds none past that, silently. A .hud may draw its own instead: while it binds
// a wingman's events (render/hud_wingmen.hpp), the wingmen have none of these,
// and while it binds the attack target's, the target has none.
//
// Orders (flyer_orders_core.hpp), with a line on the HUD's message line as the
// trooper squad commands show theirs:
//
//   - Form up: hold their slots.
//   - Engage: each takes on an enemy within 800 m of the leader (flyers before
//     vehicles on the ground, spread before doubling up; troops on foot are
//     left alone), chased until 1000 m from the leader, else holds its slot.
//   - Attack my target: all on the player's lock, else the enemy under its
//     reticule, as the order is given (with neither, "no target" and nothing
//     changes); chased until it is down or 3000 m from the leader, then back to
//     the order the attack interrupted.
//   - Cover me: each takes on an AI whose attack is aimed at the leader within
//     1200 m (its agent's AttackHelper target), chased until 1500 m off.
//   - Keys: the squad key calls wingmen up when there are none (or picks the
//     friendly flyer under the reticule), else switches between form up and
//     engage (from an attack or cover, form up), and held breaks the flight
//     off (above); the comm keys Attack and Defend (F11 and F12 by default)
//     order attack my target and cover me as they go down, and the comm key
//     Spotted (F5) switches the flight to its next formation: the vee, its
//     leader class's own WingmanSlot layout, and each of the map's formation
//     shapes that holds it, round (Comm_Chat_0, input 20). They are read from the player's processed
//     inputs (Comm_Chat_6 and _7): the game makes team chat of the comm keys
//     only online (RawControllerInputs::ReadLatest runs its comm-key processing
//     only in a networked game), but turns their bindings into those inputs
//     always. Online their team chat still goes out.
//   - A chasing wingman flies at its own speeds: behind a flyer in the air as
//     real flight's chase does (flyer_real_flight_core.hpp); at anything else in
//     passes, a dive at it with the safe height let go near it (SH_TAPER), out
//     short of it or once past it, on and 60 m up, back in. How far is its own,
//     from its primary weapon's ranges (channel 0's MinRange, OptimalRange,
//     MaxRange, as its AI fights) and its cruising speed (flyer_orders_core.hpp,
//     run_shape): back in from MaxRange, in through its optimal range, out just
//     before its MinRange (10% outside it), never nearer than 2.5 s of its speed;
//     behind a flyer, 7.5% of MaxRange back (50 to 200 m). Its AI's attack is
//     aimed at its target (ForceSetAttackHelperTarget, at most every 0.5 s, and
//     only while aimed elsewhere), so it fires at it, but only while it sees it:
//     a ray (the landing ray's types, so buildings, props and capital ships but
//     no vehicles) along the line to 10 m short of it, looked along every
//     0.25 s; without the line its AttackHelper's target is let go
//     (AttackHelper::Reset, ForceSetAttackHelperTarget taking no "none") and a
//     pass inside where it would turn back is broken off. A target off the
//     ground shut in from above (a ray down onto it from 500 m up meets
//     something first: a hardpoint under a hull) is passed at along its most open
//     side instead (rays in onto it from 500 m out along 14 directions, the
//     nearest clear one to the wingman), the wingman first flying out to 400 m
//     along it, round what is in the way, and pulling out along it, not up
//     (flyer_orders_core.hpp, pick_open). One further than 2500 m from its
//     leader comes back.
//
// Landing with the player (a try; the leader class's WingmanLand, else the INI's
// FlyerWingmenLand; with 0 the wingmen break off as the player lands): when the
// player's flyer lands (state 3 landing, 0 landed; a player can land only in a
// landing region, Lua AddLandingRegion), each wingman within 150 m of it lands
// right away where it is (the user: "When I land, AI should land right away"),
// where a ray down finds ground within 200 m level with the leader's and flat
// and no flyer or vehicle (COLL_RIGID, which the landing ray goes through) on
// the way down. Coming down onto a flyer or vehicle, still not down 8 s after it
// was told to land, or down on top of one, it takes off again and is given a
// clear place as below, twice at most (flyer_orders_core.hpp, land_here,
// landing_interrupted). Any other, in slot order, is given
// a place on the ground beside it (flyer_orders_core.hpp, land_tries): its
// slot's place 1.5 times out and never nearer than 15 m, else nearer, the other
// side or behind the leader, 12 m from the others' places, where a ray down
// (CollisionManager::RayHit, the flyer's own landing ray's types 0x190) finds
// ground within 8 m of the leader's and flat. The ray starts 40 m up, or 2 m
// under a roof over the leader (a ray up from the leader's ground finds one
// within 40 m: a hangar's, which a ray from 40 m up would hit before the ground);
// under a roof the place must also be in a clear line from the leader 3 m up, and
// the wingman's way to it clear, straight or along the leader's trail (below). It
// comes down toward 15 m over it (lower under
// a low roof), at its slowest (the stretch lets it go to 10 m/s) from 300 m in,
// and lands (EntityFlyer::Land) once within 25 m of it across and 40 m up
// (EntityFlyer::Land needs no landing region). Once down its pilot gets out (and
// it is let go; asked again every 2 s while still aboard, four times in all),
// unless its own class says WingmanExitOnLand = 0: then it waits
// aboard, and when the player takes off (state 1) it takes off too
// (EntityFlyer::TakeOff) and forms up again. One with no place beside the leader
// looks on ahead, the way the leader faces (16 places 12 m to its side, every
// 12 m from 15 m ahead, none past a wall), each with no flyer or vehicle on it
// (rays down at vehicles alone; the ground's go through them). With none there
// either, or up again before its leader, it circles till one is free, looking
// again every 2 s (the user: "Instead of a circle, they should move forward
// instead. If forward is not clear (wall) then circle until a free spot
// appears"): 200 m round a point 150 m over the leader, or, with the leader under
// a roof, where that would take it into the roof, low and slow at its hover
// height, 15 to 30 m round the point with most room of those over the leader and
// on ahead (flyer_orders_core.hpp, roof_loiter_radius). With no room for that it
// is let go, and under a roof one interrupted past its two retries is let go too
// ("If they cannot land reliably ... disband"); but one let go under a roof, and
// still flying, first flies back out the way the leader flew in, along its trail
// backwards (flyer_trail_core.hpp, the way out: the user, "Lots of hangars are
// single entry/exit, so if they disband it's highly likely they'll crash unless
// they do this"), and is let go once out from under the roof two looks in a row,
// or where it is if not on the trail in 2 s, stuck 6 s or still going at 45 s.
// While landed the squad key is
// the game's (seats), and the comm keys order nothing. The player getting out of
// its landed flyer changes none of this for 60 s (flyer_orders_core.hpp,
// arrival_over; the user, after the first hangar landings: "I just don't see any
// late arriving wingmen", let go as the player got out): those still on their way
// land beside it all the same and their pilots get out, and back aboard the
// player leads the rest again. Then, or with the flyer gone, flown off or
// another taken, the rest are let go, those down still asked to get out for 20 s.
// Each tick lets go of those shot down before the landing asks any where it is:
// a destroyed flyer is soon freed, and its matrix read through freed memory
// faulted twice in those landings, the guard leaving the landing half set up.
// Every guard here now says what it caught in the log.
//
// The leader's trail (FlyerWingmenTrail; flyer_trail_core.hpp): the player's
// flyer's path is kept as a point every 8 m it flies, the last 2 km, started
// again where it takes off after landing. A wingman whose line to its slot (or,
// landing, to its place) a building, prop, the ground or water blocks (a ray of
// the landing's types, four times a second) flies that path instead: from the
// point nearest it (within 300 m, in a clear line), at the furthest point up to
// 16 ahead it has a clear line to, at the speed that keeps it its spacing behind
// the leader for each wingman in front of it, and with CalculateDest's lift off
// (SH_TAPER_FACTOR with no factor: EntityFlyer::RaiseHeight would raise the
// point 25 m over whatever its ray ahead meets, buildings and vehicles, a
// hangar's whole ship, so into the roof). It copies no tricks there. Its line
// clear three looks in a row, it goes back to its slot. One that gets no further
// along it for 6 s while its place is ahead is left to its own AI for 10 s, then
// tries again. Under a roof over the leader (a ray up 40 m, twice a second) the
// flight's slots close in to half their spacing, half of it a second, and open
// out again in the open. Under a roof itself (the same ray from each wingman), or
// with the leader under one and its line clear as last looked, a wingman flies
// at its slot, its place or its target with that lift off too
// (flyer_trail_core.hpp, lift_off): in the first hangar landings two died soon
// after leaving the trail or flying to a new place inside, most likely lifted
// into the roof.
//
// Getting out: stock, an AI pilot would get out only through its fly agent's
// own landing (UnitFlyAgent's FLY state lands at an AI "land" hint node within
// 80 m tied to a command post, IsNearLandingPt and
// HintManager::RequestLandHints; landed, its DROPOFF state calls SetExitVehicle
// with that post), and that never happens: BaseHint::GetCommandPost always
// answers none, so no hint qualifies (docs/RE/HintNodes.md). Should a fix give
// hints their posts back, a wingman's agent's CALL to IsNearLandingPt still
// answers no (it lands only with its leader). Its two CALLs to TakeOff (its
// TAKEOFF and FLY states) do nothing while it is down beside its leader.
// Landed there its pilot is got out with AILowLevel::SetExitVehicle and no
// command post, as the AI's own end-of-game exit does.
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
//   the player's targets            Controllable::GetTargetLockedObj 0x005E0C70
//                                   (thiscall(), EAX; kept at +0x138); the
//                                   reticule's at Controllable +0x164 (handle
//                                   +0x168), a second channel 8 bytes on, as the
//                                   trooper squad command reads it (0x0054985F)
//   enemies                         GameObject::GetAffiliationToMe 0x0055F840
//                                   (thiscall(other), RET 4): below 0
//   an AI's attack                  UnitController +0x2C0 its agent, whose
//                                   AttackHelper (+0x10) holds the target (+0,
//                                   handle +4); ForceSetAttackHelperTarget
//                                   0x0059EC20 (thiscall(target), RET 4; its
//                                   SetVisibleTarget reads the target, so never
//                                   null); AttackHelper::Reset 0x005B4230
//                                   (thiscall() on the AttackHelper, RET), as
//                                   SetVisibleTarget calls it first
//   the message line                0x006AECD0, cdecl(player, text, color*):
//                                   player%d.message.color, then
//                                   player%d.message; the trooper commands'
//                                   colour at 0x00AE2170 (EntitySoldier
//                                   0x00549A35); localisation 0x007F3720,
//                                   cdecl(hash, nullIfMissing)
//   the squad key held              Controllable +0x60 bit 0 down (bits 0 and 1:
//                                   pressed this update); the trooper's hold
//                                   lets its followers go past 1.0 s
//                                   (EntitySoldier::Update 0x00549D07, the 1.0
//                                   at 0x00A2A290)
//   markers                         TargetManager::AddMarker 0x0048E3A0,
//                                   cdecl(object, region, icon, scale, teams,
//                                   colour, hudTarget, flash, pulse, pulseFrom),
//                                   and RemoveMarker 0x0048E650, cdecl(object,
//                                   region, teams), as Lua's MapAddEntityMarker
//                                   (0x00471D72) and MapRemoveEntityMarker
//                                   (0x00471E1C) call them; "cyan" in the
//                                   colour table (0x00ACA9A0)
//   the comm keys                   the player's ProcessedInputs, at its
//                                   PlayerController +0x88 (PlayerController::
//                                   Update 0x0059B6A2); bit n of +0x10 for
//                                   digital input n (ProcessedInputs::GetDigital
//                                   0x00743A30), Comm_Chat_6 26 and _7 27,
//                                   set every frame a key is held (Set with
//                                   last 0, 0x007459A9), a pad's button the
//                                   frame it goes down only; ReadLatest
//                                   (0x0074695D) runs ChatInputProcess, and with
//                                   it TeamOrderInputProcess 0x00745D00, only
//                                   networked
//   landing                         EntityFlyer::Land 0x004F1380 and TakeOff
//                                   0x004F8B70, thiscall() on the entity, each
//                                   perhaps detoured first (Land by the
//                                   LandOnArrival fix, TakeOff by the carrier
//                                   fixes: a JMP at either is theirs);
//                                   CollisionManager::RayHit 0x0042E230 (as
//                                   real flight's raise_ray_mt calls it)
//   the primary weapon              EntityFlyer mWeaponIndex[channel] +0x6C8,
//                                   mWeapon[4] +0x6B8 (GetWeaponIndex
//                                   0x004D7880, GetWeaponPtr 0x004D78A0);
//                                   Weapon::mClass +0x64; WeaponClass MinRange
//                                   +0x100, OptimalRange +0x104, MaxRange +0x108
//                                   (SetProperty 0x0061EB16, 0x0061F4AB,
//                                   0x0061EF3D; CombatUtil 0x005947B0)
//   waiting aboard                  UnitFlyAgent's CALL to IsNearLandingPt
//                                   0x005AFDD3 (0x005AED00, thiscall() on the
//                                   agent, AL; its UnitController +0xC), its
//                                   CALLs to TakeOff 0x005AF154, 0x005AF323;
//                                   AILowLevel::SetExitVehicle 0x005A2170
//                                   (thiscall(CommandPost*) on UnitController
//                                   +0x2C4, RET 4)
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
// in order, each with the nearest flyer that may take it. On the leader's
// class too: WingmanLand = 0 lets its wingmen go as it lands (else they land
// with it); WingmanStartOrder (formup, engage or cover) is the order its flight
// starts with when called up; WingmanEngageRange (metres, 100 to 5000) stands
// for the 800 m engaging wingmen reach out to, a foe chased a quarter further.
// On the class that flies as a wingman: WingmanExitOnLand = 0 keeps its pilot
// aboard when it lands beside its leader, to take off with it (else it gets
// out); Wingman = 0 keeps it out of every flight, the player's and the AI
// squadrons' (a script may still name it); WingmanRole = bomber sends it at
// targets on the ground before flyers in the air (fighter, the default: the
// other way round).
//
// A map or mod may word the messages its own way under the localisation keys
// game.wingmen.formup, .engage, .attack, .cover, .notarget, .none, .targetdown,
// .targetlost, .breakoff, .join and .formation (the formation's name follows it);
// else they are English.
//
// INI: [AI] FlyerWingmen=1 gives every flyer
// wingmen, FlyerWingmenCount=3 (0 to 8), FlyerWingmenSpacing=25 (metres, 8 to
// 60, the vee's), FlyerWingmenAuto=0 (on the squad key first; 1: called up at
// every takeoff); FlyerWingmenMarkers=0 for no markers; FlyerWingmenTrail=0 to keep them
// off the leader's trail; FlyerRealFlightSquadrons=0 for
// no AI squadrons (on by default, with FlyerRealFlight).
// =============================================================================

extern bool  g_flyerWingmen;
extern int   g_flyerWingmenCount;
extern float g_flyerWingmenSpacing;
extern bool  g_flyerWingmenAuto;
extern bool  g_flyerWingmenLand;
extern bool  g_flyerWingmenMarkers;
extern bool  g_flyerWingmenTrail;
extern bool  g_flyerRealFlightSquadrons;

void flyer_wingmen_install(uintptr_t exe_base);
void flyer_wingmen_uninstall();

// The mission is over (EventManager::Cleanup): its flights and whatever its
// script said go, and nothing more is fired.
void flyer_wingmen_mission_end();

// For the HUD (render/hud_wingmen.hpp): the player's wingmen, alive, in slot
// order (at most `max`), and the flight's attack target while it attacks one.
int      flyer_wingmen_hud_list(uint8_t** out, int max);
uint8_t* flyer_wingmen_hud_target();

// =============================================================================
// The player's flight from a mission script (lua/lua_wingmen.cpp turns these
// into Lua functions; docs/user/LUA_API.md). What a script says holds for the
// rest of the mission. Modtools only, like the rest: elsewhere each does
// nothing and says so (0, false, null).
//
//   - The roster: flyers a script names (AddWingman) join the player's flight
//     before any called up on their own, whatever their classes' keys say, each
//     once it is a friendly AI flyer in the air, from anywhere. Named by a world
//     object's name, it is whatever carries that name, so the flyer a named
//     spawner sends out next joins in turn; named as a flyer, that flyer, until
//     it is gone. RemoveWingman takes one off and lets it go, and it is never
//     called up on its own again.
//   - CallWingmen: wingmen are called up at every takeoff from now on, and at
//     once where the player flies; a number sets how many are called up on their
//     own (named ones come on top, as the formation has slots).
//     DismissWingmen: the flight is let go, its names forgotten, and none is
//     called up on its own until CallWingmen or the squad key.
//   - SetWingmenLocked: none called up on their own (nor by the squad key), and
//     none let go for its distance or for not reaching its slot in time.
//   - SetWingmenOrder: formup, engage or cover is the flight's order now and
//     the one flights start with from now on; attack takes a target (else the
//     player's own, as the comm key does) for the flight in the air. A script's
//     orders put no line on the HUD.
//   - SetWingmenFormation: a map shape by name, or "vee"; nothing: the class's
//     own choice again. SetWingmenLand over WingmanLand and the INI.
//   - Events (lua/lua_events.hpp; each fn(charIndex, object)): OnWingmanJoin
//     and OnWingmanLost (the pilot, its flyer; GetWingmanLostReason says why:
//     killed, pilot, landed, noroom, team, far, late, removed, dismissed,
//     breakoff, formation, leader), OnWingmenOrder (the player, an attack's target), OnWingmanLanded
//     (the pilot, its flyer, down beside the player). Queued and fired once the
//     flyer's own update is done, so a callback may change the flight.
// =============================================================================
namespace flyer_wingmen_lua {

// A unit as a script gives it: a flyer or other world object (an entity
// pointer, or a character's vehicle), or a world object's name (PblHash), for
// whatever carries it.
struct WingmanRef {
   uint8_t* obj = nullptr;
   uint32_t name = 0;
};

bool        available();
int         call(bool setSize, int size);   // the player's wingmen after it
int         dismiss();                      // how many were let go
bool        add(WingmanRef unit);
bool        remove(WingmanRef unit);
bool        give_order(const char* order, WingmanRef target, bool hasTarget);
const char* order();                        // null: no flight
void        lock(bool locked);
bool        formation(const char* name);    // null or "": the class's own
void        land(int land);                 // -1: the class's and the INI's again
int         count();
uint8_t*    wingman(int i);                 // the i-th (0 first) in slot order, or null
bool        is_wingman(const uint8_t* flyer);
uint8_t*    leader_of(const uint8_t* flyer);   // the player's or an AI squadron's leader
int         pilot(const uint8_t* flyer);       // the character flying it, or -1
uint8_t*    character_object(int index, bool vehicleOnly);   // its vehicle, else its soldier
uint8_t*    object(WingmanRef unit);           // the world object it names now, or null
const char* lost_reason();                     // during OnWingmanLost, else null

} // namespace flyer_wingmen_lua
