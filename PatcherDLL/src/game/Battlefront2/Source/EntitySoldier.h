#pragma once

#include <stdint.h>

#include "core/build_field.hpp"

// =============================================================================
// EntitySoldier - the members GameExt reads from here, not the whole struct.
// Offsets are from the start of the object: what a PblHandle<GameObject> and
// Trackable::GetGameObject hold. NOT from the Controllable pointer a Character
// slot holds, which sits +0x240 into the object (see core/entity_layout.hpp for
// the fields read from there).
//
// m_uiInvisibilityAlpha (Phantom PDB name) is how far a soldier is cloaked, 0 to
// 255. PlayerController::Update reads it twice, for the current target and for
// each candidate, and drops an enemy whose alpha is above 0:
//
//               current target                       candidate
//   modtools    MOVZX EDX,byte [ECX+0x753] @0059C2E2 MOVZX EAX,byte [ESI+0x753] @0059C857
//   Steam       MOVZX EAX,byte [ESI+0x743] @0061AFE3 MOVZX EAX,byte [EDI+0x743] @0061B69B
//   GOG         MOVZX EAX,byte [ESI+0x743] @0061C053 MOVZX EAX,byte [EDI+0x743] @0061C70B
//
// Both reads follow a GetDerivedRtti == EntitySoldier test.
// =============================================================================

namespace layout::EntitySoldier {

inline constexpr Field<uint8_t> m_uiInvisibilityAlpha{0x753, 0x743};

} // namespace layout::EntitySoldier
