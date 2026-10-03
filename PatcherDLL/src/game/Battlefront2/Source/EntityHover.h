#pragma once

#include <stdint.h>

#include "core/build_field.hpp"

class EntityHoverClass;

// =============================================================================
// EntityHover (and CommandHover, which derives from it) and EntityHoverClass:
// the members GameExt uses so far, not the whole structs. Names from the PDB.
// Offsets are from the object start: the `this` of EntityHover::Move and
// PostCollisionUpdate, and what Trackable::GetGameObject returns for it.
// EntityHover::Update gets the object + 0x240 (its Controllable part), and
// CollisionCallback the object + 0xC (its CollisionObject part). Modtools lays
// EntityHover_data out 0x38 further in than release, and 0x40 past the
// animation blocks; the class's data starts at +0x6AC on modtools and +0x5E4 on
// release, where mSoundCloseToGround (a GameSound, 20 bytes in debug, 8 in
// release) moves everything after it a further 0xC.
//
//   EntityHover         modtools  Steam     read at (modtools / Steam)
//   mMatrix (rows)      0x00f0    0x00f0    Move 0x50E4E1 / 0x4C4059 (LEA of the
//                                           matrix); up and forward rows at
//                                           ApplyCollisionImpact 0x51529F /
//                                           0x4C6408 and Move 0x50F47D / 0x4C44A3
//   mVelocity           0x0498    0x0460    Move 0x50FCDB / 0x4C417F
//   mGroundRatio        0x04b0    0x0478    PostCollisionUpdate 0x51453D / 0x4C343D
//   mClass              0x04c4    0x048c    PostCollisionUpdate 0x51450A / Move 0x4C3FE2
//   mBoost (bool)       0x1d40    0x1d00    Update 0x513AFA / 0x4C2ECB, from the
//                                           Controllable part (+0x1B00 / +0x1AC0)
//   mFlags (byte)       0x1d41    0x1d01    Move 0x50E5AF / 0x4C40ED: 0x02 jumping
//                                           (set at 0x50F49A / 0x4C44CA, cleared at
//                                           0x50F5C0 / 0x4C52D0)
//
//   EntityHoverClass    modtools  Steam
//   mForwardSpeed       0x06c8    0x0600    Move 0x50FD4D / 0x4C42C2
//   mBoostSpeed         0x0ec0    0x0dec    Move 0x50E576 / 0x4C40B5
//
// GOG runs the same code at the same addresses as Steam; every access above is
// the same instruction there. CollisionCallback's `this` is the hover + 0xC:
// it hands the hover on with LEA ECX,[this - 0xC] (0x5156A4 / 0x4C67DD).
//
// mGroundRatio says how much the hover is on the ground. A hover with spring
// bodies (AddSpringBody) sets it to 1 whenever a spring touches ground facing
// up (UpdateColliderBody), and PostCollisionUpdate takes the frame time off it
// every frame, down to 0: 1 less the seconds since a spring last touched. A
// hover without them casts a ray down instead: 1 at its SetAltitude, 0 at ten
// times that. mBoost is set each frame from the sprint input (Update) and
// cleared by Move when the class has no BoostSpeed or the energy runs out. A
// jump (only with a JumpForce) sets 0x02 in mFlags when it starts and clears it
// when the push ends. Details: docs/RE/CameraShake.md.
// =============================================================================

namespace layout::EntityHover {

constexpr uint8_t  kFlagJumping   = 0x02;   // mFlags
constexpr uint32_t kCollisionPart = 0x0C;   // CollisionCallback's `this`

inline constexpr Field<float[4]>          mMatrix_right{0xF0};     // EntityGeometry::mMatrix rows
inline constexpr Field<float[4]>          mMatrix_up{0x100};
inline constexpr Field<float[4]>          mMatrix_forward{0x110};
inline constexpr Field<float[3]>          mVelocity{0x498, 0x460};
inline constexpr Field<float>             mGroundRatio{0x4B0, 0x478};
inline constexpr Field<EntityHoverClass*> mClass{0x4C4, 0x48C};
inline constexpr Field<uint8_t>           mBoost{0x1D40, 0x1D00};
inline constexpr Field<uint8_t>           mFlags{0x1D41, 0x1D01};

} // namespace layout::EntityHover

namespace layout::EntityHoverClass {

inline constexpr Field<float>             mForwardSpeed{0x6C8, 0x600};
inline constexpr Field<float>             mBoostSpeed{0xEC0, 0xDEC};

} // namespace layout::EntityHoverClass
