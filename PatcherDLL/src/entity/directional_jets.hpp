#pragma once

#include <stdint.h>

// =============================================================================
// UseDirectionalJets: a jetting soldier's legs lean with its move, from
// directional animations blended by the way it moves.
//
// STOCK. JET_JUMP and JET_HOVER both play the JET action, "jetpack_hover":
// one animation per half of the body whichever way the soldier jets.
// SoldierAnimator::SetupPose builds the pose each frame the soldier is drawn.
// For JET, UpdateActionAnimation lays the hover on the local pose
// (mZephyrPoseStatic) with ZephyrPoseStatic<32>::Blend, the lower body through
// the animator's lower-body mask (bone_root, bone_pelvis and both legs), and
// turns DummyRoot to the leg angle. SetupPose then calls
// ApplyProceduralAnimationAndBuildWorldMatrices, which turns the spine and
// head to the aim and builds the world matrices the soldier is drawn with.
//
// THIS. A soldier class with UseDirectionalJets = 1 (inherited through
// ClassParent) has four more animations, "jetpack_hover_forward", "_backward",
// "_left" and "_right", found for the soldier's animation map the way the
// directional rolls find their dives (soldier_anim_tables::find_named, lower
// half: "_lower", then plain, then "_full"). Each is the pose while moving
// that way. SetupPose's one call of ApplyProceduralAnimationAndBuildWorldMatrices
// goes through a stand-in that keeps every register, so on every build the
// call goes on as it came. Before it, for a soldier in JET with the hover on
// its legs, the stand-in blends the animations onto the local pose with the
// engine's own Blend, through the same lower-body mask, at the hover's own
// time (so they cycle with it): how much of each comes from the lean. The
// lean is the body's speed along its forward and right rows (its move this
// frame over the frame time, as SetAction reads it for directional jumps) over
// the unit's top jet speed that way (MaxSpeed and MaxStrafeSpeed times its
// ControlSpeed "jet" factors, what EntitySoldier::MoveJetHover pushes toward),
// full at the top speed. The legs' lean follows the move's at the rate
// UpdateActionAnimation turns the legs (7.5 per second), so a turn swings
// them over. A way without its animation keeps the hover for its share.
//
// Each animation is played by a ZephyrPoseDyn<32> on the stand-in's stack,
// set up as SetupPose's own overlay player is: SetAnimation for the
// animator's skeleton, the hover's time, no loop (Zephyr.h).
//
// Only the animation changes, for every soldier each machine draws (AI and
// remote players included, from the move it sees); nothing is sent over the
// network.
//
//                                 modtools    Steam       GOG
//   SetupPose's call of           0x0057D3ED  0x006406DC  0x0064177C  retargeted
//   ApplyProceduralAnimationAndBuildWorldMatrices
//                                 0x00579F10  0x00642860  0x00643900
//     modtools thiscall(float dt), RET 4; Steam and GOG ECX and the frame
//     time in XMM1, plain RET
//   ZephyrPoseDyn<32>::SetAnimation (thiscall(anim, fps), RET 8)
//                                 0x0082AAC0  0x0072D430  0x0072E500
//   ZephyrPoseStatic<32>::Blend(dyn, mask, t) (thiscall, RET 0xC)
//                                 0x0082D450  0x0072DB30  0x0072EC00
//   GameLoop::sClientDeltaTime    0x00C6A9AC  0x01E56058  0x01E574F0
// Fields: SoldierAnimator.h, Zephyr.h, EntitySoldierClass.h. Research:
// docs/RE/SoldierActionAnimations.md. Audited by
// tests/directional_jets_abi_tests.py.
//
// No INI key: the ODF property is the switch.
// =============================================================================

void directional_jets_install(uintptr_t exe_base);
void directional_jets_uninstall();

// Forgets the last level's animations; from LuaHelper::InitState.
void directional_jets_reset();
