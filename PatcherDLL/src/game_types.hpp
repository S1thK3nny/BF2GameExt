#pragma once

// =============================================================================
// SWBF2 (2005) Game Engine Types - Umbrella Include
//
// Reconstructed struct layouts from reverse engineering.
// One class per file under game/ — include this header for everything,
// or include individual headers for just what you need.
// =============================================================================

// Math primitives
#include "game/pbl/PblVector3.hpp"
#include "game/pbl/PblMatrix.hpp"
#include "game/pbl/PblQuaternion.hpp"
#include "game/pbl/PblSphere.hpp"
#include "game/pbl/PblHandle.hpp"
#include "game/pbl/PblListNode.hpp"

// Spatial partitioning
#include "game/treegrid/TreeGridStack.hpp"
#include "game/treegrid/TreeGridObject.hpp"

// Entity hierarchy
#include "game/entity/Entity.hpp"
#include "game/entity/EntityEx.hpp"
#include "game/entity/Damageable.hpp"
#include "game/entity/GameObject.hpp"
#include "game/entity/EntitySoldierClass.hpp"
#include "game/entity/Factory.hpp"

// Controllable / Character
#include "game/controllable/Character.hpp"
#include "game/controllable/Controllable.hpp"
#include "game/controllable/ControllableClass.hpp"
#include "game/controllable/Controller.hpp"
#include "game/controllable/PlayerController.hpp"

// Weapons
#include "game/weapon/WeaponClass.hpp"
#include "game/weapon/Weapon.hpp"
#include "game/weapon/Aimer.hpp"
#include "game/weapon/EnergyBar.hpp"
#include "game/weapon/OrdnanceMissile.hpp"

// Collision
#include "game/collision/CollisionModel.hpp"

// Camera / Rendering
#include "game/camera/CameraTrackSetting.hpp"
#include "game/camera/RedCamera.hpp"
#include "game/camera/RedSceneObject.hpp"

// Misc game systems
#include "game/misc/RedColor.hpp"
#include "game/misc/GameSoundControllable.hpp"
#include "game/misc/DamageOwner.hpp"
#include "game/misc/LockOnManager.hpp"
#include "game/misc/Team.hpp"
