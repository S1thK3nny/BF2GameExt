#pragma once

#include <stdint.h>

// =============================================================================
// AI sprint: AI soldiers sprint on long runs out of combat.
//
// Stock AI sprint only to catch up with a squad leader: AILowLevel::mHeight is
// set to HEIGHT_SPRINT (3) in NavigatorFollowTarget::UpdateFollowCrumbs and
// nowhere else for soldiers, so a unit crossing the map to an objective always
// runs. EntitySoldier::UpdateIndirect turns mHeight into a stance each update,
// after the navigator has run: HEIGHT_SPRINT calls the soldier's Sprint(),
// falling back to Stand() when that refuses.
//
// The detour of EntitySoldier::UpdateIndirect raises a STAND height to SPRINT
// for the one update when the decision in ai_sprint_core.hpp says so, and puts
// it back afterwards unless the AI set a height of its own meanwhile, so the
// AI's own state never keeps it. Sprint() still decides: a class that cannot
// sprint, or a soldier not standing, falls back to Stand(), which is what the
// stock update calls for HEIGHT_STAND anyway.
//
// The engine never checks an AI's energy before sprinting (Sprint() has no
// test; only the player's SprintUsingEnergy looks at the bar). EntitySoldier::
// Update drains the bar of any soldier in the sprint state, so the decision
// stops at the bar's overheat latch, as a player's sprint stops.
//
// READ ON ALL THREE BUILDS (modtools / Steam / GOG):
//
//   EntitySoldier::UpdateIndirect   0x0053B920 / 0x004F0110 / 0x004F0110
//     thiscall(float dt), bool in AL, RET 4; `this` is the soldier's
//     Controllable, 0x240 into the object. Steam and GOG reach it only through
//     the soldier's Controllable vtable (slot +0xC4 at Steam 0x0079CF2C), so
//     every caller keeps to the standard convention.
//   Controllable::mCtrl             +0xC8      MOV [ESI+0xC8] / [EDI+0xC8]
//   UnitControllerPtr's test        Controller vptr slot 0, IsRtti("UnitController")
//   UnitController::mLowLevel       +0x2C4     ADD EDI,0x2C4 / LEA EBX,[EAX+0x2C4]
//   AILowLevel::mHeight             +0x6C      the stance switch's MOV [reg+0x6C]
//   AILowLevel::mNavigator          +0x18      IsNavigatorInProgress (M 0x005A2D70,
//                                              S 0x0043BED0, G 0x0043BEC0)
//   Navigator::IsDone               vptr +0x14 same functions
//   Navigator IsRtti                vptr +0x20 thiscall(hash), RET 4 (M 0x005A72B0,
//                                              S 0x005AC4E0, G 0x005AD490)
//   NavigatorGraphFollower::mGlobalDest +0x24  its GetGlobalDest, LEA EAX,[ECX+0x24]
//                                              (M 0x005A7280, S 0x005ADAC0, G 0x005AEA70)
//   Navigator::mPathFollower        +0x10      Navigator::GetGlobalDest (S 0x005AB6F0)
//   PathFollower::mFinalPoint       +0x30      the same function's ADD EAX,0x30
//   AILowLevel::mTarget             +0xA0      ProcessFire's LEA ECX,[reg+0xA0]
//                                              (M 0x005A4126, S 0x0043B628, G 0x0043B618)
//   UnitController state            +0x7C      2 = in combat (UnitController::IsInCombat,
//                                              S 0x00663B92 CMP [ECX+0x7C],2)
//   the soldier's EnergyBar         +0x7E0 / +0x7D0 / +0x7D0 from the Controllable
//                                              (SprintUsingEnergy: M TEST [ECX+0xA28],1,
//                                              S and G TEST [ECX+0xA18],1, ECX the object)
//   EntitySoldier::mState           g_soldier->mState (0x514 / 0x504 / 0x504)
//
// The RTTI hashes are PblHash of the class names: each build makes them with
// the PblHash constructor from the names (Steam 0x00406C50 for UnitController,
// 0x004043D0 for NavigatorGraphFollower).
//
// AI controllers exist only where AI is simulated (single player and the host),
// so a client changes nothing; the sprint itself reaches clients as the
// soldier's state, as a player's does.
//
// INI: [AI] AISprint=1, [AI] AISprintDistance=30 (metres to the destination for
// a sprint to start; it stops at half that).
// =============================================================================

extern bool  g_aiSprint;
extern float g_aiSprintDistance;

void ai_sprint_install(uintptr_t exe_base);
void ai_sprint_uninstall();
