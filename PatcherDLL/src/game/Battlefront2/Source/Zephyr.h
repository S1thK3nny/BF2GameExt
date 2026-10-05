#pragma once

#include <stdint.h>

#include "core/build_field.hpp"

// =============================================================================
// Zephyr, the engine's skeletal animation: the members GameExt uses so far, not
// the whole structs. Names from the PDB. None of these move between the debug
// and release layouts. Addresses of the functions in game_addrs.hpp.
//
// A ZephyrAnim is one animation's data. A ZephyrPoseDyn<32> plays one: its
// m_kAnim (a ZephyrAnimInst<32>, 0x988 bytes: per-joint decoder state and the
// joint maps between the animation and the skeleton) and the time it is at,
// m_fCurT, from 0 at the first frame to 1 at the last. A ZephyrPoseStatic<32>
// is a local pose, one ZephyrTransform (a quaternion then a translation, 0x1C
// bytes) per skeleton joint, at most 32; a rotation's w of 1e6 marks a joint
// not set yet. ZephyrPoseStatic<32>::Blend(dyn, mask, t) moves each joint the
// mask and the animation both have a fraction t toward the animation at its
// time (slerp and lerp), or sets it when it is not set yet.
//
//   ZephyrPoseDyn<32>      offset  read in (modtools / Steam / GOG)
//   m_kAnim.m_pkAnim       0x980   ZephyrAnimInst<32>::SetAnim (0x862587 /
//                                  0x72F3FC / 0x7304CC), GetJointTransform
//                                  (0x82A5B8 / 0x72CDDE / 0x72DEAE)
//   m_kAnim.m_piAnimJointIdx 0x940 Blend (0x82D4AF / 0x72DB9B / 0x72EC6B)
//   m_pSkel                0x988   SetAnimation hands it to SetAnim (0x82AAD1 /
//                                  0x72D446 / 0x72E516)
//   m_fCurT                0x99C   SetAnimation zeroes it (0x82AAE9 / 0x72D466 /
//                                  0x72E536)
//   m_bLoop                0x9AA   GetJointTransform (0x82A644 / 0x72CE4B /
//                                  0x72DF1B)
//   m_bInterpolate         0x9AD   GetJointTransform (0x82A5FC / 0x72CE13 /
//                                  0x72DEE3)
//   ZephyrAnim::m_u16NumFrames  +8  GetJointTransform (0x82A5BE), and after
//                                  UpdateLowerBodyAnimation's m_pkAnim load
//   ZephyrAnim::m_u16NumJoints  +0xA SetAnim (0x8625DA / 0x72F44B / 0x73051B)
//   ZephyrSkeleton<32>::m_pShared 0  Blend, through its pose's m_pSkel
//
// SetAnimation(anim, fps) (thiscall, RET 8) points m_kAnim at the animation
// for m_pSkel's skeleton, maps its joints and starts its decoders, and zeroes
// m_fCurT. GetJointTransform reads frame (frames - 1) * m_fCurT and the next,
// and with m_bLoop the frame after the last is the second: an animation of one
// frame must not loop. An animation with more than 32 joints overruns
// m_kAnim's tables, in the engine's own players as in ours.
// =============================================================================

namespace layout::ZephyrPoseDyn {

constexpr uint32_t kSize = 0x9B0;   // ZephyrPoseDyn<32>

inline constexpr Field<void*>    m_pkAnim{0x980};   // m_kAnim.m_pkAnim
inline constexpr Field<void*>    m_pSkel{0x988};
inline constexpr Field<float>    m_fCurT{0x99C};
inline constexpr Field<uint8_t>  m_bLoop{0x9AA};
inline constexpr Field<uint8_t>  m_bInterpolate{0x9AD};

} // namespace layout::ZephyrPoseDyn

namespace layout::ZephyrAnim {

constexpr uint32_t kMaxJoints = 32;   // ZephyrAnimInst<32>'s tables

inline constexpr Field<uint16_t> m_u16NumFrames{0x8};
inline constexpr Field<uint16_t> m_u16NumJoints{0xA};

} // namespace layout::ZephyrAnim

namespace layout::ZephyrSkeleton {

inline constexpr Field<void*>    m_pShared{0x0};

} // namespace layout::ZephyrSkeleton
