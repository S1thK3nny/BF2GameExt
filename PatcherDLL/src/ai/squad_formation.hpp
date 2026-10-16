#pragma once

#include "squad_formation_core.hpp"

#include <stdint.h>

// =============================================================================
// Squad formations: followers line up on their leader in one layout, at one
// spacing, facing the way the squad travels; friendlies who tag along with the
// player can join the squad in formation. A class's ODF keys decide for it.
//
// A unit following a leader in formation (the player's "follow me" command, a
// Lua Follow goal, a tag-along that formed up) runs UnitFollowAgent's tight
// follow state. Each of its updates asks ReserveManager::ReserveOffset for its
// slot: its place among the leader's formation reservations, laid out by a type
// a small helper picks per follower (a wedge when the follower's controller has
// more than 10 m of open space round it, two abreast otherwise). The agent
// scales the slot by 2.0 (2.2 while the leader moves under 4 m/s, 1.7 while the
// follower jet-hovers) and hands it to AILowLevel::SetNavigator_FollowTarget,
// which builds a new NavigatorFollowTarget whenever the offset differs at all
// from the one it has. The navigator turns the offset by the leader's matrix
// each update and walks a trail of crumbs to the point. Why a stock squad
// clumps: each follower picks its own layout; the first two slots are 1.5 m
// either side of the leader and 0.5 m behind; and a player's matrix turns with
// the camera, so looking round swings the whole squad.
//
// Two calls are redirected for that, together or not at all:
//
//   - the follow agent's call to ReserveOffset goes to formation_offset. It
//     calls ReserveOffset for single file, which keeps the game's reservation
//     and gives the slot number as -z, then lays that slot out in formation
//     space (squad_formation_core.hpp) and hands it back divided by the agent's
//     2.0, with a tag in y. The offset stays the same while the slot does, so
//     the navigator is not rebuilt as the squad turns.
//   - UpdateTarget's D3DXVec3TransformCoord goes to follow_transform. A tagged
//     offset turns to the squad's heading round the leader's position: the
//     leader's direction of travel, eased, kept while the leader stands, so the
//     camera never moves the squad. Any other offset (CTF escorts, convoys,
//     a leader's class set to stock) goes to D3DX as before.
//
// Routes: the navigator walks the trail its slot leaves, a crumb every 2 m,
// cutting across to a crumb up to 30 m off when a straight-line test finds the
// way clear; it plans, on the coarse hub graph, only when a new navigator can
// see no crumb at all. The trail runs wherever the slot went, through scenery
// the leader walked beside, so a follower can stay caught on a wall or a
// barrier. A unit going anywhere else, a loose follower included, gets
// SetNavigator_Goto: the planner's route (A* under 50 m, the hub graph past
// it). The follow agent's call to SetNavigator_FollowTarget goes to
// follow_route, which leaves the crumbs to the game while the follower keeps
// up, and gives it SetNavigator_Goto to the leader's position once it is more
// than 10 m from its slot, or more than 3 m and has made no metre of headway
// for 2 s while its slot moved on; it keeps the route until it is within 4 m
// of its slot or its leader (squad_formation_core.hpp, follow_action). The
// route's navigator is known by its address and vptr, and another is asked for
// at most every 3 s, as the planner's queue sends a unit asking again to the
// back. On a route it sprints while more than 4 m from its slot, as the follow
// navigator would. Off with SquadPathfinding=0.
//
// Tag-alongs: PlayerController::CheckForAutoFollow recruits friendlies the
// player runs past with EVT_SC_FollowChrLoose (0x11): no slot, a random spot
// about 10 m behind, a loose group, until the player has two formation
// followers. Two more calls are redirected, together or not at all: its
// reservation count, so its own stop (CMP EAX,2) comes at the squad size, and
// its recruit, which then sends EVT_SC_FollowChr (0x10), the "follow me"
// command, or skips a class that never joins.
//
// ODF keys, on a soldier class, inherited through ClassParent. Each one a class
// sets decides that setting for the class whatever the INI says; one it leaves
// out takes the INI's. So the INI can turn a setting off for stock sides while a
// mod's classes still use theirs:
//
//   SquadFormation, SquadSpacing   the leader's class: its squads' layout, spacing
//   SquadSize                      the leader's class: followers it takes; on the
//                                  player's class, above 0, tag-alongs form up
//   SquadFollower                  the follower's class: 0 never joins a squad.
//                                  Unset, heroes (Team::AddSpecialClass) do not
//   SquadLeader                    who leads an AI squad (ai_squads.hpp)
//
// READ ON ALL THREE BUILDS (modtools / Steam / GOG):
//
//   the follow agent's call         0x005B0AB1 / 0x006687BC / 0x0066985C
//     to ReserveOffset              (UnitFollowAgent::UpdateStatePostVision, tight
//                                   follow; S 0x00668500, G 0x006695A0, M has the
//                                   code but no Ghidra function)
//   ReserveManager::ReserveOffset   0x005C6840 / 0x0062FA70 / 0x00630B10
//     modtools: cdecl(out, leader, follower, type), through the thunk 0x00406E29.
//     Steam, GOG: ECX = out, EDX = leader, follower and type on the stack, the
//     caller pops; it clobbers XMM6. Both return out in EAX. leader and follower
//     are GameObjects.
//   the slot number                 the count of the leader's formation
//                                   reservations before the follower's own: M EBX
//                                   to [ESP+0x14] (0x005C6917), S and G [EBP-4] to
//                                   ECX at the switch
//   single file, type 1             (0, 0, -slot): M 0x005C69FB scales (0,0,-1);
//                                   S 0x0062FBF1 / G 0x00630C91 multiply by 0.0
//                                   and XMM6 = -1.0 (S 0x007B253C, G 0x007B34B4)
//   the layout helper               0x005AFEE0 / 0x00667FC0 / 0x00669060: 6 (wedge)
//                                   when UnitController +0x48 (mOpenRadius) > 10,
//                                   else 2 (two abreast); the agent passes it as
//                                   type, so formation_offset reads 6 as room
//   the agent's scale               2.0, 2.2, 1.7: M MOV [ESP+0x14] at 0x005B0A51,
//                                   0x005B0A7B, 0x005B0A87; S 0x007B21D8,
//                                   0x007B21F0, 0x007B2188; G 0x007B3150,
//                                   0x007B3168, 0x007B3100. Under 4 m/s is a
//                                   squared length under 16 on every build.
//   UpdateTarget's turn             0x005A6A49 / 0x005ACC31 / 0x005ADBE1:
//                                   D3DXVec3TransformCoord(out, &mFollowOffset,
//                                   leader matrix), stdcall. M CALLs the thunk
//                                   0x007CB7B5, JMP [0x00ADFC58] (D3DX is linked in
//                                   and dispatches through that .data pointer); S
//                                   and G CALL [0x0076B54C] / [0x0076C548], the
//                                   d3dx9_43 import. The matrix comes from the
//                                   leader's +0xC interface, vptr +0x40: rows
//                                   right, up, forward, position.
//   NavigatorFollowTarget           mFollowTarget +0x44 (object) / +0x48 (handle
//                                   id), mFollowOffset +0x4C (UpdateTarget's own
//                                   reads; IsEqual, P 0x0067A860)
//   GameObject                      vptr +0x00 IsRtti, +0x44 GetVelocity (a
//                                   PblVector3 pointer; the agent's speed test:
//                                   M 0x005B0A62, S 0x00668744, G 0x006697E4);
//                                   +0x8 its GameObjectClass; +0x204 handle id
//   a soldier's class               Controllable (+0x240) + g_soldier->classPtr,
//                                   the class the ODF reader hands the handlers
//   a hero class                    GameObjectClass flags bit 1 (GameObjectClass.h)
//   CheckForAutoFollow              0x0059AD80 / 0x0061C150 / 0x0061D1C0
//     its count                     0x0059ADE9 / 0x0061C1B2 / 0x0061D222: a CALL
//                                   to GetReservationCountForObject (0x005C6560
//                                   through 0x0040200E / 0x0062F760 / 0x00630800),
//                                   modtools cdecl(obj, team, type), retail ECX =
//                                   obj, EDX = team, type pushed, the caller
//                                   popping; then CMP EAX,2 / JGE
//     its recruit                   0x0059AFC3 / 0x0061C3E1 / 0x0061D451: a CALL to
//                                   UnitController::SendEvent (0x0059F8A0 through
//                                   0x0040C216 / 0x006640D0 / 0x00665170),
//                                   thiscall(EventInfo*), RET 4, after EventInfo
//                                   was built with type 0x11 and the player
//   EventInfo                       mType +0x34, mGameObject +0x38 / +0x3C; its
//                                   constructor only stores them (S 0x0043EDF0)
//   Controller::mOwner              +0x4, the Controllable
//   the follow agent's call to      0x005B0AE1 / 0x0066880C / 0x006698AC: ECX =
//     SetNavigator_FollowTarget     the UnitController's AILowLevel (+0x2C4), the
//                                   leader and &slot pushed (M through the thunk
//                                   0x0040650A)
//   AILowLevel::                    0x005A3180 / 0x0043C240 / 0x0043C230,
//     SetNavigator_FollowTarget     thiscall(leader, offset), RET 8; it keeps the
//                                   navigator only if it is a NavigatorFollowTarget
//                                   (IsRtti, vptr +0x20) with the same leader and
//                                   offset
//   AILowLevel::SetNavigator_Goto   0x005A2E00 / 0x0043BF30 / 0x0043BF20,
//                                   thiscall(dest, obstacle, bool, bool), RET 0x10;
//                                   called (dest, null, false, false) as the game's
//                                   own agents call it (UnitFollowAgent's loose
//                                   move, P 0x00791E40). Retail reads only ECX and
//                                   its stack.
//   AILowLevel                      mOwner +0x14 (UnitController), mNavigator
//                                   +0x18, mHeight +0x6C (0 stand, 3 sprint)
//   Navigator                       vptr +0x14 IsDone
//   a Controllable's GameObject     its Trackable at +0x18, vptr +0x20; position
//                                   at GameObject +0x120
//
// Event types (Phantom's PDB, EventType): 0x0F EVQ_SC_IsFollowingChr, 0x10
// EVT_SC_FollowChr, 0x11 EVT_SC_FollowChrLoose, 0x12 EVT_SC_StopFollowChr.
// UnitAgent::HandleSquadCommandEvent builds the same follow command for 0x10
// and 0x11, tight when the type is 0x10.
//
// AI controllers exist only where AI is simulated (single player and the host),
// so a client changes nothing.
//
// INI: [AI] SquadFormation=auto|wedge|file|column|line|stock, SquadSpacing=3,
// SquadAutoFormUp=1, SquadSize=4: the values for classes that set no key.
// SquadPathfinding=1: follower routes (no ODF key).
// =============================================================================

extern int   g_squadFormation;     // squad_formation::Layout
extern float g_squadSpacing;
extern bool  g_squadAutoFormUp;
extern int   g_squadSize;
extern bool  g_squadPathfinding;

void squad_formation_install(uintptr_t exe_base);
void squad_formation_uninstall();

// For AI-led squads (ai_squads.cpp), from what this module keeps: the ODF keys
// of a soldier's class (null for none, or for anything but a soldier), whether
// an object's class is a team's hero class, the INI's values (AI squads set
// theirs at install), and an EventInfo of `type` about `leader` sent to a
// UnitController: SendEvent's answer, or -1 where the build has neither.
const squad_formation::ClassKeys* squad_class_keys(uint8_t* soldier);
bool squad_is_hero(const uint8_t* obj);
squad_formation::Defaults& squad_defaults();
int squad_send_event(void* unitController, int type, void* leader);
