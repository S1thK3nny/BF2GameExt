#include "pch.h"
#include "directional_roll_patch.hpp"
#include "lua_hooks.hpp"

#include <detours.h>
#include <cmath>
#include <cstdio>
#include <cstring>

// =============================================================================
// Directional Roll
// =============================================================================
// Redirects the diveforward animation (mAction=DIVE=24) to diveleft/diveright
// when a soldier rolls while strafing. Uses two Detours hooks:
//
//   1. SoldierAnimator::SetAction — post-call: if mAction==DIVE, check strafe
//      direction via dot(mLegMatrix.right, mMovement). If strafe-dominant, set
//      s_pending_dir = DIVE_LEFT(50) or DIVE_RIGHT(51). Clear when not rolling.
//
//   2. SoldierAnimatorClass::GetLowerBodyActionAnimation /
//      GetUpperBodyActionAnimation — when anim==DIVE and s_pending_dir is set,
//      return the cached directional animation instead. Falls back to diveforward
//      if no diveleft/diveright anim is in the bank's parent chain.
//
// Animation naming: "human_rifle_diveleft_full" derived from template mName
// by replacing "diveforward" with "diveleft"/"diveright".
//
// Note: UpdateActionAnimation hardcodes param_2=DIVE=24 from EntitySoldier::Update
// and does not read mAction. However, GetLowerBodyActionAnimation IS reached with
// that param_2=DIVE — so we intercept there directly via s_pending_dir.

// ---------------------------------------------------------------------------
// Custom action indices
// ---------------------------------------------------------------------------

static constexpr int DIVE       = 24;
static constexpr int DIVE_LEFT  = 50;
static constexpr int DIVE_RIGHT = 51;

// ---------------------------------------------------------------------------
// SoldierAnimator field offsets (identical for all builds)
// ---------------------------------------------------------------------------

// PblMatrix layout (64 bytes, 4 × PblVector4):
//   +0:  right   (row 0)
//   +16: up      (row 1)
//   +32: forward (row 2)
//   +48: trans   (row 3)
// mLegMatrix is at SoldierAnimator+0.
static constexpr unsigned OFF_LEGMATRIX_RIGHT   =  0;
static constexpr unsigned OFF_LEGMATRIX_FORWARD = 32;
static constexpr unsigned OFF_MOVEMENT          = 0xAC;   // PblVector3
static constexpr unsigned OFF_MACTION           = 0x1FEC; // int32_t

// ---------------------------------------------------------------------------
// Bank struct layout (s_aBank array, stride 0x2C = 44 bytes)
// ---------------------------------------------------------------------------

static constexpr unsigned BANK_STRIDE     = 0x2C;
static constexpr unsigned BANK_OFF_NAME   = 0;   // char[32]
static constexpr unsigned BANK_OFF_PARENT = 32;  // int, -1 = no parent

// ---------------------------------------------------------------------------
// SoldierAnimation (16 bytes, returned by GetLower/UpperBodyActionAnimation)
// ---------------------------------------------------------------------------

struct SoldierAnimation {
    void*       m_pZephyrAnim;
    uint32_t    u4;
    void*       m_pAnimData;
    const char* mName;
};

// ---------------------------------------------------------------------------
// Per-bank cache of diveleft / diveright SoldierAnimation objects
// ---------------------------------------------------------------------------

struct DivePair {
    SoldierAnimation lower;
    SoldierAnimation upper;
};

struct DiveCache {
    DivePair left;
    DivePair right;
    bool     init;
};

// 16 slots covers all weapon banks in BF2 (typically ~10)
static DiveCache s_cache[16];

// Pending directional redirect: 0=none, DIVE_LEFT=50, DIVE_RIGHT=51.
// Set by hook_set_action when a strafe-dominant roll starts.
// Cleared when SetAction fires with a non-DIVE action (roll ended).
static int s_pending_dir = 0;

// ---------------------------------------------------------------------------
// CRC-32 hash — matches PBLTEMPHASH / ZephyrAnimBank lookup (big-endian,
// polynomial 0x04C11DB7, case-insensitive for A-Z only)
// ---------------------------------------------------------------------------

static uint32_t anim_crc32(const char* str)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (; *str; ++str) {
        uint8_t c = (uint8_t)*str;
        if (c >= 'A' && c <= 'Z') c += 0x20u;
        uint8_t  idx   = (uint8_t)(crc >> 24) ^ c;
        uint32_t entry = (uint32_t)idx << 24;
        for (int i = 0; i < 8; ++i)
            entry = (entry & 0x80000000u) ? (entry << 1) ^ 0x04C11DB7u : (entry << 1);
        crc = (crc << 8) ^ entry;
    }
    return ~crc;
}

// ---------------------------------------------------------------------------
// Per-build addresses
// ---------------------------------------------------------------------------

struct DirRollAddrs {
    uintptr_t fn_set_action;
    uintptr_t fn_get_lower;
    uintptr_t fn_get_upper;
    uintptr_t fn_get_animation;      // SoldierAnimatorClass::GetAnimation
    bool      get_animation_has_name; // modtools=true (hash+name), retail=false (hash only)
    uintptr_t g_sbank;               // s_aBank array
};

static constexpr DirRollAddrs MODTOOLS_ADDRS = {
    .fn_set_action          = 0x575d50,
    .fn_get_lower           = 0x413e1c,
    .fn_get_upper           = 0x4137a5,
    .fn_get_animation       = 0x57de40,
    .get_animation_has_name = true,
    .g_sbank                = 0xacecf8,
};

static constexpr DirRollAddrs STEAM_ADDRS = {
    .fn_set_action          = 0x63ed60,
    .fn_get_lower           = 0x643960,
    .fn_get_upper           = 0x643940,
    .fn_get_animation       = 0x6442a0,
    .get_animation_has_name = false,
    .g_sbank                = 0x7e9440,
};

static constexpr DirRollAddrs GOG_ADDRS = {
    .fn_set_action          = 0x63fe00,
    .fn_get_lower           = 0x644a00,
    .fn_get_upper           = 0x6449e0,
    .fn_get_animation       = 0x645340,
    .get_animation_has_name = false,
    .g_sbank                = 0x7ea070,
};

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------

// SetAction: __thiscall (SoldierAnimator*, SoldierState, PblMatrix*, uint)
using fn_SetAction   = void(__thiscall*)(void*, int, void*, uint32_t);
// GetLower/UpperBodyActionAnimation: __thiscall (SoldierAnimatorClass*, actionIdx, bank)
using fn_GetBodyAnim = SoldierAnimation*(__thiscall*)(void*, int, int);
// GetAnimation (modtools): __thiscall (SoldierAnimatorClass*, hash, name)
using fn_GetAnimMT   = void*(__thiscall*)(void*, uint32_t, const char*);
// GetAnimation (retail): __thiscall (SoldierAnimatorClass*, hash)
using fn_GetAnimRet  = void*(__thiscall*)(void*, uint32_t);

static fn_SetAction   orig_set_action = nullptr;
static fn_GetBodyAnim orig_get_lower  = nullptr;
static fn_GetBodyAnim orig_get_upper  = nullptr;

static void*       s_get_animation          = nullptr;
static bool        s_get_animation_has_name = false;
static const char* s_sbank                  = nullptr; // resolved s_aBank base address

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void* call_get_animation(void* class_ptr, uint32_t hash, const char* name)
{
    if (s_get_animation_has_name)
        return ((fn_GetAnimMT)s_get_animation)(class_ptr, hash, name);
    else
        return ((fn_GetAnimRet)s_get_animation)(class_ptr, hash);
}

// Derive a dive animation name by replacing "diveforward" in the template's mName.
// e.g. template "human_rifle_diveforward_full" + new_action "diveleft"
//   -> "human_rifle_diveleft_full"
// Returns false if "diveforward" is not present in template_name.
static bool make_dive_name(char* out, size_t out_size,
                             const char* template_name, const char* new_action)
{
    const char* pos = strstr(template_name, "diveforward");
    if (!pos) return false;
    size_t prefix_len = (size_t)(pos - template_name);
    snprintf(out, out_size, "%.*s%s%s",
             (int)prefix_len, template_name,
             new_action,
             pos + 11 /* len("diveforward") */);
    return true;
}

// Lazily populate the cache for this bank. class_ptr = SoldierAnimatorClass*.
// Walks the bank parent chain; for each ancestor bank, derives the diveleft/diveright
// animation name from that bank's diveforward template mName by string substitution.
static void lazy_init(void* class_ptr, int bank)
{
    if (s_cache[bank].init) return;
    s_cache[bank].init = true;

    // Template from the target bank — used for blend data (m_pAnimData, u4).
    SoldierAnimation* tmpl_lower = orig_get_lower(class_ptr, DIVE, bank);
    SoldierAnimation* tmpl_upper = orig_get_upper(class_ptr, DIVE, bank);
    if (!tmpl_lower || !tmpl_upper) {
        dbg_log("[DirRoll] bank %d: diveforward template null\n", bank);
        return;
    }

    void* zeph_left  = nullptr;
    void* zeph_right = nullptr;

    for (int cur = bank; cur >= 0 && cur < 64; ) {
        SoldierAnimation* sl = orig_get_lower(class_ptr, DIVE, cur);
        if (sl && sl->mName) {
            char left_name[128], right_name[128];
            if (make_dive_name(left_name,  sizeof(left_name),  sl->mName, "diveleft") &&
                make_dive_name(right_name, sizeof(right_name), sl->mName, "diveright")) {

                if (!zeph_left) {
                    zeph_left = call_get_animation(class_ptr, anim_crc32(left_name), left_name);
                    dbg_log_verbose("[DirRoll] bank %d (cur=%d): \"%s\" -> %s\n",
                        bank, cur, left_name, zeph_left ? "FOUND" : "not found");
                }
                if (!zeph_right) {
                    zeph_right = call_get_animation(class_ptr, anim_crc32(right_name), right_name);
                    dbg_log_verbose("[DirRoll] bank %d (cur=%d): \"%s\" -> %s\n",
                        bank, cur, right_name, zeph_right ? "FOUND" : "not found");
                }
            }
        }
        if (zeph_left && zeph_right) break;

        int parent = *(const int*)(s_sbank + cur * BANK_STRIDE + BANK_OFF_PARENT);
        if (parent < 0 || parent == cur) break;
        cur = parent;
    }

    if (zeph_left) {
        s_cache[bank].left.lower = *tmpl_lower;
        s_cache[bank].left.lower.m_pZephyrAnim = zeph_left;
        s_cache[bank].left.upper = *tmpl_upper;
        s_cache[bank].left.upper.m_pZephyrAnim = zeph_left;
    }
    if (zeph_right) {
        s_cache[bank].right.lower = *tmpl_lower;
        s_cache[bank].right.lower.m_pZephyrAnim = zeph_right;
        s_cache[bank].right.upper = *tmpl_upper;
        s_cache[bank].right.upper.m_pZephyrAnim = zeph_right;
    }

    if (!zeph_left && !zeph_right)
        dbg_log("[DirRoll] bank %d: no diveleft/diveright anims found\n", bank);
    else
        dbg_log("[DirRoll] bank %d: left=%s right=%s\n", bank,
                zeph_left ? "ok" : "missing", zeph_right ? "ok" : "missing");
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

// Post-call: if the original set mAction=DIVE, compute strafe direction and
// set s_pending_dir. Clear s_pending_dir on any non-DIVE action.
static void __fastcall hook_set_action(void* ecx, void* /*edx*/,
                                        int state, void* mat, uint32_t flags)
{
    orig_set_action(ecx, state, mat, flags);

    int32_t mAction = *(int32_t*)((char*)ecx + OFF_MACTION);
    if (mAction != DIVE) {
        s_pending_dir = 0;
        return;
    }

    // mLegMatrix is at +0; forward is row 2 at +32, right is row 0 at +0.
    // mMovement was computed inside SetAction as (new_pos - old_pos).
    const float* right   = (const float*)((char*)ecx + OFF_LEGMATRIX_RIGHT);
    const float* forward = (const float*)((char*)ecx + OFF_LEGMATRIX_FORWARD);
    const float* mv      = (const float*)((char*)ecx + OFF_MOVEMENT);

    float strafe = right[0]*mv[0] + right[1]*mv[1] + right[2]*mv[2];
    float fwd    = forward[0]*mv[0] + forward[1]*mv[1] + forward[2]*mv[2];

    dbg_log("[DirRoll] ROLL: strafe=%.4f fwd=%.4f mv=(%.4f,%.4f,%.4f)\n",
            strafe, fwd, mv[0], mv[1], mv[2]);

    // Redirect if strafe component > 50% of forward component, with a minimum
    // magnitude check to ignore standing-still rolls.
    if (fabsf(strafe) > fabsf(fwd) * 0.5f && fabsf(strafe) > 0.01f) {
        s_pending_dir = (strafe < 0.0f) ? DIVE_LEFT : DIVE_RIGHT;
        dbg_log("[DirRoll] -> %s\n", (s_pending_dir == DIVE_LEFT) ? "DIVE_LEFT" : "DIVE_RIGHT");
    } else {
        s_pending_dir = 0;
    }
}

// When anim==DIVE and s_pending_dir is set, return the cached directional
// animation instead. Falls back to diveforward if no directional anim found.
static SoldierAnimation* __fastcall hook_get_lower(void* ecx, void* /*edx*/,
                                                     int anim, int bank)
{
    if (anim == DIVE && s_pending_dir != 0 && (unsigned)bank < 16u) {
        lazy_init(ecx, bank);
        DivePair& pair = (s_pending_dir == DIVE_LEFT) ? s_cache[bank].left : s_cache[bank].right;
        if (pair.lower.m_pZephyrAnim)
            return &pair.lower;
    }
    return orig_get_lower(ecx, anim, bank);
}

static SoldierAnimation* __fastcall hook_get_upper(void* ecx, void* /*edx*/,
                                                     int anim, int bank)
{
    if (anim == DIVE && s_pending_dir != 0 && (unsigned)bank < 16u) {
        lazy_init(ecx, bank);
        DivePair& pair = (s_pending_dir == DIVE_LEFT) ? s_cache[bank].left : s_cache[bank].right;
        if (pair.upper.m_pZephyrAnim)
            return &pair.upper;
    }
    return orig_get_upper(ecx, anim, bank);
}

// ---------------------------------------------------------------------------
// Install / Uninstall
// ---------------------------------------------------------------------------

void directional_roll_install(uintptr_t exe_base)
{
    const DirRollAddrs* addrs = nullptr;
    switch (g_exeType) {
        case ExeType::MODTOOLS: addrs = &MODTOOLS_ADDRS; break;
        case ExeType::STEAM:    addrs = &STEAM_ADDRS;    break;
        case ExeType::GOG:      addrs = &GOG_ADDRS;      break;
        default:
            dbg_log("[DirRoll] Skipping — unsupported build\n");
            return;
    }

    auto resolve = [=](uintptr_t addr) -> uintptr_t {
        return addr - 0x400000u + exe_base;
    };

    memset(s_cache, 0, sizeof(s_cache));
    s_pending_dir = 0;

    s_get_animation_has_name = addrs->get_animation_has_name;
    s_get_animation          = (void*)resolve(addrs->fn_get_animation);
    s_sbank                  = (const char*)resolve(addrs->g_sbank);

    orig_set_action = (fn_SetAction)resolve(addrs->fn_set_action);
    orig_get_lower  = (fn_GetBodyAnim)resolve(addrs->fn_get_lower);
    orig_get_upper  = (fn_GetBodyAnim)resolve(addrs->fn_get_upper);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)orig_set_action, hook_set_action);
    DetourAttach(&(PVOID&)orig_get_lower,  hook_get_lower);
    DetourAttach(&(PVOID&)orig_get_upper,  hook_get_upper);
    LONG result = DetourTransactionCommit();

    if (result == NO_ERROR)
        dbg_log("[DirRoll] Hooks installed\n");
    else
        dbg_log("[DirRoll] ERROR: Detours commit failed (%ld)\n", result);
}

void directional_roll_uninstall()
{
    if (!orig_set_action) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)orig_set_action, hook_set_action);
    DetourDetach(&(PVOID&)orig_get_lower,  hook_get_lower);
    DetourDetach(&(PVOID&)orig_get_upper,  hook_get_upper);
    DetourTransactionCommit();

    dbg_log("[DirRoll] Hooks removed\n");
}
