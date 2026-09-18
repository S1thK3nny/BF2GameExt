#include "pch.h"
#include "soldier_prone.hpp"
#include "core/resolve.hpp"

#include <cmath>
#include <cstdlib>
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
//      b. Restores the hint node prone stance: levels author a 3-bit stance
//         mask per hint node, but the engine masks the prone bit off. See
//         the hint node prone stance block below.
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
// Build-VARYING EntitySoldier offsets (mState, weaponIndex, weaponArray,
// soundPos2, foleyProne) live in SoldierLayout / g_soldier — see entity_layout.hpp.
// kCrouchTrigger is a Controllable-base field, build-invariant on all builds.
static constexpr int kCrouchTrigger = 0x48;  // Controllable::mControlCrouch (Controllable base == entity)

// SoldierAnimator offsets (Ghidra struct: SoldierAnimator, 8240 bytes)
// NOTE: mOwner (+0x50) stores struct_base, NOT entity (struct_base + 0x240).
// Use owner_to_entity() to convert.
static constexpr int kSAOwner       = 0x50;   // EntitySoldier* mOwner (== struct_base)
static constexpr int kSoldierAction = 0x70;   // SoldierState mSoldierAction
static constexpr int kMAction       = 0x1FEC; // ActionAnimation mAction
static constexpr int kMPosture      = 0x1FE8; // Posture mPosture

// BaseHint (64-byte pooled object, identical on all three builds).  The PDB
// declares one ushort bitfield at +0x3C:
//
//   bits 0-7   mType             Snipe=1, Patrol=2, Cover=4, JetJump=8,
//                                Mine=0x10, Land=0x20, Fortification=0x40,
//                                VehicleCover=0x80
//   bits 8-9   mPrimaryStance    2-bit mask, bit0=Stand, bit1=Crouch
//   bits 10-11 mSecondaryStance  same
//   bits 12-13 mSidestep         bit0=Left, bit1=Right
//   bits 14-15 unused            <- where the prone bits go, see below
static constexpr int kHintStanceWord = 0x3C;

static constexpr int kHintPrimaryShift   = 8;
static constexpr int kHintSecondaryShift = 10;
static constexpr uint16_t kHintPronePrimary   = 0x4000;
static constexpr uint16_t kHintProneSecondary = 0x8000;

// PblHash of the two hint node stance properties, as ProcessHint hands them to
// BaseHint::SetProperty.
static constexpr uint32_t kHintPropPrimaryStance   = 0xBF0F1CD1;
static constexpr uint32_t kHintPropSecondaryStance = 0x92C05A59;

// ControllableHeight (AILowLevel::mHeight): Stand=0, Crouch=1, Prone=2
static constexpr int HEIGHT_STAND = 0;

// Weapon foley id for the prone transition.  Crouch uses 8, prone 7.
static constexpr int kWeaponFoleyProne = 7;

// m_uiInputLockMask is 3 bits starting at bit 2; EntitySoldier::Prone sets all
// three (0x1C).  ApplyPush uses 0x3C, which also raises the neighbouring
// m_bSlide bit -- not wanted here.
static constexpr uint8_t kInputLockMaskBits = 0x1C;

// How long the getdown animation holds input.  The engine derives this as
// (clipFrames - 1) / 30 from the lower-body action animation; our prone.lvl
// ships crouch_getdown_prone and stand_getdown_prone at 40 frames, so 39/30.
// Only wrong if a replacement prone.lvl retimes those clips, and then only by
// the difference in clip length.
static constexpr float kProneGetdownSeconds = 39.0f / 30.0f;

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

// BaseHint::GetRandom{Primary,Secondary}Stance — ECX = BaseHint*, no arguments,
// returns a ControllableHeight.
typedef int (__fastcall* fn_HintStanceGetter_t)(void* ecx, void* edx);

// BaseHint::GetRandomStance — ECX = BaseHint* (unused by the body), stance mask
// as a byte-sized stack argument, callee-cleaned.
typedef int (__fastcall* fn_GetRandomStance_t)(void* ecx, void* edx, int mask);

// Weapon::PlayFoleyFX — ECX = Weapon*, foley id as a stack argument.
typedef void (__fastcall* fn_PlayFoleyFX_t)(void* ecx, void* edx, int id);

// FirstPerson::CrouchToProne — __cdecl, one stack argument (the first person
// index), no return.
typedef void (__cdecl* fn_FpCrouchToProne_t)(int index);

// BaseHint::SetProperty — ECX = BaseHint*, PblHash of the property name and its
// value as a string, callee-cleaned.
typedef void (__fastcall* fn_HintSetProperty_t)(void* ecx, void* edx, uint32_t hash, const char* value);


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

static fn_PlayFoleyFX_t      fn_playFoleyFX     = nullptr;
static fn_FpCrouchToProne_t  fn_fpCrouchToProne = nullptr;

static fn_HintStanceGetter_t original_GetRandomPrimaryStance   = nullptr;
static fn_HintStanceGetter_t original_GetRandomSecondaryStance = nullptr;
static fn_HintSetProperty_t  original_HintSetProperty          = nullptr;
static fn_GetRandomStance_t  fn_getRandomStance                = nullptr;

// BaseHint constructor stance-init mask patch (one byte)
static uint8_t* g_hintStanceInitPtr = nullptr;

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

// Lowres prone runtime dispatch patch (modtools: jump table entry)
static uint32_t* g_lowresProneJumpEntry = nullptr;
static uint32_t  g_lowresProneJumpOrig  = 0;

// Lowres prone runtime dispatch patch (Steam: MOV EBX imm byte in the
// dedicated prone case of GetAnimatorLocal)
static uint8_t* g_lowresProneCaseImmPtr = nullptr;

// Lowres crouch-idle patch: stops crouch from sharing prone's pose slot
static uint8_t* g_lowresCrouchIdlePtr     = nullptr;
static uint8_t  g_lowresCrouchIdleOrig[3] = {};
static size_t   g_lowresCrouchIdleLen     = 0;

// WeaponClass struct offsets
static constexpr int kWeaponClassOffset = 0x060;  // Weapon* -> WeaponClass*

// WeaponMeleeClass vtable pointer — resolved at install time.
// Identifying melee via WeaponClass+0x20 (mSoldierAnimationWeapon) would
// false-positive on CustomAnimationBank weapons: that property allocates a
// fresh WEAPON enum slot (>= 5) via FUN_00570cc0, so any non-melee weapon
// using CustomAnimationBank would look like melee.  Vtable identity is the
// authoritative check.
static void* g_weaponMeleeVtable = nullptr;

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
        uint8_t raw = *(uint8_t*)(base + g_soldier->weaponIndex);
        int slot = raw & 0x0F;
        if (slot >= 8) return false;
        void* weapon = *(void**)(base + g_soldier->weaponArray + slot * 4);
        if (!weapon) return false;
        void* weaponClass = *(void**)((char*)weapon + kWeaponClassOffset);
        if (!weaponClass) return false;
        // The first dword of any C++ object is its vtable pointer.
        void* vtable = *(void**)weaponClass;
        return vtable == g_weaponMeleeVtable;
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
        if (g_soldier->foleyProne && fn_getFoleyFX && fn_gameSoundPlay) {
            void* foley = fn_getFoleyFX(struct_base);
            if (foley) {
                void* sound = (char*)foley + g_soldier->foleyProne; // FoleyFXSoldier::mProne
                void* pos1  = struct_base + 0x120;                  // world pos (struct_base+0x120, same all builds)
                void* pos2  = (char*)entity + g_soldier->soundPos2;
                fn_gameSoundPlay(sound, pos1, pos2, 0, 1);
            }
        }

        // Weapon foley, the same call Crouch makes with id 8.  Uses the
        // engine's own slot guard: the active-slot nibble is read as a signed
        // 4-bit value, so an empty slot (8..15 -> negative) is rejected.
        if (fn_playFoleyFX) {
            int8_t slot = (int8_t)(*(uint8_t*)((char*)entity + g_soldier->weaponIndex) << 4);
            if (slot >= 0) {
                void* weapon = *(void**)((char*)entity + g_soldier->weaponArray + (slot >> 4) * 4);
                if (weapon) fn_playFoleyFX(weapon, nullptr, kWeaponFoleyProne);
            }
        }

        // Transition input lock.  EntitySoldier::Prone sets the full 3-bit
        // m_uiInputLockMask and holds it for the length of the getdown clip so
        // the soldier cannot walk or turn out of the animation; the engine's
        // own knockback path (ApplyPush) uses the same two fields, and ticks
        // the timer back down for us.
        {
            uint8_t* flags = (uint8_t*)((char*)entity + g_soldier->inputLockFlags);
            float*   timer = (float*)((char*)entity + g_soldier->inputLockTime);
            *flags |= kInputLockMaskBits;
            if (*timer < kProneGetdownSeconds) *timer = kProneGetdownSeconds;
        }

        // First-person transition (hands down + camera path).  Only the
        // modtools executable still carries it: the retail builds dropped the
        // prone FP camera paths, so fp_crouch_to_prone is 0 there and first
        // person simply keeps the stance pose it already had.
        //
        // The player can only ever reach prone from crouch (double-tap crouch),
        // so CrouchToProne is the whole first-person story; AI entering prone
        // from standing has no first-person view to update.
        if (fn_fpCrouchToProne) {
            // EntitySoldier::GetFirstPersonIndex: bits 4-5 of the weapon-index
            // byte, sign-extended, so 3 (both bits set) reads as -1 = not in
            // first person.
            int fpIndex = (int8_t)(*(uint8_t*)((char*)entity + g_soldier->weaponIndex) << 2) >> 6;
            if (fpIndex >= 0) fn_fpCrouchToProne(fpIndex);
        }
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
    int state = *(int*)((char*)ecx + g_soldier->mState);

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
    int state = *(int*)((char*)ecx + g_soldier->mState);

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
//
// MUST be register-transparent: the accessor is a 2-instruction leaf that
// clobbers only EAX, and the Steam LTCG build has callers that rely on that —
// Combo::ResolveForWeapon (0x4757a0) keeps the pointer it passes in ECX alive
// in EDX ACROSS the call and dereferences EDX afterwards.  A plain C hook
// compiled by MSVC treats EDX as volatile and zeroes it, so the caller then
// reads [0+4] -> AV at 0x4757ad whenever a melee weapon resolves its combos
// (i.e. instantly on Jedi spawn).  Hence naked asm: touch only EAX, tail-jump
// to the Detours trampoline for the non-null path.
// ---------------------------------------------------------------------------
static __declspec(naked) unsigned short __fastcall hooked_animAccessor(void* /*ecx*/, void* /*edx*/)
{
    __asm {
        test ecx, ecx
        jz   ret_zero
        jmp  dword ptr [original_animAccessor]
    ret_zero:
        xor  eax, eax
        ret
    }
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
// Hint node prone stances
//
// ZeroEditor writes PrimaryStance()/SecondaryStance() into every hint node it
// exports, and the authored value is a 3-bit stance mask: bit0 Stand, bit1
// Crouch, bit2 Prone (SecondaryStance packs the 2-bit sidestep mask on top, at
// bits 3-4, which is why the engine range-checks the value against 0x20).
// Pandemic's own levels use the prone bit -- end1.hnt has PrimaryStance(4)
// (prone only) and tat2.hnt has PrimaryStance(6) (crouch or prone).
//
// The engine throws that bit away.  BaseHint stores each stance two bits wide
// (see the bitfield above), and BaseHint::SetProperty masks the parsed value
// with & 3 before storing it.  A prone-only hint masks to 0, which SetProperty
// treats as "no value given" and skips, leaving the constructor's Stand.  So
// prone hint nodes have always behaved as stand/crouch ones.
//
// The two unused top bits of the bitfield are where the dropped prone bits go:
//
//   1. The constructor's stance-init mask (AND 0xC5FF) is patched to 0x05FF so
//      bits 14-15 start cleared instead of holding memory-pool garbage.
//   2. SetProperty is hooked: after the original runs, the authored value is
//      re-parsed and its prone bit stored in bit 14 (primary) or bit 15
//      (secondary).  For a prone-only hint the vanilla 2-bit field is cleared
//      too, since the original left it at the default Stand.
//   3. GetRandomPrimaryStance / GetRandomSecondaryStance are hooked to rebuild
//      the full 3-bit mask and hand that to BaseHint::GetRandomStance, which
//      already rolls GetRandomInt(0, 2) over all three heights.
//
// Consumers: CoverHelper and SnipeHelper EnterState state 3 take the primary
// stance, CoverHelper states 4 and 5 the secondary; both end up in
// AILowLevel::mHeight, whose prone case reaches Prone() through the height
// dispatch patch in the installer below.
//
// With prone off for the mission (no prone.lvl) the prone bit is filtered back
// out, and a mask left empty by that falls back to Stand -- which is what the
// vanilla engine did with those hints anyway.
// ---------------------------------------------------------------------------
static int hint_roll_stance(void* hint, bool primary)
{
    const uint16_t word = *(const uint16_t*)((const char*)hint + kHintStanceWord);
    const int shift     = primary ? kHintPrimaryShift : kHintSecondaryShift;
    const uint16_t bit  = primary ? kHintPronePrimary : kHintProneSecondary;

    int mask = (word >> shift) & 3;
    if (g_proneEnabled && (word & bit))
        mask |= 4;

    // Only reachable for a prone-only hint with prone off: the engine would
    // spin forever on an empty mask, so answer Stand as vanilla effectively did.
    if (mask == 0) return HEIGHT_STAND;

    return fn_getRandomStance(hint, nullptr, mask);
}

static int __fastcall hooked_GetRandomPrimaryStance(void* ecx, void* edx)
{
    if (!ecx) return original_GetRandomPrimaryStance(ecx, edx);
    return hint_roll_stance(ecx, true);
}

static int __fastcall hooked_GetRandomSecondaryStance(void* ecx, void* edx)
{
    if (!ecx) return original_GetRandomSecondaryStance(ecx, edx);
    return hint_roll_stance(ecx, false);
}

// Runs for every PROP chunk of every hint node at world load.  The original
// applies the vanilla fields (including the sidestep bits and the command post
// cross-check warnings); this only adds the prone bit it discards.
static void __fastcall hooked_HintSetProperty(void* ecx, void* edx, uint32_t hash, const char* value)
{
    original_HintSetProperty(ecx, edx, hash, value);

    if (!ecx || !value) return;
    if (hash != kHintPropPrimaryStance && hash != kHintPropSecondaryStance) return;

    const bool primary  = (hash == kHintPropPrimaryStance);
    const int  authored = atoi(value);
    const int  stance   = authored & 7;

    uint16_t* word     = (uint16_t*)((char*)ecx + kHintStanceWord);
    const int shift    = primary ? kHintPrimaryShift : kHintSecondaryShift;
    const uint16_t bit = primary ? kHintPronePrimary : kHintProneSecondary;

    if (stance & 4) {
        *word |= bit;
        // Prone only: the original skipped its write, so the 2-bit field still
        // holds the constructor's Stand. Clear it or the hint reads as
        // stand-or-prone.
        if ((stance & 3) == 0)
            *word &= (uint16_t)~(3u << shift);
    } else {
        *word &= (uint16_t)~bit;
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
    if (g_build == GameBuild::Unknown) return;
    if (g_addr->EntitySoldier_crouch == 0 || g_addr->EntitySoldier_stand == 0 ||
        g_addr->EntitySoldier_SetState == 0 || g_addr->EntitySoldier_prone == 0)
        return;

    // Per-build EntitySoldier offsets come from g_soldier (entity_layout.hpp),
    // already selected by game_build_select().  Controllable / SoldierAnimator /
    // Weapon offsets used below are build-invariant constants.

    // Resolve function pointers
    original_Crouch    = (fn_Stance_t)resolve(exe_base, g_addr->EntitySoldier_crouch);
    original_StandUp   = (fn_Stance_t)resolve(exe_base, g_addr->EntitySoldier_stand);
    fn_setState        = (fn_SetState_t)resolve(exe_base, g_addr->EntitySoldier_SetState);
    fn_getFoleyFX      = (fn_GetFoleyFX_t)resolve(exe_base, g_addr->FoleyFXCollider_GetFoleyFX);
    fn_gameSoundPlay   = (fn_GameSoundPlay_t)resolve(exe_base, g_addr->GameSound_play);

    original_animAccessor = (fn_AnimAccessor_t)resolve(exe_base, g_addr->prone_anim_accessor);
    original_SetAction    = (fn_SetAction_t)resolve(exe_base, g_addr->SoldierAnimator_SetAction);

    g_weaponMeleeVtable   = (void*)resolve(exe_base, g_addr->WeaponMeleeClass_vftable);

    if (g_addr->weapon_play_foley_fx)
        fn_playFoleyFX = (fn_PlayFoleyFX_t)resolve(exe_base, g_addr->weapon_play_foley_fx);
    if (g_addr->fp_crouch_to_prone)
        fn_fpCrouchToProne = (fn_FpCrouchToProne_t)resolve(exe_base, g_addr->fp_crouch_to_prone);

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
        uint8_t* pJnz = (uint8_t*)resolve(exe_base, g_addr->prone_guard_jnz);
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
        uint8_t* p = (uint8_t*)resolve(exe_base, g_addr->prone_acklay_gate_jnz);
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
    g_proneVtableSlotPtr = (void**)resolve(exe_base, g_addr->EntitySoldier_prone);
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
    // Original dispatch at case 1 (Crouch), where <reg> holds `this` (ESI on
    // modtools, EDI on Steam — see SoldierLayout::aiHeightBaseRm):
    //   8B 1<rm>     MOV EDX, [<reg>]     ; vtable
    //   8B C<rm>     MOV ECX, <reg>       ; this
    //   FF 92 9C..   CALL [EDX + 0x9C]    ; Crouch()
    //   E9 95..      JMP end_of_switch
    //
    // Our stub does the same but calls [EDX + 0xA0] (Prone).  The `this`
    // register must match the patched build's dispatch site or the stub reads a
    // garbage vtable and crashes.
    // -----------------------------------------------------------------------
    {
        uintptr_t switchEnd = (uintptr_t)resolve(exe_base, g_addr->prone_height_switch_end);
        const uint8_t rm = g_soldier->aiHeightBaseRm; // 6 = ESI (modtools), 7 = EDI (Steam)

        g_proneDispatchStub = (uint8_t*)VirtualAlloc(
            nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (g_proneDispatchStub) {
            uint8_t* p = g_proneDispatchStub;
            // MOV EDX, [<reg>]   (8B /r, reg=EDX=010, mod=00)
            *p++ = 0x8B; *p++ = (uint8_t)(0x10 | rm);
            // MOV ECX, <reg>     (8B /r, reg=ECX=001, mod=11)
            *p++ = 0x8B; *p++ = (uint8_t)(0xC8 | rm);
            // CALL [EDX + 0xA0]
            *p++ = 0xFF; *p++ = 0x92;
            *p++ = 0xA0; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
            // JMP rel32 -> end_of_switch
            *p++ = 0xE9;
            int32_t rel = (int32_t)(switchEnd - ((uintptr_t)p + 4));
            memcpy(p, &rel, 4);

            // Patch jump table entry [2] to point to our stub
            g_heightJumpTableEntry = (uint32_t*)resolve(exe_base, g_addr->prone_height_jump_table + 8);
            g_heightJumpTableOrig = *g_heightJumpTableEntry;
            *g_heightJumpTableEntry = (uint32_t)(uintptr_t)g_proneDispatchStub;
        }
    }

    // -----------------------------------------------------------------------
    // AI prone fix 2: carry the hint node prone stance bit that the engine
    // discards — see the hint node prone stance block above.
    //
    // Not gated on g_proneEnabled: that flag is per-mission and still false
    // here, so the bit is always captured at load and filtered at use.
    // -----------------------------------------------------------------------
    if (g_addr->hint_get_primary_stance && g_addr->hint_get_secondary_stance &&
        g_addr->hint_get_random_stance && g_addr->hint_set_property &&
        g_addr->hint_stance_init_mask_byte) {

        // Clear the two spare bitfield bits on construction: the engine's
        // stance-init mask keeps them, so they would otherwise carry whatever
        // the previous owner of that memory pool block left behind.
        //   AND <reg>, 0xC5FF  ->  AND <reg>, 0x05FF
        uint8_t* pMask = (uint8_t*)resolve(exe_base, g_addr->hint_stance_init_mask_byte);
        if (*pMask == 0xC5) {
            *pMask = 0x05;
            g_hintStanceInitPtr = pMask;
        }

        fn_getRandomStance = (fn_GetRandomStance_t)resolve(exe_base, g_addr->hint_get_random_stance);
        original_GetRandomPrimaryStance =
            (fn_HintStanceGetter_t)resolve(exe_base, g_addr->hint_get_primary_stance);
        original_GetRandomSecondaryStance =
            (fn_HintStanceGetter_t)resolve(exe_base, g_addr->hint_get_secondary_stance);
        original_HintSetProperty =
            (fn_HintSetProperty_t)resolve(exe_base, g_addr->hint_set_property);

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)original_GetRandomPrimaryStance, hooked_GetRandomPrimaryStance);
        DetourAttach(&(PVOID&)original_GetRandomSecondaryStance, hooked_GetRandomSecondaryStance);
        DetourAttach(&(PVOID&)original_HintSetProperty, hooked_HintSetProperty);
        DetourTransactionCommit();
    }

    // -----------------------------------------------------------------------
    // Lowres prone animation fix: patch the animation name table.
    //
    // SoldierAnimatorLowResClass::PostLoad looks up lowres animations by name.
    // Index 2 (prone) uses "rifle_crouch_idle_takeknee" which doesn't exist
    // in the shipped lowres banks, so it falls back to crouch.  Patch the
    // table pointer to use "rifle_prone_idle_emote" instead.
    //
    // Gated on the INI value, unlike the rest of this installer.  Everything
    // else here funnels through our own hooks, which re-check g_proneEnabled at
    // runtime and no-op when prone is off; this table is read by engine code we
    // do not hook, so leaving it patched with Prone=0 makes PostLoad warn
    // ("Can't find lowres animation ..., using CROUCH") for every lowres bank
    // that lacks the prone clip.
    //
    // g_proneEnabled still holds the configured value here: it is the *live*
    // per-mission flag, and prone_lvl_load_install — which captures it into
    // s_proneConfigured and then clears it — runs on the NEXT line of dllmain.
    // -----------------------------------------------------------------------
    if (g_proneEnabled && g_addr->lowres_prone_anim_name_ptr) {
        g_lowresProneNamePtr = (const char**)resolve(exe_base, g_addr->lowres_prone_anim_name_ptr);
        g_lowresProneNameOrig = *g_lowresProneNamePtr;
        *g_lowresProneNamePtr = g_lowresProneAnimName;
    }

    // -----------------------------------------------------------------------
    // Lowres prone runtime dispatch fix.
    //
    // GetAnimatorLocal has a switch on mState; the PRONE case selects the
    // CROUCH lowres pose (index 1) instead of pose 2 (whose name-table entry
    // we patch to the prone anim above).  The fix differs per build:
    //
    //   modtools: repoint the jump table entry for case 2 at the existing
    //   "MOV ESI,2" case body.
    //
    //   Steam (0x648ff0): a byte map (0x649368) routes state 2 to its OWN
    //   dedicated case @0x6491C9 = "MOV EBX,1; JMP merge" — no other state
    //   maps there, so just patch the MOV immediate 1 -> 2.
    //
    // Pose slot 2 is not free, though: the CROUCH case picks it too, for a
    // soldier who is crouching and standing still (crouching while moving uses
    // slot 1).  So renaming slot 2 to the prone clip also puts every still,
    // crouching soldier into the prone pose at low LOD.  There is no unused
    // static slot to move to — PostLoad only reads entries 0..5 of the name
    // table (stand_idle, crouch_idle_emote, crouch_idle_takeknee, jetpack_hover,
    // divefoward, thrown_flail) and everything from 6 up is the dynamic
    // script-animation range — so instead send crouch-idle to slot 1 as well
    // and leave slot 2 to prone.  Cost is that a still crouching soldier at low
    // LOD uses the "emote" crouch idle instead of "takeknee"; both are crouch
    // idles, and this is the distant-LOD model only.
    //
    // Skipped with the name-table patch above when prone is off: pointing prone
    // at pose slot 2 only means anything once slot 2 holds the prone clip name.
    // -----------------------------------------------------------------------
    if (g_proneEnabled && g_addr->lowres_crouch_idle_branch) {
        uint8_t* p = (uint8_t*)resolve(exe_base, g_addr->lowres_crouch_idle_branch);
        if (g_build == GameBuild::Modtools) {
            // JNZ 0x588575 (75 07) -> NOP NOP: fall through to MOV ESI,1.
            if (p[0] == 0x75 && p[1] == 0x07) {
                memcpy(g_lowresCrouchIdleOrig, p, 2);
                g_lowresCrouchIdlePtr = p;
                g_lowresCrouchIdleLen = 2;
                p[0] = 0x90; p[1] = 0x90;
            }
        } else {
            // SETBE BL (0F 96 C3) -> XOR BL,BL + NOP, so the INC EBX that
            // follows always yields pose 1.  Byte-identical on Steam and GOG.
            if (p[0] == 0x0F && p[1] == 0x96 && p[2] == 0xC3) {
                memcpy(g_lowresCrouchIdleOrig, p, 3);
                g_lowresCrouchIdlePtr = p;
                g_lowresCrouchIdleLen = 3;
                p[0] = 0x30; p[1] = 0xDB; p[2] = 0x90;
            }
        }
    }

    if (g_proneEnabled) {
        if (g_build == GameBuild::Modtools &&
            g_addr->lowres_prone_jump_entry && g_addr->lowres_prone_jump_target) {
            g_lowresProneJumpEntry = (uint32_t*)resolve(exe_base, g_addr->lowres_prone_jump_entry);
            g_lowresProneJumpOrig = *g_lowresProneJumpEntry;
            uintptr_t target = (uintptr_t)resolve(exe_base, g_addr->lowres_prone_jump_target);
            *g_lowresProneJumpEntry = (uint32_t)target;
        }
        else if (g_addr->lowres_prone_case_imm) {
            uint8_t* p = (uint8_t*)resolve(exe_base, g_addr->lowres_prone_case_imm);
            if (*p == 0x01) {
                *p = 0x02;
                g_lowresProneCaseImmPtr = p;
            }
        }
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

    // Restore AI height dispatch jump table.
    // NOTE: uninstall runs at DLL detach, long after install_patches_impl
    // re-protected the exe sections — every restore below must go through
    // protected_write or it access-violates (jump tables live in .text).
    if (g_heightJumpTableEntry && g_heightJumpTableOrig) {
        protected_write(g_heightJumpTableEntry, &g_heightJumpTableOrig,
                        sizeof(g_heightJumpTableOrig));
    }

    // Free AI dispatch stub
    if (g_proneDispatchStub) {
        VirtualFree(g_proneDispatchStub, 0, MEM_RELEASE);
        g_proneDispatchStub = nullptr;
    }

    // Restore the hint node stance-init mask
    if (g_hintStanceInitPtr) {
        const uint8_t orig = 0xC5;
        protected_write(g_hintStanceInitPtr, &orig, 1);
        g_hintStanceInitPtr = nullptr;
    }

    // Restore Acklay gate patch
    if (g_acklayGatePtr) {
        protected_write(g_acklayGatePtr, g_acklayGateOrig, 6);
        g_acklayGatePtr = nullptr;
    }

    // Restore lowres prone animation name
    if (g_lowresProneNamePtr && g_lowresProneNameOrig) {
        protected_write(g_lowresProneNamePtr, &g_lowresProneNameOrig,
                        sizeof(g_lowresProneNameOrig));
        g_lowresProneNamePtr = nullptr;
    }

    // Restore lowres prone runtime dispatch
    if (g_lowresProneJumpEntry && g_lowresProneJumpOrig) {
        protected_write(g_lowresProneJumpEntry, &g_lowresProneJumpOrig,
                        sizeof(g_lowresProneJumpOrig));
        g_lowresProneJumpEntry = nullptr;
    }
    if (g_lowresProneCaseImmPtr) {
        const uint8_t orig = 0x01;
        protected_write(g_lowresProneCaseImmPtr, &orig, 1);
        g_lowresProneCaseImmPtr = nullptr;
    }

    // Restore lowres crouch-idle branch
    if (g_lowresCrouchIdlePtr) {
        protected_write(g_lowresCrouchIdlePtr, g_lowresCrouchIdleOrig, g_lowresCrouchIdleLen);
        g_lowresCrouchIdlePtr = nullptr;
    }

    // Detach hooks
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (original_Crouch)       DetourDetach(&(PVOID&)original_Crouch, hooked_Crouch);
    if (original_StandUp)      DetourDetach(&(PVOID&)original_StandUp, hooked_StandUp);
    if (original_animAccessor) DetourDetach(&(PVOID&)original_animAccessor, hooked_animAccessor);
    if (original_SetAction)    DetourDetach(&(PVOID&)original_SetAction, hooked_SetAction);
    if (original_GetRandomPrimaryStance)
        DetourDetach(&(PVOID&)original_GetRandomPrimaryStance, hooked_GetRandomPrimaryStance);
    if (original_GetRandomSecondaryStance)
        DetourDetach(&(PVOID&)original_GetRandomSecondaryStance, hooked_GetRandomSecondaryStance);
    if (original_HintSetProperty)
        DetourDetach(&(PVOID&)original_HintSetProperty, hooked_HintSetProperty);
    DetourTransactionCommit();
}
