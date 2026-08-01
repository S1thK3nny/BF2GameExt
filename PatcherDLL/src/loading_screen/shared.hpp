#pragma once

// Internal shared header for loading_screen — not for external inclusion.

#include "loading_screen.hpp"
#include "load_display.hpp"
#include "core/resolve.hpp"

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
// GameSoundControllable::Stop(bool hardStop) — __thiscall, mirrored as __fastcall.
typedef void (__fastcall* fn_snd_ctrl_stop_t)(void* ecx, void* edx, int hardStop);
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
inline fn_snd_ctrl_stop_t  g_snd_ctrl_stop   = nullptr;
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
//
// `LoadDisplay::LoadDataFile` is `__thiscall(const char*)` — RET 4 — on all
// three builds, so one trampoline shape covers them.  The retail builds inlined
// the "Load\\load" literal into the body and never read the argument, which is
// what made it look like they had dropped it; they still declare and pop it.
//
// Do not "simplify" it to a no-argument hook.  A RET 0 detour here leaves four
// bytes on `LoadDisplay::LoadData`'s stack, because LoadData omits the cleanup
// for its preceding `__cdecl RedSetCurrentHeap` call and relies on LoadDataFile
// to pop it (steam 0x005773a5 PUSH / 0x005773c9 CALL / 0x005773d9 CALL).
// LoadData then pops its saved registers and its return address off by one slot
// and returns into the LoadDisplay object.  It reproduces on the first loading
// screen of the session — which is during startup, since GameState::PreStateInit
// calls LoadDisplay::Begin -> LoadData -> LoadDataFile.
inline fn_load_data_file_t g_orig_load_data_file = nullptr;
inline fn_load_config_t    g_orig_load_config    = nullptr;
inline fn_render_screen_t  g_orig_render_screen  = nullptr;
inline fn_load_end_t       g_orig_load_end       = nullptr;
inline fn_set_all_on_t     g_orig_set_all_on     = nullptr;
inline fn_load_update_t    g_orig_load_update    = nullptr;
inline fn_load_render_t    g_orig_load_render    = nullptr;
inline DWORD*              g_qpc_stamp           = nullptr;

// Red3DModelElementLite::SetModel(const char*) — __thiscall. Binds a team icon
// model by name, exactly as the engine's own TeamModel key would.
using fn_set_model_t = void(__fastcall*)(void* ecx, void* edx, const char* name);
inline fn_set_model_t      g_set_model           = nullptr;
inline DWORD               g_lastRenderMs        = 0;

// =============================================================================
// Snd::VoiceVirtual looping flag
// =============================================================================
// Bit 0x10 of the byte at VoiceVirtual+0x34 is the authoritative looping flag.
// Read straight out of modtools Snd::VoiceVirtual::Update (0x00894c50), which
// treats the virtual voice as byte** and does:
//
//     if (((uint)vv[0xd] & 0x10) == 0)   // vv[0xd] == byte offset 0x34
//         Stop(vv);                      // past the end and not looping -> stop
//     else
//         pos -= sampleEnd;              // looping -> wrap
//
// The same function calls VoiceBase::CopyParameters(vv->mVoice, vv, false) every
// tick, which pushes this flag down onto the real Voice. That is why clearing it
// on the Voice (Voice+0x34, which Snd::Voice::Update genuinely does read) had no
// effect: the VoiceVirtual copied it straight back on the next update.
//
// Clearing it here means the engine retires the voice by itself, at the end of
// the pass it is already playing, using its own Stop path.
// Verified identical on Steam: VoiceVirtual::Update 0x007380b0 makes the same
// `param_1[0xd] & 0x10` test, byte for byte.
inline constexpr uintptr_t kVoiceVirtual_LoopByte = 0x34;
inline constexpr uint8_t   kVoiceVirtual_LoopBit  = 0x10;

// =============================================================================
// Snd::Properties field offsets — build-dependent, filled in by install()
// =============================================================================
// Unlike VoiceVirtual, Properties is NOT laid out the same on debug and release:
// everything from +0x18 on sits 4 bytes lower on retail.  Values come from
// g_addr (see the snd_props_* entries in game_addrs.hpp).

inline uintptr_t g_propsLoopByte    = 0x1c;  // bit 0x10 = looping
inline uintptr_t g_propsNextAllowed = 0x68;  // float, replay cooldown

// GameSoundControllable::StolenCallback, resolved for the active build. Passed
// to Snd::Play so a stolen voice is retired through the engine's own path.
inline void* g_snd_stolen_callback = nullptr;

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

// Set when load_data_guard_install could not resolve one of the callees it
// reimplements. Reported from hooked_load_config, which runs long after the
// install window where calling into the game would fault.
inline bool  g_guardUnresolved = false;

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

// Extension-only hashes. Filled in by loading_screen_init_hashes() on the first
// LoadConfig, not at install time — see the note on that function.
inline uint32_t kHash_ZoomSelectorTileSize = 0;
inline uint32_t kHash_LoadSoundLVL         = 0;
inline uint32_t kHash_RemoveToolTips       = 0;
inline uint32_t kHash_RemoveLoadingBar     = 0;
inline uint32_t kHash_RemoveLoadingText    = 0;
inline uint32_t kHash_RemoveMissionName    = 0;
inline uint32_t kHash_RemoveModeName       = 0;
inline uint32_t kHash_TeamModelScale       = 0;
inline uint32_t kHash_TeamModelOffset      = 0;

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

// Size of the buffer handed to PblConfig::ReadNextData, in dwords.
//
// ReadNextData writes the entry's argument dwords AND its whole string blob
// into that buffer, taking the blob length straight from the file with no bound
// of its own. The engine's own LoadConfig gives it a 1896-byte frame buffer
// (auStack_768 on modtools). Ours used to be 512, which a stock retail
// load.cfg overruns on its longer entries — and since the PblConfig objects
// live next to it on our stack, the overrun lands on the parent-chunk pointer
// (+0x14) and file pointer (+0x28) that drive every subsequent read.
//
// 8 KB, generous rather than merely sufficient: the real bound lives in the
// data file, so there is nothing here to derive it from.
inline constexpr int kPblDataDwords = 0x800;

// Argument i as a string, or nullptr when it is not one.
//
// PblConfig stores a string argument as a byte offset from the start of the
// argument dwords to the entry's trailing string blob; a NUMBER argument
// occupies the same dword as a raw float. There is no type tag, so reading a
// numeric argument as a string reinterprets its bit pattern as an offset:
// `TeamModelOffset(120, -90)` gave 120.0f = 0x42F00000 and walked 1.1 GB off
// the end of the buffer. That is not a hypothetical - it is a config written
// against an older signature of a key, which is exactly what happens whenever
// one of these keys gains or loses an argument.
//
// Every real offset must clear the argument dwords it is measured from and stay
// inside the buffer we handed ReadNextData, and the string must terminate inside
// it too. Float bit patterns fail that by a mile in both directions: anything
// with a positive exponent is far past the end, and anything at or below 1.0f is
// still ~0x3F800000. Callers must treat nullptr as "argument absent".
inline const char* pbl_get_str(const uint32_t* data_buf, int i) {
    const uint32_t argc = data_buf[1];
    if (argc > (uint32_t)kPblDataDwords || i < 0 || (uint32_t)i >= argc) return nullptr;

    const int32_t off = (int32_t)data_buf[2 + i];
    // The blob starts after the argument dwords; the buffer ends kPblDataDwords
    // in, of which the first two are the hash and the count.
    const int32_t lo  = (int32_t)(argc * 4u);
    const int32_t hi  = (int32_t)((kPblDataDwords - 2) * 4);
    if (off < lo || off >= hi) return nullptr;

    const char* s   = (const char*)&data_buf[2] + off;
    const char* end = (const char*)&data_buf[2] + hi;
    for (const char* p = s; p < end; ++p)
        if (!*p) return s;
    return nullptr;  // runs off the end of the buffer without terminating
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
// Stops every sound this module started, tracking and one-shot alike. Safe to
// call repeatedly and when nothing is playing.
void loading_screen_stop_all_sounds();

// Hash the extension's own config keys with the engine's PblHash::calcHash.
// Idempotent. Must be called from a hook, never from install — see the
// definition in lifecycle.cpp.
void loading_screen_init_hashes();

// =============================================================================
// Hook function forward declarations (for install/uninstall in lifecycle.cpp)
// =============================================================================

// LoadDataChunk bounds guard (data_guard.cpp) — installed by loading_screen_install.
void load_data_guard_install(uintptr_t exe_base);
void load_data_guard_uninstall();

void __fastcall hooked_load_config(void* ecx, void* edx, uint32_t* fh);
void __fastcall hooked_render_screen(void* ecx, void* edx);
void __fastcall hooked_load_update(void* ecx, void* edx);
void __fastcall hooked_load_end(void* ecx, void* edx);
void __fastcall hooked_load_data_file(void* ecx, void* edx, const char* lvlPath);
