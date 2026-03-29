#include "pch.h"
#include "soldier_prone.hpp"
#include "core/resolve.hpp"

#include <cmath>
#include <detours.h>

// =============================================================================
// Prone stance system
//
// Wires up the unused PRONE soldier state (SoldierState 2) that Pandemic
// stubbed out before shipping BF2.
//
//   1. Stance cycling: Detours Crouch (0x00543B60) so the crouch key
//      cycles STAND -> CROUCH -> PRONE -> STAND.  StandUp (0x005435D0)
//      is hooked as a passthrough (needed for original_StandUp pointer).
//
//   2. Patches the Controllable vtable Prone slot (offset 0xA0) from the
//      vanilla "return false" stub to a real function that enters prone.
//
//   3. Melee weapon guard: blocks prone entry when holding a melee weapon,
//      and forces out of prone if the soldier switches to one mid-prone.
//
//   4. AI prone support:
//      a. Patches the height dispatch jump table so HEIGHT_PRONE (case 2)
//         calls Prone() instead of Crouch().
//      b. Patches GetRandomPrimaryStance to extract 3 bits (& 7) instead
//         of 2 (& 3), allowing the Prone stance bit to be read from hints.
//
//   5. Acklay terrain alignment fix: patches the gate condition in
//      PostCollisionUpdate (0x0052C0F0) so the prone-specific terrain
//      alignment block never runs.  That block raycasts front/back of the
//      soldier to build a surface-aligned orientation matrix, but its yaw
//      computation causes continuous rotation on slopes.  Using an inline
//      patch (JNZ -> JMP) instead of a Detours hook avoids crashing on
//      the function's SSE stack-alignment prologue (AND ESP, 0xFFFFFFF0).
// =============================================================================

bool g_proneEnabled = false;

// SoldierState enum values
static constexpr int STATE_STAND  = 0;
static constexpr int STATE_CROUCH = 1;
static constexpr int STATE_PRONE  = 2;

// EntitySoldier offsets (from entity ptr = struct_base + 0x240)
// Ghidra struct: EntitySoldier (4080 bytes).
//   entity+0x4F0 = struct+0x730  Weapon*[8]  mWeapon
//   entity+0x512 = struct+0x752  char[2]     mWeaponIndex  (low nibble = slot)
//   entity+0x514 = struct+0x754  SoldierState mState
//   entity+0x520 = struct+0x760  SoldierAnimator*
//   entity+0x218 = struct+0x458  EntitySoldierClass*
static constexpr int kMState        = 0x514; // SoldierState mState
static constexpr int kWeaponSlot    = 0x512; // mWeaponIndex byte (low nibble = slot)
static constexpr int kWeaponArray   = 0x4F0; // Weapon*[8] mWeapon
static constexpr int kCrouchTrigger = 0x48;  // Controllable::mControlCrouch (Controllable base == entity)

// SoldierAnimator offsets (Ghidra struct: SoldierAnimator, 8240 bytes)
// NOTE: mOwner (+0x50) stores struct_base, NOT entity (struct_base + 0x240).
// Use owner_to_entity() to convert.
static constexpr int kSAOwner       = 0x50;   // EntitySoldier* mOwner (== struct_base)
static constexpr int kSoldierAction = 0x70;   // SoldierState mSoldierAction
static constexpr int kMAction       = 0x1FEC; // ActionAnimation mAction
static constexpr int kMPosture      = 0x1FE8; // Posture mPosture

// ActionAnimation enum values for prone transitions (confirmed from PDB + SetupPose switch table)
static constexpr int ACTION_PRONE_TO_STAND  = 27; // 0x1B
static constexpr int ACTION_CROUCH_TO_PRONE = 28; // 0x1C
static constexpr int ACTION_PRONE_TO_CROUCH = 29; // 0x1D


// ---------------------------------------------------------------------------
// Function pointer types
// ---------------------------------------------------------------------------

// __fastcall mirrors __thiscall for Detours: ECX=this, EDX=unused
typedef bool (__fastcall* fn_Stance_t)(void* ecx, void* edx);

// Pure __thiscall — ECX=this, params on stack
typedef bool (__thiscall* fn_SetState_t)(void* adjusted, int state);
typedef void* (__thiscall* fn_GetFoleyFX_t)(void* adjusted);
typedef void (__thiscall* fn_GameSoundPlay_t)(void* gs, void* pos1, void* pos2, int a, int b);


// Animation accessor at 0x005701F0: reads (*param)->field_8, returns ushort.
// Crashes when param (ECX) is null — happens when PRONE animation data is missing.
typedef unsigned short (__fastcall* fn_AnimAccessor_t)(void* ecx, void* edx);

// SoldierAnimator::SetAction — called once per frame from EntitySoldier::Render.
// Sets mPosture, mAction, mSoldierAction based on the entity's current SoldierState.
typedef void (__fastcall* fn_SetAction_t)(void* ecx, void* edx, int param_2, void* param_3, unsigned int param_4);


// ---------------------------------------------------------------------------
// Resolved pointers (set during install)
// ---------------------------------------------------------------------------
static fn_Stance_t           original_Crouch    = nullptr;
static fn_Stance_t           original_StandUp   = nullptr;
static fn_SetState_t         fn_setState        = nullptr;
static fn_GetFoleyFX_t       fn_getFoleyFX      = nullptr;
static fn_GameSoundPlay_t    fn_gameSoundPlay   = nullptr;

static fn_AnimAccessor_t     original_animAccessor = nullptr;
static fn_SetAction_t        original_SetAction    = nullptr;

// Vtable patch state
static void** g_proneVtableSlotPtr  = nullptr;
static void*  g_proneVtableSlotOrig = nullptr;

// AI height dispatch: allocated code cave + saved jump table entry
static uint8_t* g_proneDispatchStub     = nullptr;
static uint32_t g_heightJumpTableOrig   = 0;
static uint32_t* g_heightJumpTableEntry = nullptr;

// Acklay terrain alignment gate patch (6 bytes at kAcklayGateJnz)
static uint8_t* g_acklayGatePtr        = nullptr;
static uint8_t  g_acklayGateOrig[6]    = {};

// Lowres prone animation name patch
static const char* g_lowresProneAnimName = "rifle_prone_idle_emote";
static const char** g_lowresProneNamePtr = nullptr;
static const char*  g_lowresProneNameOrig = nullptr;

// WeaponClass struct offsets
static constexpr int kWeaponClassOffset = 0x060;  // Weapon* -> WeaponClass*
static constexpr int kSoldierAnimWeapon = 0x020;  // WeaponClass -> WEAPON mSoldierAnimationWeapon enum
static constexpr int WEAPON_TYPE_MELEE  = 4;       // melee weapon type

// ---------------------------------------------------------------------------
// owner_to_entity — converts SoldierAnimator::mOwner (struct_base) to entity
// ---------------------------------------------------------------------------
static inline void* owner_to_entity(void* owner)
{
    return (char*)owner + 0x240;
}

// ---------------------------------------------------------------------------
// is_melee_weapon — checks if the soldier's active weapon is melee
//
// Reads the active weapon slot, fetches the WeaponClass*, and checks
// WeaponClass+0x020 (mSoldierAnimationWeapon) == 4 (melee).
// ---------------------------------------------------------------------------
static bool is_melee_weapon(void* entity)
{
    __try {
        char* base = (char*)entity;
        // The byte at +0x512 encodes the weapon slot in its low nibble.
        // The game extracts it via (SHL 4, SAR 4) — equivalent to & 0x0F
        // for non-negative values.
        uint8_t raw = *(uint8_t*)(base + kWeaponSlot);
        int slot = raw & 0x0F;
        if (slot >= 8) return false;
        void* weapon = *(void**)(base + kWeaponArray + slot * 4);
        if (!weapon) return false;
        void* weaponClass = *(void**)((char*)weapon + kWeaponClassOffset);
        if (!weaponClass) return false;
        int weaponType = *(int*)((char*)weaponClass + kSoldierAnimWeapon);
        // MELEE = 4 in WEAPON enum; custom weapon types (>= 5) like
        // lightsabers are also melee.  Block all types >= MELEE.
        return weaponType >= WEAPON_TYPE_MELEE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// do_prone_transition — enters prone state, plays sounds
//
// Modeled on Crouch inner (0x00543B60):
//   1. SetState(struct_base, PRONE)
//   2. Play soldier FoleyFX stance sound
//   3. Play weapon FoleyFX
//
// Takes the ENTITY pointer (struct_base + 0x240), same as ECX in hooks.
// ---------------------------------------------------------------------------
static bool do_prone_transition(void* entity)
{
    if (!g_proneEnabled) return false;

    // Melee weapons don't have prone animations — block entry
    if (is_melee_weapon(entity)) return false;

    char* struct_base = (char*)entity - 0x240;

    if (!fn_setState) return false;
    bool ok = fn_setState(struct_base, STATE_PRONE);
    if (!ok) return false;

    __try {
        // FoleyFXSoldier::mProne (GameSound at +0xD8, confirmed in Ghidra struct)
        if (fn_getFoleyFX && fn_gameSoundPlay) {
            void* foley = fn_getFoleyFX(struct_base);
            if (foley) {
                void* sound = (char*)foley + 0xD8; // FoleyFXSoldier::mProne
                void* pos1  = struct_base + 0x120; // world pos
                void* pos2  = (char*)entity + 0x2AC;
                fn_gameSoundPlay(sound, pos1, pos2, 0, 1);
            }
        }

        // Weapon foley skipped for prone transitions — the weapon slot can
        // hold a stale pointer after SetState(PRONE), and any dereference
        // faults under the debug heap (x32dbg).  The body sound
        // (FoleyFXSoldier::mProne above) is the audible stance cue anyway.
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    return true;
}

// ---------------------------------------------------------------------------
// hooked_Crouch — handles PRONE -> STAND exit.
//
// The game's toggle logic: if state != CROUCH -> Crouch(), else StandUp().
// So Crouch() fires for STAND->CROUCH and PRONE->??? (since PRONE != CROUCH).
// We only intervene for PRONE; STAND->CROUCH is vanilla.
//
// Reentrance guard: EntitySoldier::Stand does a headroom check for PRONE.
// If headroom is blocked (e.g. under low ceiling), Stand falls back to
// vtable[0x9C] (Crouch) which re-enters this hook.  The guard detects the
// re-entry and lets original_Crouch run — doing SetState(CROUCH), which is
// the correct fallback (lower headroom requirement than STAND).
// ---------------------------------------------------------------------------
static bool __fastcall hooked_Crouch(void* ecx, void* /*edx*/)
{
    int state = *(int*)((char*)ecx + kMState);

    if (state == STATE_PRONE) {
        // PRONE -> STAND (with headroom-blocked fallback to CROUCH)
        static bool s_inStandFromProne = false;
        if (s_inStandFromProne) {
            return original_Crouch(ecx, nullptr);
        }
        s_inStandFromProne = true;
        bool result = original_StandUp(ecx, nullptr);
        s_inStandFromProne = false;
        return result;
    }

    // STAND -> CROUCH (vanilla behavior)
    return original_Crouch(ecx, nullptr);
}

// ---------------------------------------------------------------------------
// hooked_StandUp — double-tap crouch enters PRONE.
//
// The game calls StandUp() when state == CROUCH and the crouch key is pressed.
// The Trigger state machine at entity+0x60 (mControlCrouch) tracks press
// timing: bit 4 (0x10) is set on a quick press→release→press sequence.
//
// Flow: first tap STAND→CROUCH (vanilla, via hooked_Crouch passthrough).
// Second tap within the double-press window → StandUp called → we check
// bit 4 → enter PRONE.  Single tap from CROUCH → vanilla STAND.
//
// PlayerController also calls StandUp() before Jump when state is
// STAND/SPRINT — no trigger check needed, those pass through unchanged.
// ---------------------------------------------------------------------------
static bool __fastcall hooked_StandUp(void* ecx, void* /*edx*/)
{
    int state = *(int*)((char*)ecx + kMState);

    if (state == STATE_CROUCH) {
        // Check the crouch Trigger for double-tap (bit 4)
        uint32_t trigger = *(uint32_t*)((char*)ecx + kCrouchTrigger);
        if (trigger & 0x10) {
            if (do_prone_transition(ecx))
                return true;
        }
    }

    // Single tap CROUCH -> STAND, or STAND/SPRINT -> STAND (jump path)
    return original_StandUp(ecx, nullptr);
}

// ---------------------------------------------------------------------------
// hooked_animAccessor — null-guard for the animation accessor at 0x005701F0.
//
// The original function dereferences ECX without checking for null.  When the
// soldier is in PRONE state but prone animations aren't loaded, the animation
// system passes a null SoldierAnimation* here and crashes.  Returning 0
// (same as the function's own fallback for *param==0) lets the animation
// system degrade gracefully instead of crashing.
// ---------------------------------------------------------------------------
static unsigned short __fastcall hooked_animAccessor(void* ecx, void* /*edx*/)
{
    if (!ecx) return 0;
    return original_animAccessor(ecx, nullptr);
}

// ---------------------------------------------------------------------------
// hooked_SetAction — transition animations and melee guard.
//
// Called once per frame from EntitySoldier::Render.  Two responsibilities:
//
//   1. Pandemic defined ActionAnimation values for prone transitions
//      (CROUCH_TO_PRONE=28, PRONE_TO_STAND=27, PRONE_TO_CROUCH=29) and
//      wired up the playback side in SetupPose, but SetAction never writes
//      them.  The vanilla code uses MOVE+IDLE for entering prone and
//      LAND_HARD for leaving prone.  This post-hook overrides mAction with
//      the correct transition value.
//
//   2. Per-frame melee guard: if the soldier is in PRONE and the active
//      weapon is melee, force the state to STAND.
// ---------------------------------------------------------------------------
static void __fastcall hooked_SetAction(void* ecx, void* edx, int param_2, void* param_3, unsigned int param_4)
{
    if (!g_proneEnabled) {
        original_SetAction(ecx, edx, param_2, param_3, param_4);
        return;
    }

    // Per-frame melee guard (uses entity ptr, not struct_base)
    if (param_2 == STATE_PRONE) {
        void* owner = *(void**)((char*)ecx + kSAOwner);
        if (owner) {
            void* entity = owner_to_entity(owner);
            if (is_melee_weapon(entity) && original_StandUp)
                original_StandUp(entity, nullptr);
        }
    }

    // Save old soldier action before original overwrites it
    int oldState = *(int*)((char*)ecx + kSoldierAction);

    // Let original SetAction run (sets mPosture, mAction, mSoldierAction)
    original_SetAction(ecx, edx, param_2, param_3, param_4);

    // Only fix up if the state actually changed and involves PRONE
    if (oldState == param_2) return;

    int* pAction = (int*)((char*)ecx + kMAction);

    if (oldState == STATE_CROUCH && param_2 == STATE_PRONE) {
        *pAction = ACTION_CROUCH_TO_PRONE;
    } else if (oldState == STATE_PRONE && param_2 == STATE_STAND) {
        *pAction = ACTION_PRONE_TO_STAND;
    } else if (oldState == STATE_PRONE && param_2 == STATE_CROUCH) {
        *pAction = ACTION_PRONE_TO_CROUCH;
    }
}

// ---------------------------------------------------------------------------
// vtable_Prone — replaces the vanilla "return false" stub at vtable+0xA0
//
// Called by AI posture system or any code that explicitly invokes Prone().
// ---------------------------------------------------------------------------
static bool __fastcall vtable_Prone(void* ecx, void* /*edx*/)
{
    return do_prone_transition(ecx);
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------
void prone_system_install(uintptr_t exe_base)
{
    using namespace game_addrs::modtools;

    // Resolve function pointers
    original_Crouch    = (fn_Stance_t)resolve(exe_base, prone_crouch_inner);
    original_StandUp   = (fn_Stance_t)resolve(exe_base, prone_standup_inner);
    fn_setState        = (fn_SetState_t)resolve(exe_base, prone_set_state);
    fn_getFoleyFX      = (fn_GetFoleyFX_t)resolve(exe_base, prone_get_foley_fx);
    fn_gameSoundPlay   = (fn_GameSoundPlay_t)resolve(exe_base, prone_game_sound_play);

    original_animAccessor = (fn_AnimAccessor_t)resolve(exe_base, prone_anim_accessor);
    original_SetAction    = (fn_SetAction_t)resolve(exe_base, prone_set_action);

    // Detour Crouch, StandUp, animation accessor, SetAction
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)original_Crouch, hooked_Crouch);
    DetourAttach(&(PVOID&)original_StandUp, hooked_StandUp);
    DetourAttach(&(PVOID&)original_animAccessor, hooked_animAccessor);
    DetourAttach(&(PVOID&)original_SetAction, hooked_SetAction);
    DetourTransactionCommit();

    // Patch out the PRONE guard in EntitySoldier::Update.
    // Pandemic left a hardcoded check: if (mState == PRONE) Crouch();
    // Change the JNZ (0x75) to JMP (0xEB) so the Crouch() call is always skipped.
    {
        uint8_t* pJnz = (uint8_t*)resolve(exe_base, prone_guard_jnz);
        if (*pJnz == 0x75)
            *pJnz = 0xEB;
    }

    // -----------------------------------------------------------------------
    // Acklay terrain alignment fix: patch the gate in PostCollisionUpdate.
    //
    // PostCollisionUpdate (0x0052C0F0) has an SSE stack-alignment prologue
    // (AND ESP, 0xFFFFFFF0) that makes Detours crash — inline patch instead.
    //
    // The Acklay block (dual raycasts + SetWorldMatrix) only runs when
    // mStanceIndex == 2 (PRONE), gated at 0x0052C285:
    //
    //   AND DL, 0xC0          ; isolate mStanceIndex (top 2 bits of +0x752)
    //   CMP DL, 0x80          ; == PRONE (2 << 6)?
    //   JNZ 0x0052C653        ; skip if not prone
    //
    // Pandemic's prone terrain alignment causes continuous yaw rotation on
    // slopes.  Patch the JNZ to an unconditional JMP so the Acklay block
    // never runs.  Collision accumulator normalization and network sync
    // (both earlier/later in the same function) are preserved.
    // -----------------------------------------------------------------------
    {
        uint8_t* p = (uint8_t*)resolve(exe_base, prone_acklay_gate_jnz);
        if (p[0] == 0x0F && p[1] == 0x85) {
            memcpy(g_acklayGateOrig, p, 6);
            g_acklayGatePtr = p;

            // JNZ rel32 (6 bytes: 0F 85 xx xx xx xx) -> JMP rel32 (5 bytes) + NOP
            // JMP next_ip is 1 byte earlier than JNZ, so rel offset += 1
            int32_t jnzRel;
            memcpy(&jnzRel, p + 2, 4);
            int32_t jmpRel = jnzRel + 1;

            p[0] = 0xE9;
            memcpy(p + 1, &jmpRel, 4);
            p[5] = 0x90;
        }
    }

    // Patch Controllable vtable: Prone slot (offset 0xA0)
    g_proneVtableSlotPtr = (void**)resolve(exe_base, prone_vtable_slot);
    g_proneVtableSlotOrig = *g_proneVtableSlotPtr;
    *g_proneVtableSlotPtr = (void*)&vtable_Prone;

    // -----------------------------------------------------------------------
    // AI prone fix 1: Patch the height dispatch jump table.
    //
    // EntitySoldier::UpdateIndirect has a switch on AILowLevel::mHeight.
    // Case 2 (HEIGHT_PRONE) erroneously jumps to the same code as case 1
    // (HEIGHT_CROUCH), calling Crouch() instead of Prone().  We allocate
    // a small code cave that calls vtable[0xA0] (Prone) and patch the
    // jump table entry for case 2 to point there.
    //
    // Original dispatch at case 1 (Crouch):
    //   8B 16        MOV EDX, [ESI]       ; vtable
    //   8B CE        MOV ECX, ESI         ; this
    //   FF 92 9C..   CALL [EDX + 0x9C]    ; Crouch()
    //   E9 95..      JMP end_of_switch
    //
    // Our stub does the same but calls [EDX + 0xA0] (Prone).
    // -----------------------------------------------------------------------
    {
        uintptr_t switchEnd = (uintptr_t)resolve(exe_base, prone_height_switch_end);

        g_proneDispatchStub = (uint8_t*)VirtualAlloc(
            nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (g_proneDispatchStub) {
            uint8_t* p = g_proneDispatchStub;
            // MOV EDX, [ESI]
            *p++ = 0x8B; *p++ = 0x16;
            // MOV ECX, ESI
            *p++ = 0x8B; *p++ = 0xCE;
            // CALL [EDX + 0xA0]
            *p++ = 0xFF; *p++ = 0x92;
            *p++ = 0xA0; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
            // JMP rel32 -> end_of_switch
            *p++ = 0xE9;
            int32_t rel = (int32_t)(switchEnd - ((uintptr_t)p + 4));
            memcpy(p, &rel, 4);

            // Patch jump table entry [2] to point to our stub
            g_heightJumpTableEntry = (uint32_t*)resolve(exe_base, prone_height_jump_table + 8);
            g_heightJumpTableOrig = *g_heightJumpTableEntry;
            *g_heightJumpTableEntry = (uint32_t)(uintptr_t)g_proneDispatchStub;
        }
    }

    // -----------------------------------------------------------------------
    // AI prone fix 2: Patch GetRandomPrimaryStance bitmask extraction.
    //
    // The function reads the stance bitmask with AND EAX, 0xFFFFFF03 (& 3),
    // which masks out the Prone bit (bit 2).  Change the AND immediate from
    // 0x03 to 0x07 so all three stance bits (Stand, Crouch, Prone) are kept.
    // -----------------------------------------------------------------------
    {
        uint8_t* pAnd = (uint8_t*)resolve(exe_base, prone_primary_stance_and);
        if (*pAnd == 0x03)
            *pAnd = 0x07;
    }

    // -----------------------------------------------------------------------
    // Lowres prone animation fix: patch the animation name table.
    //
    // SoldierAnimatorLowResClass::PostLoad looks up lowres animations by name.
    // Index 2 (prone) uses "rifle_crouch_idle_takeknee" which doesn't exist
    // in the shipped lowres banks, so it falls back to crouch.  Patch the
    // table pointer to use "rifle_prone_idle_emote" instead.
    // -----------------------------------------------------------------------
    {
        g_lowresProneNamePtr = (const char**)resolve(exe_base, lowres_prone_anim_name_ptr);
        g_lowresProneNameOrig = *g_lowresProneNamePtr;
        *g_lowresProneNamePtr = g_lowresProneAnimName;
    }

}

void prone_system_uninstall()
{
    // Restore vtable entry
    if (g_proneVtableSlotPtr && g_proneVtableSlotOrig) {
        DWORD oldProt;
        if (VirtualProtect(g_proneVtableSlotPtr, sizeof(void*), PAGE_READWRITE, &oldProt)) {
            *g_proneVtableSlotPtr = g_proneVtableSlotOrig;
            VirtualProtect(g_proneVtableSlotPtr, sizeof(void*), oldProt, &oldProt);
        }
    }

    // Restore AI height dispatch jump table
    if (g_heightJumpTableEntry && g_heightJumpTableOrig) {
        *g_heightJumpTableEntry = g_heightJumpTableOrig;
    }

    // Free AI dispatch stub
    if (g_proneDispatchStub) {
        VirtualFree(g_proneDispatchStub, 0, MEM_RELEASE);
        g_proneDispatchStub = nullptr;
    }

    // Restore Acklay gate patch
    if (g_acklayGatePtr) {
        memcpy(g_acklayGatePtr, g_acklayGateOrig, 6);
        g_acklayGatePtr = nullptr;
    }

    // Restore lowres prone animation name
    if (g_lowresProneNamePtr && g_lowresProneNameOrig) {
        *g_lowresProneNamePtr = g_lowresProneNameOrig;
        g_lowresProneNamePtr = nullptr;
    }

    // Detach hooks
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (original_Crouch)       DetourDetach(&(PVOID&)original_Crouch, hooked_Crouch);
    if (original_StandUp)      DetourDetach(&(PVOID&)original_StandUp, hooked_StandUp);
    if (original_animAccessor) DetourDetach(&(PVOID&)original_animAccessor, hooked_animAccessor);
    if (original_SetAction)    DetourDetach(&(PVOID&)original_SetAction, hooked_SetAction);
    DetourTransactionCommit();
}
