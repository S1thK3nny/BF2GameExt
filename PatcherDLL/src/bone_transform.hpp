#pragma once

#include "pch.h"

// TransformBone ODF property — enables procedural transform animation on
// custom bones per EntitySoldierClass. The vanilla game only enables this
// for bone_root and hp_weapons. This hook lets modders add any bone via:
//
//   AnimationName = "aalya"              (must come first)
//   TransformBone = "hp_weapons_left"
//   TransformBone = "some_custom_bone"

void bone_transform_install(uintptr_t exe_base);
void bone_transform_uninstall();
