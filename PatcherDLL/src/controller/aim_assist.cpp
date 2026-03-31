#include "pch.h"
#include "aim_assist.hpp"
#include "controller_support.hpp"
#include "util/ini_config.hpp"
#include "core/resolve.hpp"

#include <detours.h>
#include <cmath>

// =============================================================================
// Aim assist — reimplemented from Xbox UpdateTargetLockedObjTracking
// =============================================================================
//
// The Xbox function (Controllable::UpdateTargetLockedObjTracking, dead code at
// 0x005e1120 on modtools PC — zero xrefs, consumer code stripped) implements:
//
//   1. Auto-tracking (mTurnAuto/mPitchAuto): when no stick input, camera
//      automatically tracks the locked target. Rate-limited ramp at 5.0/s.
//
//   2. Directional friction (mTurnAdjusted/mPitchAdjusted): only slows stick
//      when pushing AWAY from the target. Full stick toward target. Friction
//      scales from 1.0 (center) to 0.0 (edge) — edge = complete suppression.
//
//   3. Lock break: when pushing away near the bubble edge for 0.1s, the
//      target is deselected via SetTargetLockedObj(0).
//
// Since the PC consumer code for mTurnAdjusted/mTurnAuto was stripped, we
// apply these as modifications to mControlTurn/mControlPitch directly.
//
// Xbox constants (from 0x00ad311c): ramp rate 5.0, friction scale 3.0,
// friction threshold 0.7 screen units, ramp range 1.3, ramp scale 1.5,
// lock break timer 0.1s, dot threshold 0.7.

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static GameLog_t s_log = nullptr;

// ---------------------------------------------------------------------------
// INI config
// ---------------------------------------------------------------------------

static bool  s_aimAssistEnabled = true;
static float s_coneAngle = 30.0f;   // degrees — fallback when weapon has no AutoAimSize
static float s_trackingDeadZone = 0.5f;    // multiplier for weapon AutoAimSize (Xbox: 0.5)
static float s_frictionStrength = 3.0f;    // directional friction scale (Xbox: 3.0)
static float s_pullStrength = 5.0f;    // auto-tracking ramp rate per second (Xbox: 5.0)
static float s_lockBreakTime = 0.1f;    // seconds of pushing away to break lock (Xbox: 0.1)
static bool  s_autoLockOnHit = true;
static float s_snapStrength = 1.0f;    // instant correction on first lock frame (0=ramp only, 1=full snap)
static bool  s_proximityFriction = true;    // slowdown when crosshair near ANY enemy
static float s_proxFrictionRadius = 0.5f;    // screen-space radius (NDC units)
static float s_proxFrictionScale = 0.4f;    // min friction factor at center (0=full stop, 1=none)

void aim_assist_load_config(const char* ini_path)
{
    ini_config cfg{ ini_path };
    s_aimAssistEnabled = cfg.get_bool("AimAssist", "Enabled", true);
    s_coneAngle = cfg.get_float("AimAssist", "ConeAngle", 30.0f);
    s_trackingDeadZone = cfg.get_float("AimAssist", "TrackingDeadZone", 0.5f);
    s_frictionStrength = cfg.get_float("AimAssist", "FrictionStrength", 3.0f);
    s_pullStrength = cfg.get_float("AimAssist", "PullStrength", 5.0f);
    s_lockBreakTime = cfg.get_float("AimAssist", "LockBreakTime", 0.1f);
    s_autoLockOnHit = cfg.get_bool("AimAssist", "AutoLockOnHit", true);
    s_snapStrength = cfg.get_float("AimAssist", "SnapStrength", 1.0f);
    s_proximityFriction = cfg.get_bool("AimAssist", "ProximityFriction", true);
    s_proxFrictionRadius = cfg.get_float("AimAssist", "ProximityFrictionRadius", 0.5f);
    s_proxFrictionScale = cfg.get_float("AimAssist", "ProximityFrictionScale", 0.4f);
}

// ---------------------------------------------------------------------------
// Per-build address table
// ---------------------------------------------------------------------------

struct AimAssistAddrs {
    uintptr_t player_controller_update;
    uintptr_t apply_damage;
    uintptr_t lockon_mgr_array;
    uintptr_t get_cur_wpn;
    uintptr_t set_target_locked_obj;
    uintptr_t m_camera_global;
    uintptr_t num_joysticks_global;
    unsigned  wpn_class_vert_threshold;
    unsigned  wpn_class_horiz_threshold;
    uintptr_t team_get_objects_in_range;
};

static constexpr AimAssistAddrs MODTOOLS_ADDRS = {
    game_addrs::modtools::player_controller_update,
    game_addrs::modtools::apply_damage,
    game_addrs::modtools::lockon_mgr_array,
    game_addrs::modtools::get_cur_wpn,
    game_addrs::modtools::set_target_locked_obj,
    game_addrs::modtools::m_camera_global,
    game_addrs::modtools::num_joysticks_global,
    0x298, // WeaponClass AutoAimSize vertical
    0x29C, // WeaponClass AutoAimSize horizontal
    game_addrs::modtools::team_get_objects_in_range,
};

static constexpr AimAssistAddrs STEAM_ADDRS = {
    game_addrs::steam::player_controller_update,
    game_addrs::steam::apply_damage,
    game_addrs::steam::lockon_mgr_array,
    game_addrs::steam::get_cur_wpn,
    game_addrs::steam::set_target_locked_obj,
    game_addrs::steam::m_camera_global,
    game_addrs::steam::num_joysticks_global,
    0x1C0,
    0x1C4,
    game_addrs::steam::team_get_objects_in_range,
};

static constexpr AimAssistAddrs GOG_ADDRS = {
    game_addrs::gog::player_controller_update,
    game_addrs::gog::apply_damage,
    game_addrs::gog::lockon_mgr_array,
    game_addrs::gog::get_cur_wpn,
    game_addrs::gog::set_target_locked_obj,
    game_addrs::gog::m_camera_global,
    game_addrs::gog::num_joysticks_global,
    0x1C0,
    0x1C4,
    game_addrs::gog::team_get_objects_in_range,
};

// Active address set — selected at install time
static const AimAssistAddrs* s_addrs = nullptr;

// ---------------------------------------------------------------------------
// Runtime joystick check — aim assist is controller-only
// ---------------------------------------------------------------------------

static bool is_joystick_connected()
{
    if (!s_addrs) return false;
    return *(int*)resolve(s_addrs->num_joysticks_global) > 0;
}

// ---------------------------------------------------------------------------
// Resolved addresses
// ---------------------------------------------------------------------------

static uintptr_t s_lockOnMgrArray = 0;  // LockOnManager* array base
static uintptr_t s_cameraGlobal = 0;  // RedCamera** (dereference once for RedCamera*)

// TeamManager::sGetObjectsInRange — returns validated GameObjects from team member lists
// __cdecl(PblVector3* pos, float radius, GameObject** out, int maxCount, Team* team, int affiliationFlags, GameObject* exclude)
// With team=NULL, affiliationFlags=0x02: returns ALL team-registered objects in range.
using fn_TeamGetObjectsInRange = int(__cdecl*)(float* pos, float radius, uintptr_t* out, int maxCount, void* team, int flags, uintptr_t exclude);
static fn_TeamGetObjectsInRange s_teamGetObjectsInRange = nullptr;

// GetCurWpn — returns Weapon* from Controllable
using fn_GetCurWpn = void* (__thiscall*)(void* ctrl);
static fn_GetCurWpn s_getCurWpn = nullptr;
static unsigned s_wpnClassVertThreshold = 0;
static unsigned s_wpnClassHorizThreshold = 0;

// SetTargetLockedObj — handles PblHandle + LockOnManager + network sync
// Pass nullptr to clear lock (calls RemoveTargetLock internally).
// __thiscall(Controllable*, GameObject*), RET 4
using fn_SetTargetLockedObj = void(__thiscall*)(void* ctrl, void* gameObject);
static fn_SetTargetLockedObj s_setTargetLockedObj = nullptr;

// ---------------------------------------------------------------------------
// Controllable offsets (identical all builds)
// ---------------------------------------------------------------------------

static constexpr unsigned OFF_CTRL_TURN = 0x88;
static constexpr unsigned OFF_CTRL_PITCH = 0x8C;
static constexpr unsigned OFF_CTRL_AIM_START = 0xDC;   // PblVector3 (3 floats) — eye position
static constexpr unsigned OFF_CTRL_AIM_DIR = 0xE8;   // PblVector3 (3 floats) — aim direction
static constexpr unsigned OFF_TARGET_LOCKED = 0x138;  // PblHandle.mObject
static constexpr unsigned OFF_TARGET_HANDLE_ID = 0x13C;// PblHandle.mSavedHandleId

// PlayerController offsets
static constexpr unsigned OFF_PC_OWNER = 0x04;   // Controllable* mOwner

// Entity offsets (from Entity* / GameObject* pointer)
static constexpr unsigned ENT_DAMAGEABLE_HEALTH = 0x144;  // Damageable.mCurHealth (float)
static constexpr unsigned ENT_HANDLE_ID = 0x204;  // PblHandled.mHandleId (uint32, generation counter)

// TreeGridObject — collision sphere position (all builds identical)
static constexpr unsigned ENT_TGO_STACK_PTR = 0x10;   // TreeGridObject.mStackPtr
static constexpr unsigned ENT_TGO_STACK_IDX = 0x14;   // TreeGridObject.mStackIdx
static constexpr unsigned ENT_TGO_INLINE_SPHERE = 0x18;   // Inline mCollisionSphere (Vec3 fallback)

// Damageable subobject offset from Entity base
static constexpr unsigned DAMAGEABLE_TO_ENTITY = 0x140;  // Entity* = Damageable* - 0x140
// Controllable subobject offset from Entity base
static constexpr unsigned CONTROLLABLE_TO_ENTITY = 0x240;  // Entity* = Controllable* - 0x240

// DamageDesc.DamageOwner offsets (shooter info)
static constexpr unsigned DD_SHOOTER_ENTITY = 0x04;   // DamageOwner.mGameObject.mObject (Entity*)

// GameObject team field — low 4 bits
static constexpr unsigned ENT_TEAM_AND_TYPE = 0x234;

// LockOnManager offsets
static constexpr unsigned LOM_CLEAR_TARGET = 0x928;
static constexpr unsigned LOM_CLEAR_HANDLE_ID = 0x92C;
static constexpr unsigned LOM_FORCE_TARGET = 0x934;
static constexpr unsigned LOM_FORCE_HANDLE_ID = 0x938;
static constexpr unsigned LOM_FORCE_LOCK = 0x940;

// Weapon vtable slot 21 = IsMelee
static constexpr unsigned WEAPON_VTABLE_IS_MELEE = 21 * 4;

// ---------------------------------------------------------------------------
// Math helpers
// ---------------------------------------------------------------------------

struct Vec3 {
    float x, y, z;
};

static inline Vec3 vec3_sub(Vec3 a, Vec3 b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

static inline float vec3_dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline float vec3_length(Vec3 v)
{
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

static inline Vec3 vec3_normalize(Vec3 v)
{
    float len = vec3_length(v);
    if (len < 1e-6f) return { 0.0f, 0.0f, 0.0f };
    float inv = 1.0f / len;
    return { v.x * inv, v.y * inv, v.z * inv };
}

// Get collision sphere world position — same method as game's UpdateTargetLockedObjTracking.
// Works for all entity types (soldiers, vehicles, turrets).
static inline Vec3 getCollisionSpherePos(uintptr_t entity)
{
    uintptr_t stackPtr = *(uintptr_t*)(entity + ENT_TGO_STACK_PTR);
    if (stackPtr) {
        int stackIdx = *(int*)(entity + ENT_TGO_STACK_IDX);
        float* pos = (float*)(stackPtr + (stackIdx + 4) * 16);
        return { pos[0], pos[1], pos[2] };
    }
    return *(Vec3*)(entity + ENT_TGO_INLINE_SPHERE);
}

// Check IsMelee via weapon vtable slot 21
static bool weapon_is_melee(void* weapon)
{
    if (!weapon) return false;
    uintptr_t vtable = *(uintptr_t*)weapon;
    if (!vtable) return false;
    auto fn = (bool(__thiscall*)(void*)) *(uintptr_t*)(vtable + WEAPON_VTABLE_IS_MELEE);
    return fn(weapon);
}

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

// Shared state between ApplyDamage and PCUpdate hooks
static uintptr_t s_playerEntity = 0;      // Entity* — set each frame in PCUpdate when aim assist is allowed
static uintptr_t s_autoLockTarget = 0;      // Entity* of last damaged target
static uint32_t  s_autoLockHandleId = 0;      // generation counter for validation
static bool      s_currentWpnIsMelee = false;  // true when holding melee weapon

// Auto-tracking & lock break state
static uintptr_t s_prevLockedEntity = 0;      // detect new lock
static float     s_turnAuto = 0.0f;   // smoothed horizontal auto-correction [-1, 1]
static float     s_pitchAuto = 0.0f;   // smoothed vertical auto-correction [-1, 1]
static float     s_lockBreakTimer = 0.0f;   // accumulates when pushing away near edge

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void clear_lockon_mgr_state()
{
    if (!s_lockOnMgrArray)
        return;

    uintptr_t lockOnMgr = *(uintptr_t*)s_lockOnMgrArray;
    if (!lockOnMgr)
        return;

    *(uintptr_t*)(lockOnMgr + LOM_CLEAR_TARGET) = 0;
    *(uint32_t*)(lockOnMgr + LOM_CLEAR_HANDLE_ID) = 0;
    *(uintptr_t*)(lockOnMgr + LOM_FORCE_TARGET) = 0;
    *(uint32_t*)(lockOnMgr + LOM_FORCE_HANDLE_ID) = 0;
    *(int*)(lockOnMgr + LOM_FORCE_LOCK) = 0;
}

static void clear_locked_target(uintptr_t ctrl)
{
    if (!ctrl)
        return;

    if (*(uintptr_t*)(ctrl + OFF_TARGET_LOCKED) == 0 &&
        *(uint32_t*)(ctrl + OFF_TARGET_HANDLE_ID) == 0)
    {
        clear_lockon_mgr_state();
        return;
    }

    if (s_setTargetLockedObj) {
        s_setTargetLockedObj((void*)ctrl, nullptr);
    }
    else {
        *(uintptr_t*)(ctrl + OFF_TARGET_LOCKED) = 0;
        *(uint32_t*)(ctrl + OFF_TARGET_HANDLE_ID) = 0;
        clear_lockon_mgr_state();
    }
}

static void clear_aim_assist_runtime()
{
    s_turnAuto = 0.0f;
    s_pitchAuto = 0.0f;
    s_lockBreakTimer = 0.0f;
    s_prevLockedEntity = 0;
    s_playerEntity = 0;
    s_autoLockTarget = 0;
    s_autoLockHandleId = 0;
}

// ---------------------------------------------------------------------------
// Hook: Damageable::ApplyDamage — auto-lock-on-hit
// ---------------------------------------------------------------------------

// Modtools: using 6-param base (__thiscall, RET 0x14)
using fn_ApplyDamage6 = bool(__thiscall*)(void* thisPtr, void* damageDesc,
    void* hitPos, void* hitDir, int param4, unsigned int param5);
static fn_ApplyDamage6 original_ApplyDamage6 = nullptr;

static void checkAutoLock(void* damageable, void* damageDesc)
{
    if (!s_autoLockOnHit || !s_playerEntity || s_currentWpnIsMelee || !is_joystick_connected())
        return;

    // Get shooter Entity* from DamageDesc.DamageOwner.mGameObject.mObject
    uintptr_t shooterEntity = *(uintptr_t*)((uintptr_t)damageDesc + DD_SHOOTER_ENTITY);
    if (!shooterEntity)
        return;

    // Only auto-lock when the LOCAL player is the shooter
    if (shooterEntity != s_playerEntity)
        return;

    // Get damaged entity (Damageable subobject is at Entity + 0x140)
    uintptr_t damagedEntity = (uintptr_t)damageable - DAMAGEABLE_TO_ENTITY;

    // Don't self-lock
    if (damagedEntity == s_playerEntity)
        return;

    // Store for PCUpdate to consume next frame
    s_autoLockTarget = damagedEntity;
    s_autoLockHandleId = *(uint32_t*)(damagedEntity + ENT_HANDLE_ID);
}

static bool __fastcall hooked_ApplyDamage6(void* thisPtr, void* /*edx*/,
    void* damageDesc, void* hitPos, void* hitDir, int param4, unsigned int param5)
{
    checkAutoLock(thisPtr, damageDesc);
    return original_ApplyDamage6(thisPtr, damageDesc, hitPos, hitDir, param4, param5);
}

// ---------------------------------------------------------------------------
// Hook: PlayerController::Update
// ---------------------------------------------------------------------------

// __thiscall, float dt param, RET 4
using fn_PlayerControllerUpdate = void(__thiscall*)(void* thisPtr, float dt);
static fn_PlayerControllerUpdate original_PCUpdate = nullptr;

static void __fastcall hooked_PCUpdate(void* thisPtr, void* /*edx*/, float dt)
{
    // Call original — writes mControlTurn/mControlPitch to Controllable
    original_PCUpdate(thisPtr, dt);

    // Base guards
    if (!s_aimAssistEnabled || !g_controllerEnabled || !is_joystick_connected()) {
        s_currentWpnIsMelee = false;
        s_playerEntity = 0;
        return;
    }

    // Get Controllable* from PlayerController
    uintptr_t ctrl = *(uintptr_t*)((char*)thisPtr + OFF_PC_OWNER);
    if (!ctrl) {
        s_currentWpnIsMelee = false;
        s_playerEntity = 0;
        return;
    }

    // --------------------------------------------------------------------
    // HARD GATE:
    // Aim assist is allowed ONLY when:
    //   - on foot
    //   - holding a valid ranged weapon
    // Everything else disables all aim assist behavior and clears lock.
    // --------------------------------------------------------------------

    // Vehicle check: read Character.mVehicle directly.
    bool inVehicle = false;
    {
        uintptr_t character = *(uintptr_t*)(ctrl + 0xCC); // Controllable.mCharacter
        if (character) {
            uintptr_t charVehicle = *(uintptr_t*)(character + 0x14C); // Character.mVehicle
            inVehicle = (charVehicle != 0);
        }
    }

    void* weapon = nullptr;
    bool hasRangedWeapon = false;
    s_currentWpnIsMelee = true; // default deny

    if (s_getCurWpn) {
        weapon = s_getCurWpn((void*)ctrl);
        if (weapon) {
            s_currentWpnIsMelee = weapon_is_melee(weapon);
            hasRangedWeapon = !s_currentWpnIsMelee;
        }
    }

    // Vehicle: disable all aim assist
    if (inVehicle) {
        clear_aim_assist_runtime();
        return;
    }

    // Set player entity for ApplyDamage hook (only when ranged — melee skips auto-lock)
    s_playerEntity = hasRangedWeapon ? (ctrl - CONTROLLABLE_TO_ENTITY) : 0;

    // --------------------------------------------------------------------
    // Auto-lock-on-hit: consume target set by ApplyDamage hook
    // Only auto-lock when NO target is currently locked.
    // --------------------------------------------------------------------

    if (s_autoLockOnHit && s_autoLockTarget && !s_currentWpnIsMelee) {
        uintptr_t autoTarget = s_autoLockTarget;
        uint32_t  autoHandleId = s_autoLockHandleId;
        s_autoLockTarget = 0;
        s_autoLockHandleId = 0;

        uintptr_t curLocked = *(uintptr_t*)(ctrl + OFF_TARGET_LOCKED);
        if (curLocked == 0 && *(uint32_t*)(autoTarget + ENT_HANDLE_ID) == autoHandleId) {
            float targetHealth = *(float*)(autoTarget + ENT_DAMAGEABLE_HEALTH);
            if (targetHealth > 0.0f) {
                if (s_setTargetLockedObj) {
                    s_setTargetLockedObj((void*)ctrl, (void*)autoTarget);
                }
                else {
                    *(uintptr_t*)(ctrl + OFF_TARGET_LOCKED) = autoTarget;
                    *(uint32_t*)(ctrl + OFF_TARGET_HANDLE_ID) = autoHandleId;
                    if (s_lockOnMgrArray) {
                        uintptr_t lockOnMgr = *(uintptr_t*)s_lockOnMgrArray;
                        if (lockOnMgr) {
                            *(uintptr_t*)(lockOnMgr + LOM_FORCE_TARGET) = autoTarget;
                            *(uint32_t*)(lockOnMgr + LOM_FORCE_HANDLE_ID) = autoHandleId;
                            *(int*)(lockOnMgr + LOM_FORCE_LOCK) = 5;
                        }
                    }
                }
            }
        }
    }

    // =====================================================================
    // Camera setup (needed for both proximity friction and locked-target)
    // =====================================================================

    uintptr_t camPtr = 0;
    if (s_cameraGlobal)
        camPtr = *(uintptr_t*)s_cameraGlobal;
    if (!camPtr) {
        s_turnAuto = 0.0f;
        s_pitchAuto = 0.0f;
        return;
    }

    float* camMatrix = (float*)(camPtr + 0x30);  // RedCamera._Matrix (4x4 row-major)
    Vec3 camRight = { camMatrix[0], camMatrix[1], camMatrix[2] };
    Vec3 camUp = { camMatrix[4], camMatrix[5], camMatrix[6] };
    Vec3 camFwd = { -camMatrix[8], -camMatrix[9], -camMatrix[10] };
    Vec3 camPos = { camMatrix[12], camMatrix[13], camMatrix[14] };
    float tanHFovW = *(float*)(camPtr + 0x144);
    float tanHFovH = tanHFovW * 0.75f;  // Xbox 4:3 aspect

    if (tanHFovW <= 0.0f)
        return;

    Vec3 playerEye = *(Vec3*)(ctrl + OFF_CTRL_AIM_START);
    Vec3 playerAimDir = vec3_normalize(*(Vec3*)(ctrl + OFF_CTRL_AIM_DIR));

    // Project crosshair (eyePoint + eyeDir * 1024, same as Xbox)
    Vec3 crosshairWorld = {
       playerEye.x + playerAimDir.x * 1024.0f,
       playerEye.y + playerAimDir.y * 1024.0f,
       playerEye.z + playerAimDir.z * 1024.0f
    };
    Vec3 crosshairRel = vec3_sub(crosshairWorld, camPos);
    float crosshairZ = vec3_dot(crosshairRel, camFwd);
    float crosshairScreenX = 0.0f, crosshairScreenY = 0.0f;
    if (crosshairZ > 0.01f) {
        crosshairScreenX = vec3_dot(crosshairRel, camRight) / (crosshairZ * tanHFovW);
        crosshairScreenY = vec3_dot(crosshairRel, camUp) / (crosshairZ * tanHFovH);
    }

    float& ctrlTurn = *(float*)(ctrl + OFF_CTRL_TURN);
    float& ctrlPitch = *(float*)(ctrl + OFF_CTRL_PITCH);

    // Save raw stick input before any friction (needed for lock break check)
    float origTurn = ctrlTurn;
    float origPitch = ctrlPitch;

    // =====================================================================
    // Proximity friction — omnidirectional slowdown near ANY enemy
    // =====================================================================

    if (s_proximityFriction && s_teamGetObjectsInRange && !s_currentWpnIsMelee) {
        uintptr_t playerEntity = ctrl - CONTROLLABLE_TO_ENTITY;
        uint32_t playerTeam = (*(uint32_t*)(playerEntity + ENT_TEAM_AND_TYPE)) & 0xF;

        static uintptr_t nearbyObjects[128];
        float queryPos[3] = { camPos.x, camPos.y, camPos.z };
        int count = s_teamGetObjectsInRange(queryPos, 100.0f, nearbyObjects, 128,
            nullptr, 0x02, playerEntity);

        float bestFriction = 1.0f;

        for (int i = 0; i < count; i++) {
            uintptr_t entity = nearbyObjects[i];
            if (!entity) continue;

            uint32_t entTeam = (*(uint32_t*)(entity + ENT_TEAM_AND_TYPE)) & 0xF;
            if (entTeam == 0 || entTeam == playerTeam) continue;  // neutral or friendly

            float hp = *(float*)(entity + ENT_DAMAGEABLE_HEALTH);
            if (hp <= 0.0f) continue;

            Vec3 tPos = getCollisionSpherePos(entity);
            Vec3 tRel = vec3_sub(tPos, camPos);
            float tZ = vec3_dot(tRel, camFwd);
            if (tZ <= 0.01f) continue;

            float tSX = vec3_dot(tRel, camRight) / (tZ * tanHFovW);
            float tSY = vec3_dot(tRel, camUp) / (tZ * tanHFovH);

            float dx = tSX - crosshairScreenX;
            float dy = tSY - crosshairScreenY;
            float dist = sqrtf(dx * dx + dy * dy);

            if (dist < s_proxFrictionRadius) {
                float t = dist / s_proxFrictionRadius;
                float friction = s_proxFrictionScale + (1.0f - s_proxFrictionScale) * t;
                if (friction < bestFriction)
                    bestFriction = friction;
            }
        }

        if (bestFriction < 1.0f) {
            ctrlTurn *= bestFriction;
            ctrlPitch *= bestFriction;
        }
    }

    // =====================================================================
    // Locked-target aim assist
    // =====================================================================

    uintptr_t lockedEntity = *(uintptr_t*)(ctrl + OFF_TARGET_LOCKED);
    if (lockedEntity == 0) {
        s_turnAuto = 0.0f;
        s_pitchAuto = 0.0f;
        s_lockBreakTimer = 0.0f;
        s_prevLockedEntity = 0;
        return;
    }

    uint32_t savedHandleId = *(uint32_t*)(ctrl + OFF_TARGET_HANDLE_ID);
    uint32_t actualHandleId = *(uint32_t*)(lockedEntity + ENT_HANDLE_ID);
    if (actualHandleId != savedHandleId) {
        s_turnAuto = 0.0f;
        s_pitchAuto = 0.0f;
        s_lockBreakTimer = 0.0f;
        s_prevLockedEntity = 0;
        return;
    }

    if (lockedEntity == (ctrl - CONTROLLABLE_TO_ENTITY)) {
        s_turnAuto = 0.0f;
        s_pitchAuto = 0.0f;
        s_lockBreakTimer = 0.0f;
        s_prevLockedEntity = 0;
        return;
    }

    float health = *(float*)(lockedEntity + ENT_DAMAGEABLE_HEALTH);
    if (health <= 0.0f) {
        s_turnAuto = 0.0f;
        s_pitchAuto = 0.0f;
        s_lockBreakTimer = 0.0f;
        s_prevLockedEntity = 0;
        return;
    }

    Vec3 targetPos = getCollisionSpherePos(lockedEntity);
    if (targetPos.x == 0.0f && targetPos.y == 0.0f && targetPos.z == 0.0f) {
        s_turnAuto = 0.0f;
        s_pitchAuto = 0.0f;
        return;
    }

    Vec3 toTarget = vec3_sub(targetPos, playerEye);
    Vec3 toTargetDir = vec3_normalize(toTarget);
    float targetDist = vec3_length(toTarget);

    float dot = vec3_dot(toTargetDir, playerAimDir);

    // --- Per-weapon auto-aim zone ---
    float wpnHorizThreshold = 0.0f;
    float wpnVertThreshold = 0.0f;

    if (weapon) {
        void* wpnClass = *(void**)((char*)weapon + 0x60);
        if (wpnClass) {
            wpnHorizThreshold = *(float*)((uintptr_t)wpnClass + s_wpnClassHorizThreshold);
            wpnVertThreshold = *(float*)((uintptr_t)wpnClass + s_wpnClassVertThreshold);
        }
    }

    if (wpnHorizThreshold <= 0.0f && wpnVertThreshold <= 0.0f) {
        constexpr float DEG2RAD = 3.14159265358979323846f / 180.0f;
        wpnHorizThreshold = wpnVertThreshold = sinf(s_coneAngle * DEG2RAD);
    }

    // Project locked target to screen space
    Vec3 targetRel = vec3_sub(targetPos, camPos);
    float targetZ = vec3_dot(targetRel, camFwd);
    if (targetZ <= 0.01f) {
        s_turnAuto = 0.0f;
        s_pitchAuto = 0.0f;
        return;
    }

    float targetScreenX = vec3_dot(targetRel, camRight) / (targetZ * tanHFovW);
    float targetScreenY = vec3_dot(targetRel, camUp) / (targetZ * tanHFovH);

    float hError = targetScreenX - crosshairScreenX;
    float vError = targetScreenY - crosshairScreenY;

    // Xbox: outside dot cone AND far away → zero vertical, double horizontal
    if (dot <= 0.7f && (dot <= 0.0f || targetDist >= 2.25f)) {
        if (dot <= 0.0f) {
            s_turnAuto = 0.0f;
            s_pitchAuto = 0.0f;
            return;
        }
        vError = 0.0f;
        hError = hError * 2.0f;
    }

    float outerH = wpnHorizThreshold * s_trackingDeadZone;
    float outerV = wpnVertThreshold * s_trackingDeadZone;

    // Dynamic vertical threshold (Xbox: scales by camera pitch position)
    {
        float vertMargin;
        if (vError < 0.0f)
            vertMargin = crosshairScreenY + 0.7f;
        else
            vertMargin = 0.7f - crosshairScreenY;
        if (vertMargin < 0.0f) vertMargin = 0.0f;
        outerV = (vertMargin / 0.7f) * outerV;
    }

    float frictionH = 0.7f;
    float frictionV;
    {
        float vertMargin;
        if (vError < 0.0f)
            vertMargin = crosshairScreenY + 0.7f;
        else
            vertMargin = 0.7f - crosshairScreenY;
        if (vertMargin < 0.0f) vertMargin = 0.0f;
        frictionV = vertMargin;
    }

    float hAbs = fabsf(hError);
    float vAbs = fabsf(vError);

    // Detect new lock — snap on first frame
    bool isNewLock = (lockedEntity != s_prevLockedEntity);
    if (isNewLock) {
        s_prevLockedEntity = lockedEntity;
        s_lockBreakTimer = 0.0f;
    }

    // =====================================================================
    // 1. Auto-correction target (outside tracking bubble → ramps 0 to 1)
    // =====================================================================

    float desiredTurnCorr = 0.0f;
    if (hAbs > outerH) {
        float dist = hAbs - outerH;
        desiredTurnCorr = fminf((dist / 1.3f) * 1.5f, 1.0f);
        if (hError < 0.0f) desiredTurnCorr = -desiredTurnCorr;
    }

    float desiredPitchCorr = 0.0f;
    if (vAbs > outerV) {
        float dist = vAbs - outerV;
        desiredPitchCorr = fminf((dist / 1.3f) * 1.5f, 1.0f);
        if (vError < 0.0f) desiredPitchCorr = -desiredPitchCorr;
    }

    // =====================================================================
    // 2. Auto-correction: instant snap on new lock, smooth ramp otherwise
    // =====================================================================

    if (isNewLock && s_snapStrength > 0.0f) {
        ctrlTurn = desiredTurnCorr * s_snapStrength;
        ctrlPitch = desiredPitchCorr * s_snapStrength;

        // Do NOT seed lingering auto-pull from the snap frame.
        s_turnAuto = 0.0f;
        s_pitchAuto = 0.0f;
        return;
    }
    else {
        float maxDelta = s_pullStrength * dt;

        if (desiredTurnCorr > s_turnAuto)
            s_turnAuto = fminf(s_turnAuto + maxDelta, desiredTurnCorr);
        else if (desiredTurnCorr < s_turnAuto)
            s_turnAuto = fmaxf(s_turnAuto - maxDelta, desiredTurnCorr);
        if (fabsf(s_turnAuto) < 0.001f) s_turnAuto = 0.0f;

        if (desiredPitchCorr > s_pitchAuto)
            s_pitchAuto = fminf(s_pitchAuto + maxDelta, desiredPitchCorr);
        else if (desiredPitchCorr < s_pitchAuto)
            s_pitchAuto = fmaxf(s_pitchAuto - maxDelta, desiredPitchCorr);
        if (fabsf(s_pitchAuto) < 0.001f) s_pitchAuto = 0.0f;
    }

    // =====================================================================
    // 3. Directional friction + auto-tracking
    // =====================================================================

    bool breaking = (s_lockBreakTimer > 0.0f);

    // --- Horizontal ---
    {
        float turnOut = 2.0f; // sentinel = "not set"

        if (ctrlTurn != 0.0f) {
            bool insideFriction = (hAbs < frictionH);
            bool sameDir = (ctrlTurn * s_turnAuto >= 0.0f);

            if (sameDir || insideFriction) {
                float friction;
                if (hError * ctrlTurn >= 0.0f) {
                    friction = 1.0f;
                }
                else if (frictionH <= 0.0f) {
                    friction = 1.0f;
                }
                else {
                    friction = ((frictionH - hAbs) / frictionH) * s_frictionStrength;
                    friction = fminf(fmaxf(friction, 0.0f), 1.0f);
                }
                turnOut = friction * ctrlTurn;
            }
        }

        if (turnOut >= 1.5f && s_turnAuto != 0.0f && !breaking) {
            turnOut = s_turnAuto;
        }

        if (turnOut < 1.5f) {
            ctrlTurn = turnOut;
        }
    }

    // --- Vertical ---
    {
        float pitchOut = 2.0f;

        if (ctrlPitch != 0.0f) {
            bool insideFriction = (vAbs < frictionV);
            bool sameDir = (ctrlPitch * s_pitchAuto >= 0.0f);

            if (sameDir || insideFriction) {
                float friction;
                if (vError * ctrlPitch >= 0.0f) {
                    friction = 1.0f;
                }
                else if (frictionV <= 0.0f) {
                    friction = 1.0f;
                }
                else {
                    friction = ((frictionV - vAbs) / frictionV) * s_frictionStrength;
                    friction = fminf(fmaxf(friction, 0.0f), 1.0f);
                }
                pitchOut = friction * ctrlPitch;
            }
        }

        if (pitchOut >= 1.5f && s_pitchAuto != 0.0f && !breaking) {
            pitchOut = s_pitchAuto;
        }

        if (pitchOut < 1.5f) {
            ctrlPitch = pitchOut;
        }
    }

    // =====================================================================
    // 4. Lock break — pushing away near edge for LockBreakTime → deselect
    // =====================================================================

    if (s_lockBreakTime > 0.0f) {
        bool breakH = (hError * origTurn < 0.0f) &&
            ((frictionH - hAbs < 0.05f) || (dot <= 0.73f));
        bool breakV = (vError * origPitch < 0.0f) &&
            ((frictionV - vAbs < 0.05f) || (dot <= 0.73f));

        if (breakH || breakV) {
            s_lockBreakTimer += dt;
            if (s_lockBreakTimer > s_lockBreakTime) {
                clear_locked_target(ctrl);
                s_turnAuto = 0.0f;
                s_pitchAuto = 0.0f;
                s_lockBreakTimer = 0.0f;
                s_prevLockedEntity = 0;
            }
        }
        else {
            s_lockBreakTimer = fmaxf(s_lockBreakTimer - dt * 3.0f, 0.0f);
        }
    }
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------

void aim_assist_install(uintptr_t exe_base)
{
    s_log = get_gamelog();

    if (!s_aimAssistEnabled) return;

    // TODO: select based on exe identification once global exe type detection exists
    s_addrs = &MODTOOLS_ADDRS;

    s_lockOnMgrArray = (uintptr_t)resolve(exe_base, s_addrs->lockon_mgr_array);
    s_cameraGlobal = (uintptr_t)resolve(exe_base, s_addrs->m_camera_global);
    s_getCurWpn = (fn_GetCurWpn)resolve(exe_base, s_addrs->get_cur_wpn);
    s_wpnClassVertThreshold = s_addrs->wpn_class_vert_threshold;
    s_wpnClassHorizThreshold = s_addrs->wpn_class_horiz_threshold;

    s_setTargetLockedObj = (fn_SetTargetLockedObj)resolve(exe_base, s_addrs->set_target_locked_obj);
    s_teamGetObjectsInRange = (fn_TeamGetObjectsInRange)resolve(exe_base, s_addrs->team_get_objects_in_range);

    original_PCUpdate = (fn_PlayerControllerUpdate)resolve(exe_base, s_addrs->player_controller_update);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)original_PCUpdate, hooked_PCUpdate);

    // Auto-lock-on-hit
    if (s_autoLockOnHit) {
        original_ApplyDamage6 = (fn_ApplyDamage6)resolve(exe_base, s_addrs->apply_damage);
        DetourAttach(&(PVOID&)original_ApplyDamage6, hooked_ApplyDamage6);
    }

    LONG result = DetourTransactionCommit();

    if (result != NO_ERROR) {
        if (s_log) s_log("[AimAssist] ERROR: Detours commit failed (%ld)\n", result);
        original_PCUpdate = nullptr;
        original_ApplyDamage6 = nullptr;
    }
}

void aim_assist_uninstall()
{
    if (!original_PCUpdate) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)original_PCUpdate, hooked_PCUpdate);
    if (original_ApplyDamage6)
        DetourDetach(&(PVOID&)original_ApplyDamage6, hooked_ApplyDamage6);
    DetourTransactionCommit();

    original_PCUpdate = nullptr;
    original_ApplyDamage6 = nullptr;
    s_getCurWpn = nullptr;
    s_setTargetLockedObj = nullptr;
    s_teamGetObjectsInRange = nullptr;
    s_playerEntity = 0;
    s_autoLockTarget = 0;
    s_autoLockHandleId = 0;
    s_prevLockedEntity = 0;
    s_turnAuto = 0.0f;
    s_pitchAuto = 0.0f;
    s_lockBreakTimer = 0.0f;
    s_currentWpnIsMelee = false;
}
