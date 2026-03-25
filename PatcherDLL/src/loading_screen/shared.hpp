#pragma once

// Internal shared header for loading_screen — not for external inclusion.

#include "loading_screen.hpp"

#include <cstring>

// =============================================================================
// Function pointer types
// =============================================================================

// PblConfig API — all __thiscall, mirrored as __fastcall
typedef void  (__fastcall* fn_pbl_ctor_t)      (void* ecx, void* edx, uint32_t* fh);
typedef void  (__fastcall* fn_pbl_copy_ctor_t) (void* ecx, void* edx, void* parent, int share_fh);
typedef void  (__fastcall* fn_pbl_read_data_t) (void* ecx, void* edx, void* data_buf);
typedef void* (__fastcall* fn_pbl_read_scope_t)(void* ecx, void* edx, void* temp_buf);

typedef void* (__cdecl* fn_find_by_hash_t)(uint32_t hash);
typedef void  (__cdecl* fn_snd_play_t)(int src3D, void* props, int cb, int userdata, int param5);
typedef void* (__cdecl* fn_snd_play_ex_t)(void* src3D, void* props, void* cb, void* userdata, int param5);
typedef uint32_t (__cdecl* fn_voice_to_handle_t)(void* voice);
typedef void (__fastcall* fn_vvrelease_t)(void* ecx, void* edx);
typedef void (__cdecl* fn_snd_eng_update_t)(float dt, char full);

typedef void (__stdcall* fn_prt_t)(
    uint32_t tex_hash,
    float x0, float y0, float x1, float y1,
    void* color_ptr, int alpha_blend,
    float u0, float v0, float u1, float v1,
    float r, float g, float b, float a
);

typedef int (__cdecl* fn_set_current_heap_t)(int heap);
typedef void (__fastcall* fn_load_data_file_t)(void* ecx, void* edx, const char* lvlPath);
typedef void* (__cdecl* fn_pbl_find_t)(void* table, uint32_t size, uint32_t hash);
typedef uint32_t (__cdecl* fn_hash_string_t)(const char* str);

typedef void (__fastcall* fn_load_config_t)  (void* ecx, void* edx, uint32_t* fh);
typedef void (__fastcall* fn_render_screen_t)(void* ecx, void* edx);
typedef void (__fastcall* fn_load_end_t)     (void* ecx, void* edx);
typedef void (__fastcall* fn_set_all_on_t)   (void* ecx, void* edx);
typedef void (__fastcall* fn_load_update_t)  (void* ecx, void* edx);
typedef void (__fastcall* fn_load_render_t)  (void* ecx, void* edx);

// =============================================================================
// GameLog
// =============================================================================

typedef void (__cdecl* GameLog_t)(const char*, ...);

inline GameLog_t get_gamelog() {
    const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    return (GameLog_t)((0x7E3D50 - 0x400000u) + base);
}

// =============================================================================
// Resolved function pointers (set by loading_screen_install)
// =============================================================================

inline fn_pbl_ctor_t       g_pbl_ctor        = nullptr;
inline fn_pbl_copy_ctor_t  g_pbl_copy_ctor   = nullptr;
inline fn_pbl_read_data_t  g_pbl_read_data   = nullptr;
inline fn_pbl_read_scope_t g_pbl_read_scope  = nullptr;
inline fn_find_by_hash_t   g_find_by_hash    = nullptr;
inline fn_snd_play_t       g_snd_play        = nullptr;
inline fn_snd_play_ex_t    g_snd_play_ex     = nullptr;
inline fn_voice_to_handle_t g_voice_to_handle = nullptr;
inline fn_vvrelease_t      g_vvrelease       = nullptr;
inline fn_snd_eng_update_t g_snd_update      = nullptr;
inline DWORD               g_lastSndUpdateMs = 0;
inline fn_prt_t            g_prt             = nullptr;
inline void*               g_color_ptr       = nullptr;
inline fn_set_current_heap_t g_set_current_heap = nullptr;
inline int*                g_runtime_heap_idx = nullptr;
inline int*                g_s_load_heap_ptr  = nullptr;

inline fn_pbl_find_t       g_pbl_find        = nullptr;
inline void*               g_tex_table       = nullptr;
inline fn_hash_string_t    g_hash_string     = nullptr;

// Hook trampolines
inline fn_load_data_file_t g_orig_load_data_file = nullptr;
inline fn_load_config_t    g_orig_load_config    = nullptr;
inline fn_render_screen_t  g_orig_render_screen  = nullptr;
inline fn_load_end_t       g_orig_load_end       = nullptr;
inline fn_set_all_on_t     g_orig_set_all_on     = nullptr;
inline fn_load_update_t    g_orig_load_update    = nullptr;
inline fn_load_render_t    g_orig_load_render    = nullptr;
inline DWORD*              g_qpc_stamp           = nullptr;
inline DWORD               g_lastRenderMs        = 0;

// =============================================================================
// Sound helper types
// =============================================================================

struct GameSoundLocal {
    const char* mSoundDescription; // +0
    void*       mNodeNext;         // +4
    void*       mNodePrev;         // +8
    void*       mProps;            // +12  <- Properties* from FindByHashID
    uint8_t     mType;             // +16  <- 1 = GameSound, 2 = Parameterized, 3 = Stream
    uint8_t     _pad[3];
};

struct GameSoundControllable {
    uint16_t mVoiceVirtualHandle; // +0  <- filled by Play(); passed to VoiceVirtualRelease
    uint8_t  mFlags;              // +2
    uint8_t  _pad;                // +3
};

// =============================================================================
// Shared state across hooks
// =============================================================================

inline DWORD g_animStartMs    = 0;
inline bool  g_endProcessed   = false;
inline bool  s_sndLvlLoaded   = false;
inline int   s_lastAnimPhase  = -1;
inline int   s_lastAnimCycle  = -1;
inline DWORD s_nextBarSoundMs = 0;

// =============================================================================
// Animation constants and easing
// =============================================================================

inline constexpr DWORD kAnimCycleMs = 1200u + 400u + 1200u + 400u + 1500u; // 4700

inline float anim_smoothstep(float t)   { return t * t * (3.0f - 2.0f * t); }
inline float anim_ease_out(float t)     { float u = 1.0f - t; return 1.0f - u * u; }

// =============================================================================
// Config DATA chunk hashes
// =============================================================================

inline constexpr uint32_t kHash_LoadDisplay          = 0x8689C861;
inline constexpr uint32_t kHash_ScanLineTexture      = 0xe3dd2365;
inline constexpr uint32_t kHash_ZoomSelectorTextures = 0x6ae7b95f;
inline constexpr uint32_t kHash_AnimatedTextures     = 0xe83d35ac;
inline constexpr uint32_t kHash_XTrackingSound       = 0x853656d1;
inline constexpr uint32_t kHash_YTrackingSound       = 0x149267cc;
inline constexpr uint32_t kHash_ZoomSound            = 0x8b73a019;
inline constexpr uint32_t kHash_TransitionSound      = 0xd134f3a9;
inline constexpr uint32_t kHash_BarSound             = 0x27bac391;
inline constexpr uint32_t kHash_BarSoundInterval     = 0x18ed027c;
inline constexpr uint32_t kHash_PlanetLevel          = 0xd7b37b83;
inline constexpr uint32_t kHash_EnableBF1            = 0xd7436995;
inline constexpr uint32_t kHash_Map                  = 0xdfa2efb1;
inline constexpr uint32_t kHash_World                = 0x37a3e893;

// Known but unimplemented / BF2-native params
inline constexpr uint32_t kHash_TeamModel              = 0xd6c2b5f9;
inline constexpr uint32_t kHash_TeamModelRotationSpeed = 0x26455a06;
inline constexpr uint32_t kHash_ProgressBarTotalTime   = 0x31a6bc76;

// Sub-scope hashes
inline constexpr uint32_t kHash_LoadingTextColorPallete = 0xa6fb2870;

// Platform sub-scope hashes (computed at runtime in install())
inline uint32_t kHash_PC   = 0;
inline uint32_t kHash_PS2  = 0;
inline uint32_t kHash_XBOX = 0;

// Extension-only hashes (computed at runtime in install())
inline uint32_t kHash_ZoomSelectorTileSize = 0;
inline uint32_t kHash_LoadSoundLVL         = 0;
inline uint32_t kHash_RemoveToolTips       = 0;
inline uint32_t kHash_RemoveLoadingBar     = 0;

// =============================================================================
// PblConfig helpers
// =============================================================================

inline bool pbl_has_more(const void* pblcfg) {
    const uint32_t* lfh = *(const uint32_t* const*)((const uint8_t*)pblcfg + 0x14);
    const uint32_t  pos  = lfh[3];
    const uint32_t  size = lfh[2];
    const uint32_t  aligned = ((0u - pos) & 3u) + pos;
    return (int)aligned < (int)size;
}

inline const char* pbl_get_str(const uint32_t* data_buf, int i) {
    const int32_t off = (int32_t)data_buf[2 + i];
    return (const char*)&data_buf[2] + off;
}

inline float pbl_get_float(const uint32_t* data_buf, int i) {
    float f;
    memcpy(&f, &data_buf[2 + i], 4);
    return f;
}

inline int pbl_get_int(const uint32_t* data_buf, int i) {
    return (int)pbl_get_float(data_buf, i);
}

inline uint32_t hash_name(const char* name) {
    if (!name || !*name) return 0u;
    return g_hash_string ? g_hash_string(name) : 0u;
}

// =============================================================================
// Sound function declarations (defined in lifecycle.cpp)
// =============================================================================

void loading_screen_play_sound(uint32_t sound_hash);
void tracking_sound_start(uint32_t hash);
void tracking_sound_stop();

// =============================================================================
// Hook function forward declarations (for install/uninstall in lifecycle.cpp)
// =============================================================================

void __fastcall hooked_load_config(void* ecx, void* edx, uint32_t* fh);
void __fastcall hooked_render_screen(void* ecx, void* edx);
void __fastcall hooked_load_update(void* ecx, void* edx);
void __fastcall hooked_load_end(void* ecx, void* edx);
void __fastcall hooked_load_data_file(void* ecx, void* edx, const char* lvlPath);
