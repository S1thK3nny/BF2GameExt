#pragma once

#include <stdint.h>

// =============================================================================
// AI squads: AI soldiers form squads with each other, a leader and followers
// who keep formation behind it and fight round it.
//
// SWITCHED OFF (2026-10-12) until reworked: ai_squads_install logs and returns
// (kAISquadsOn in ai_squads.cpp), and its INI keys are out of the registry and
// unread. See the ROADMAP's "AI squads". The rest describes what was built.
//
// Stock BF2 has no AI-led squads: an AI follows a leader only when the player
// commands it ("follow me"), tags along behind the player, or a Lua Follow goal
// sends it. All three end in the same place: UnitController::SendEvent with an
// EventInfo of EVT_SC_FollowChr (0x10) about the leader, which
// UnitAgent::HandleSquadCommandEvent turns into a tight follow command. Nothing
// in that path needs the leader to be a player.
//
// Once a second, before ControllerManager::Update, the tick walks the AI
// controllers and lists the AI soldiers: alive, on foot (an AI in a vehicle
// controls the vehicle, not a soldier), with their team, position, whether
// they already follow someone (their agent is a UnitFollowAgent) or are in
// combat (UnitController state 2), and their class's squad keys. The squads it
// made are checked with the game's own query, EVQ_SC_IsFollowingChr (0x0F),
// which a follower answers 1 only about the leader it follows; a recruit's
// order waits for its controller's next decision, so it counts as a member for
// a few seconds before it has to (ai_squads_core.hpp). A dead leader or one
// gone into a vehicle ends its squad on its own. ai_squads_core.hpp plans
// the recruits, which go out as EVT_SC_FollowChr, as the player's command does.
// squad_formation then lines the squad up.
//
// Who leads: the highest SquadLeader rank in the class ODF; unset, any class but
// a hero at rank 1 while [AI] AISquads is on, none while it is off. Ties go to
// the one alive longest (the tick notes when it first sees each soldier). A
// unit joins only a leader of its team within [AI] AISquadRange, ranked at
// least as high as itself, unless its class sets SquadFollower = 0 (heroes do
// not join unless their class sets 1). A squad takes SquadSize from the
// leader's class, or [AI] AISquadSize.
//
// Only a soldier on its way somewhere recruits, its navigator taking it 20 m or
// more (read as AI sprint reads it): a sniper settled at its spot, or a unit
// holding or defending a place, takes nobody along. A squad whose leader stands
// within 3 m of one place out of combat for 20 s is let go: each follower's
// UnitFollowAgent gets SetNextState(100), what its own stop-follow handling
// does, without the voice line that comes with the event.
//
// Each recruit plays the game's own follow response, as for the player's
// command: VOHelper::Play with no listener plays it at the local player.
//
// READ ON ALL THREE BUILDS (modtools / Steam / GOG):
//
//   ControllerManager::Update       0x005997A0 / 0x00486370 / 0x00486370
//     modtools cdecl(float dt); Steam and GOG take dt in XMM0, no stack
//     argument, so their detour is a naked shim that keeps every register and
//     jumps on to the trampoline.
//   sAICtrls, the AI controllers    the global 0x00B8EE70 / 0x01E30934 /
//                                   0x01E31DCC: +4 the first node, a node's +4
//                                   the next and +0xC the UnitController, a null
//                                   controller the end. Update's decision budget
//                                   walks it, and a loop that hands each one to a
//                                   UnitController method (M 0x00599AFE, S and G
//                                   0x004865D2, SendEvent). Its first loop walks
//                                   sPlayerCtrls (0x00B8EE6C / 0x01E30930 /
//                                   0x01E31DDC), player controllers only.
//   Controller                      vptr +0x00 IsRtti; +0x4 mOwner, the
//                                   Controllable; the Controllable's +0x18
//                                   interface, vptr +0x20, gives its GameObject
//                                   (CheckForAutoFollow's own reads)
//   UnitController                  +0x7C state (2 in combat, as AI sprint reads
//                                   it); +0x2C0 mAgent (SendEvent: M 0x0059F8A6,
//                                   S 0x006640D9); +0x2C4 AILowLevel, its +0x18
//                                   navigator and the navigator's destination as
//                                   ai_sprint.hpp lists them
//   UnitAgent's vtable              +0x00 EnterState, +0x1C SetNextState (stores
//                                   +8, RET 4), +0x20 IsRtti, +0x50 EventHandler
//                                   (Phantom's UnitAgent_vftable;
//                                   UnitFollowAgent's at M 0x00A487F0, S 0x007AFB10,
//                                   G 0x007B0A88, its IsRtti M 0x005B00C0,
//                                   S 0x00667F00, G 0x00668FA0). Slot 0 is NOT
//                                   IsRtti here, unlike a Controller's or a
//                                   GameObject's: the first build called
//                                   EnterState on every AI agent each tick.
//   GameObject                      +0x120 position; +0x144 health; +0x1FC flags,
//                                   bit 3 alive (as the target bars read them);
//                                   +0x204 handle id; +0x234 team, the low 4 bits
//                                   signed (CheckForAutoFollow's SHL 0x1C / SAR)
//   the events                      squad_formation.hpp: EventInfo's constructor
//                                   and SendEvent
//
// AI controllers exist only where AI is simulated (single player and the host),
// so a client changes nothing.
//
// INI: [AI] AISquads=1, AISquadSize=3, AISquadRange=20 - the values for classes
// that set no key.
// =============================================================================

extern bool  g_aiSquads;
extern int   g_aiSquadSize;
extern float g_aiSquadRange;

void ai_squads_install(uintptr_t exe_base);
void ai_squads_uninstall();
