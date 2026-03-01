#include "pch.h"
#include "bf1_load_ext.hpp"
#include "lua_hooks.hpp"

#include <detours.h>

// =============================================================================
// Function pointer types
// =============================================================================

// PblConfig API — all __thiscall, mirrored as __fastcall
// (ECX = this, EDX = unused padding, stack args follow)

// PblConfig::PblConfig(fh) — ctor, copies 5-uint FileHandle, reads NAME+scope hash
typedef void  (__fastcall* fn_pbl_ctor_t)      (void* ecx, void* edx, uint32_t* fh);

// PblConfig::PblConfig(parent, share_fh) — copy ctor, RETN 8
typedef void  (__fastcall* fn_pbl_copy_ctor_t) (void* ecx, void* edx, void* parent, int share_fh);

// PblConfig::ReadNextData(data_buf) — writes hash/argc/args into buffer, RETN 4
typedef void  (__fastcall* fn_pbl_read_data_t) (void* ecx, void* edx, void* data_buf);

// PblConfig::ReadNextScope(temp_buf) — enters next scope, returns temp_buf, RETN 4
typedef void* (__fastcall* fn_pbl_read_scope_t)(void* ecx, void* edx, void* temp_buf);

// Snd::Sound::Properties::FindByHashID(hash) — static lookup by hash
typedef void* (__cdecl* fn_find_by_hash_t)(uint32_t hash);

// Snd::Sound::Play(entity, props, p3, p4, p5)
typedef void  (__cdecl* fn_snd_play_t)(int entity, void* props, int p3, int p4, int p5);

// PlatformRenderTexture — __stdcall, 15 args
typedef void (__stdcall* fn_prt_t)(
    uint32_t tex_hash,
    float x0, float y0, float x1, float y1,
    void* color_ptr, int alpha_blend,
    float u0, float v0, float u1, float v1,
    float r, float g, float b, float a
);

// _RedSetCurrentHeap(heap) — __cdecl; switches the active allocator heap,
// returns the previous heap index.  Used to redirect SortHeap array growth.
typedef int (__cdecl* fn_set_current_heap_t)(int heap);

// LoadDisplay::LoadDataFile(lvlPath) — MakeFullName → PblFile open → ChunkProcessor
// Hooked to log texture count before/after: confirms whether tex_ chunks are being read.
typedef void (__fastcall* fn_load_data_file_t)(void* ecx, void* edx, const char* lvlPath);

// PblHashTableCode::_Find(table_ptr, table_size, hash) — global texture table lookup.
// Confirmed from disasm of PlatformRenderTexture (call at 006d07ea).
// Returns a pointer to the found entry, or NULL if the hash is not registered.
typedef void* (__cdecl* fn_pbl_find_t)(void* table, uint32_t size, uint32_t hash);

// HashString(str) — the game's own FNV-1a case-insensitive hash (0x007e1bd0).
// Must be used instead of our own implementation to guarantee hashes match
// whatever the lvl loader stored when it processed tex_ chunks.
typedef uint32_t (__cdecl* fn_hash_string_t)(const char* str);

// LoadConfig / RenderScreen / End / Render — __thiscall mirrored as __fastcall
typedef void (__fastcall* fn_load_config_t)  (void* ecx, void* edx, uint32_t* fh);
typedef void (__fastcall* fn_render_screen_t)(void* ecx, void* edx);
// LoadDisplay::End() — called from Lua when loading finishes.  We hook it and
// spin-loop calling Update() until the BF1 animation finishes, then let the
// real End() run in the correct call context.
typedef void (__fastcall* fn_load_end_t)     (void* ecx, void* edx);
// LoadDisplay::ProgressIndicator::SetAllOn() — fills every progress bar segment
// to 100% immediately.  Called with ECX = LoadDisplay* + 0xd30.
typedef void (__fastcall* fn_set_all_on_t)   (void* ecx, void* edx);
// LoadDisplay::Update() — QPC-timed update: advances the "Loading..." blink timer,
// calls ProgressIndicator::Update(), then calls Render() to drive the screen.
typedef void (__fastcall* fn_load_update_t)  (void* ecx, void* edx);
// LoadDisplay::Render() — NOT throttled; renders at vsync rate.  Called directly
// from the hooked_load_end spin-loop to keep the end animation smooth.
typedef void (__fastcall* fn_load_render_t)  (void* ecx, void* edx);

// =============================================================================
// Gamelog — matches lua_hooks.cpp; writes to BFront2.log via the game's own logger
// =============================================================================

typedef void (__cdecl* GameLog_t)(const char*, ...);

static GameLog_t get_gamelog() {
    const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    return (GameLog_t)((0x7E3D50 - 0x400000u) + base);
}

// =============================================================================
// Resolved function pointers
// =============================================================================

static fn_pbl_ctor_t       g_pbl_ctor        = nullptr;
static fn_pbl_copy_ctor_t  g_pbl_copy_ctor   = nullptr;
static fn_pbl_read_data_t  g_pbl_read_data   = nullptr;
static fn_pbl_read_scope_t g_pbl_read_scope  = nullptr;
static fn_find_by_hash_t   g_find_by_hash    = nullptr;
static fn_snd_play_t       g_snd_play        = nullptr;
static fn_prt_t            g_prt             = nullptr;
static void*               g_color_ptr       = nullptr;
static fn_set_current_heap_t g_set_current_heap = nullptr;
static int*                  g_runtime_heap_idx  = nullptr; // &RunTimeHeap (game global, value=2)
static int*                  g_s_load_heap_ptr   = nullptr; // &s_loadHeap (game global at 0x00ba111c)

static fn_pbl_find_t       g_pbl_find            = nullptr;
static void*               g_tex_table           = nullptr;
static fn_hash_string_t    g_hash_string         = nullptr;

static fn_load_data_file_t g_orig_load_data_file = nullptr;
static fn_load_config_t    g_orig_load_config    = nullptr;
static fn_render_screen_t  g_orig_render_screen  = nullptr;
static fn_load_end_t       g_orig_load_end       = nullptr;
static fn_set_all_on_t     g_orig_set_all_on     = nullptr; // NOT Detour-hooked; called directly
static fn_load_update_t    g_orig_load_update    = nullptr; // Detour-hooked → hooked_load_update
static fn_load_render_t    g_orig_load_render    = nullptr; // NOT Detour-hooked; called directly
static DWORD*              g_qpc_stamp           = nullptr; // DAT_00ba2f60: QPC stamp written by Update() when it renders
static DWORD               g_lastRenderMs        = 0;       // last time any Render() was called (by us or Update())
static bool                g_inRealEnd           = false;   // set while g_orig_load_end() is running; prevents hooked_load_update injection
static bool                g_endProcessed        = false;   // set after our hooked_load_end has called g_orig_load_end once; prevents double-End() crash

// =============================================================================
// Global BF1 extension data
// =============================================================================

Bf1LoadExt g_bf1Ext = {};

// Timestamp set by hooked_load_config each time a new loading screen begins.
// The animation always starts from PlanetLevel 0 relative to this time, so
// every match starts fresh regardless of when GetTickCount() happens to be.
static DWORD g_animStartMs = 0;

// One-shot probe log: fires on the first BF1-enabled render frame per loading
// screen. Declared at file scope so hooked_load_config can re-arm it each load.
static bool s_probed = false;

// Per-transition cycle duration (ms): must exactly match the phase constants
// used in hooked_render_screen so hooked_load_end can share the same value.
//   Phase A (top/bottom converge): 1200 ms
//   Phase Ah (hold):                400 ms
//   Phase B (left/right converge):  1200 ms
//   Phase Bh (hold):                400 ms
//   Phase C (zoom-in):             1500 ms
//   Total:                         4700 ms per transition
static constexpr DWORD kAnimCycleMs = 1200u + 400u + 1200u + 400u + 1500u; // 4700

// Easing curves used for the zoom-sequence animation.
// Convergence phases (A, B): smoothstep — symmetric S-curve, slow start/end.
// Zoom-in phase (C):         ease-out-quad — fast initial punch, gentle fill.
static inline float anim_smoothstep(float t)   { return t * t * (3.0f - 2.0f * t); }
static inline float anim_ease_out(float t)     { float u = 1.0f - t; return 1.0f - u * u; }

// Config DATA chunk hashes — all BF1-only params we handle
static constexpr uint32_t kHash_LoadDisplay          = 0x8689C861;
static constexpr uint32_t kHash_ScanLineTexture      = 0xe3dd2365;
static constexpr uint32_t kHash_ZoomSelectorTextures = 0x6ae7b95f;
static constexpr uint32_t kHash_AnimatedTextures     = 0xe83d35ac;
static constexpr uint32_t kHash_PlanetBackdrops      = 0x3e13d8fe;
static constexpr uint32_t kHash_XTrackingSound       = 0x853656d1;
static constexpr uint32_t kHash_YTrackingSound       = 0x149267cc;
static constexpr uint32_t kHash_ZoomSound            = 0x8b73a019;
static constexpr uint32_t kHash_TransitionSound      = 0xd134f3a9;
static constexpr uint32_t kHash_BarSound             = 0x27bac391;
static constexpr uint32_t kHash_BarSoundInterval     = 0x18ed027c;
static constexpr uint32_t kHash_PlanetLevel          = 0xd7b37b83;
static constexpr uint32_t kHash_EnableBF1            = 0xd7436995;
static constexpr uint32_t kHash_Map                  = 0xdfa2efb1;
static constexpr uint32_t kHash_World                = 0x37a3e893;

// Known params that we recognise but do not act on — logged by name so they
// don't pollute the "unrecognised hash" fallback.
// TeamModel / TeamModelRotationSpeed: BF1-only 3D model during loading; not
//   reproducible in BF2's 2D loading screen pipeline.
// ProgressBarTotalTime: BF2 native param parsed by the *original* LoadConfig;
//   our second-pass re-parse sees it again but has no need to act on it.
static constexpr uint32_t kHash_TeamModel              = 0xd6c2b5f9;
static constexpr uint32_t kHash_TeamModelRotationSpeed = 0x26455a06;
static constexpr uint32_t kHash_ProgressBarTotalTime   = 0x31a6bc76;

// Extension-only parameters (not in vanilla BF1 load.cfg).
// Hash computed at runtime in bf1_load_ext_install() via hash_name().
static uint32_t kHash_ZoomSelectorTileSize = 0; // ZoomSelectorTileSize(half_w[, half_h])

// =============================================================================
// PblConfig helpers
// =============================================================================

// Check if the scope has more chunks to read.
// Reads via the lfh pointer at pblcfg+0x14 (pblcfg[5]).
//   share_fh=0 (root, constructed by ctor): lfh = &pblcfg[0], so lfh[2/3] == pblcfg[2/3] — correct.
//   share_fh=1 (scope child, constructed by copy_ctor): lfh = parent's lfh — bounds use parent scope.
static bool pbl_has_more(const void* pblcfg) {
    const uint32_t* lfh = *(const uint32_t* const*)((const uint8_t*)pblcfg + 0x14);
    const uint32_t  pos  = lfh[3];
    const uint32_t  size = lfh[2];
    const uint32_t  aligned = ((0u - pos) & 3u) + pos;
    return (int)aligned < (int)size;
}

// Sub-scope hashes — entries whose DATA header is immediately followed by a SCOPE body.
// After ReadNextData returns one of these hashes, ReadNextScope must be called to consume
// the scope body (otherwise the next ReadNextData call will hit a SCOPE chunk and crash).
// HASH_LoadingTextColorPallete: confirmed from BF2 binary (CMP ESI,0xa6fb2870 → ReadNextScope)
static constexpr uint32_t kHash_LoadingTextColorPallete = 0xa6fb2870;

// ReadNextData output layout: data_buf[0]=hash, data_buf[1]=argcount, data_buf[2+i]=arg[i]
// String args: self-relative int32 offset stored at data_buf[2+i], but the base for ALL
// string offsets is &data_buf[2] (the first arg slot), not &data_buf[2+i].
// Numeric args: PBL stores all numbers as IEEE 754 floats, even integer-typed params.
static const char* pbl_get_str(const uint32_t* data_buf, int i) {
    const int32_t off = (int32_t)data_buf[2 + i];
    return (const char*)&data_buf[2] + off;  // base is always first arg slot
}

static float pbl_get_float(const uint32_t* data_buf, int i) {
    float f;
    memcpy(&f, &data_buf[2 + i], 4);
    return f;
}

static int pbl_get_int(const uint32_t* data_buf, int i) {
    return (int)pbl_get_float(data_buf, i);  // PBL stores all numerics as float
}

// Hash a name using the game's own HashString function.
// This guarantees the hash matches whatever the lvl loader stored for tex_ chunks.
static uint32_t hash_name(const char* name) {
    if (!name || !*name) return 0u;
    return g_hash_string ? g_hash_string(name) : 0u;
}

// Forward declaration — defined after the hooks
void bf1_play_sound(uint32_t sound_hash);

// Parse one DATA entry as a known BF1 param and update g_bf1Ext.
// Called from both the LoadDisplay and Map loops so BF1 params work in either scope.
// PlanetLevel is NOT handled here — it has position args and is Map-only; the Map loop
// handles it directly.
static void parse_bf1_entry(const uint32_t* data_buf, GameLog_t fn_log)
{
    const uint32_t hash = data_buf[0];
    const uint32_t argc = data_buf[1];

    if (hash == kHash_EnableBF1 && argc >= 1) {
        g_bf1Ext.bf1Enabled = (pbl_get_int(data_buf, 0) != 0);
        fn_log("[BF1Ext]        EnableBF1 = %d\n", (int)g_bf1Ext.bf1Enabled);
    }
    else if (hash == kHash_ScanLineTexture && argc >= 1) {
        const char* s = pbl_get_str(data_buf, 0);
        fn_log("[BF1Ext]        ScanLineTexture = '%s'\n", s ? s : "(null)");
        g_bf1Ext.scanLineTexHash     = hash_name(s);
        if (argc >= 2) g_bf1Ext.scanLineParams[0] = pbl_get_float(data_buf, 1);
        if (argc >= 3) g_bf1Ext.scanLineParams[1] = pbl_get_float(data_buf, 2);
        if (argc >= 4) g_bf1Ext.scanLineParams[2] = pbl_get_float(data_buf, 3);
    }
    else if (hash == kHash_ZoomSelectorTextures && argc >= 1) {
        fn_log("[BF1Ext]        ZoomSelectorTextures argc=%u\n", argc);
        const int n = (int)argc < Bf1LoadExt::kMaxZoomSel
                    ? (int)argc : Bf1LoadExt::kMaxZoomSel;
        for (int i = 0; i < n; ++i) {
            const char* s = pbl_get_str(data_buf, i);
            fn_log("[BF1Ext]          [%d] = '%s'\n", i, s ? s : "(null)");
            g_bf1Ext.zoomSelHashes[i] = hash_name(s);
        }
        g_bf1Ext.zoomSelCount = n;
    }
    else if (hash == kHash_AnimatedTextures && argc >= 2) {
        const char* base = pbl_get_str(data_buf, 0);
        int         cnt  = pbl_get_int(data_buf, 1);
        const float fps  = (argc >= 3) ? pbl_get_float(data_buf, 2) : 10.0f;
        // Optional rect args (3-6): x, y, w, h in normalized 0-1 screen space.
        // w==0 (absent) → falls back to full screen in the renderer.
        const float ax = (argc >= 4) ? pbl_get_float(data_buf, 3) : 0.0f;
        const float ay = (argc >= 5) ? pbl_get_float(data_buf, 4) : 0.0f;
        const float aw = (argc >= 6) ? pbl_get_float(data_buf, 5) : 0.0f;
        const float ah = (argc >= 7) ? pbl_get_float(data_buf, 6) : 0.0f;
        fn_log("[BF1Ext]        AnimatedTextures base='%s' cnt=%d fps=%.2f rect=(%.3f,%.3f,%.3f,%.3f)\n",
               base ? base : "(null)", cnt, fps, ax, ay, aw, ah);
        if (base && cnt > 0) {
            if (cnt > Bf1LoadExt::kMaxAnimFrames) cnt = Bf1LoadExt::kMaxAnimFrames;
            for (int i = 0; i < cnt; ++i) {
                char nm[256];
                snprintf(nm, sizeof(nm), "%s%d", base, i);
                g_bf1Ext.animHashes[i] = hash_name(nm);
            }
            g_bf1Ext.animCount = cnt;
            g_bf1Ext.animFPS   = fps;
            g_bf1Ext.animX = ax; g_bf1Ext.animY = ay;
            g_bf1Ext.animW = aw; g_bf1Ext.animH = ah;
        }
    }
    else if (hash == kHash_PlanetBackdrops && argc >= 1) {
        fn_log("[BF1Ext]        PlanetBackdrops argc=%u\n", argc);
        const int n = (int)argc < Bf1LoadExt::kMaxBackdrops
                    ? (int)argc : Bf1LoadExt::kMaxBackdrops;
        for (int i = 0; i < n; ++i) {
            const char* s = pbl_get_str(data_buf, i);
            fn_log("[BF1Ext]          [%d] = '%s'\n", i, s ? s : "(null)");
            g_bf1Ext.backdropHashes[i] = hash_name(s);
        }
        g_bf1Ext.backdropCount = n;
    }
    else if (hash == kHash_XTrackingSound && argc >= 1) {
        const char* s = pbl_get_str(data_buf, 0);
        fn_log("[BF1Ext]        XTrackingSound = '%s'\n", s ? s : "(null)");
        g_bf1Ext.xTrackSoundHash = hash_name(s);
    }
    else if (hash == kHash_YTrackingSound && argc >= 1) {
        const char* s = pbl_get_str(data_buf, 0);
        fn_log("[BF1Ext]        YTrackingSound = '%s'\n", s ? s : "(null)");
        g_bf1Ext.yTrackSoundHash = hash_name(s);
    }
    else if (hash == kHash_ZoomSound && argc >= 1) {
        const char* s = pbl_get_str(data_buf, 0);
        fn_log("[BF1Ext]        ZoomSound = '%s'\n", s ? s : "(null)");
        g_bf1Ext.zoomSoundHash = hash_name(s);
    }
    else if (hash == kHash_TransitionSound && argc >= 1) {
        const char* s = pbl_get_str(data_buf, 0);
        fn_log("[BF1Ext]        TransitionSound = '%s'\n", s ? s : "(null)");
        g_bf1Ext.transitionSoundHash = hash_name(s);
    }
    else if (hash == kHash_BarSound && argc >= 1) {
        const char* s = pbl_get_str(data_buf, 0);
        fn_log("[BF1Ext]        BarSound = '%s'\n", s ? s : "(null)");
        g_bf1Ext.barSoundHash = hash_name(s);
    }
    else if (hash == kHash_BarSoundInterval && argc >= 1) {
        fn_log("[BF1Ext]        BarSoundInterval = %d\n", pbl_get_int(data_buf, 0));
        g_bf1Ext.barSoundInterval = pbl_get_int(data_buf, 0);
    }
    // ── BF1Ext-only extension params ──────────────────────────────────────────
    // ZoomSelectorTileSize(half_w[, half_h]):
    // Half-dimensions of each tiling strip tile in normalized 0-1 screen space.
    // Drives the BF1-accurate 16-quad crosshair frame around the planet rect.
    // If omitted, defaults to 0.02 (≈2% of screen width per strip).
    else if (kHash_ZoomSelectorTileSize && hash == kHash_ZoomSelectorTileSize && argc >= 1) {
        g_bf1Ext.zoomTileHalfW = pbl_get_float(data_buf, 0);
        g_bf1Ext.zoomTileHalfH = (argc >= 2) ? pbl_get_float(data_buf, 1)
                                              : g_bf1Ext.zoomTileHalfW;
        fn_log("[BF1Ext]        ZoomSelectorTileSize hw=%.4f hh=%.4f\n",
               g_bf1Ext.zoomTileHalfW, g_bf1Ext.zoomTileHalfH);
    }
    // ── Known-but-unimplemented / BF2-native params ───────────────────────────
    // Recognised by hash so they don't fall through to the noisy "unrecognised"
    // log.  No runtime action is taken for any of these.
    else if (hash == kHash_TeamModel) {
        // BF1-only: a 3D team model rotated during the loading screen.
        // BF2's loading screen has no 3D model pipeline, so we skip this.
        const char* s = (argc >= 1) ? pbl_get_str(data_buf, 0) : nullptr;
        fn_log("[BF1Ext]        TeamModel = '%s' (BF1-only, not implemented)\n",
               s ? s : "(null)");
    }
    else if (hash == kHash_TeamModelRotationSpeed) {
        // BF1-only: rotation speed (deg/s) for the TeamModel above.
        fn_log("[BF1Ext]        TeamModelRotationSpeed = %.3f (BF1-only, not implemented)\n",
               (argc >= 1) ? pbl_get_float(data_buf, 0) : 0.0f);
    }
    else if (hash == kHash_ProgressBarTotalTime) {
        // BF2 native: total animation time for the loading progress bar.
        // Already consumed by BF2's own LoadConfig; we see it on our second
        // parse pass but have no need to act on it.
        fn_log("[BF1Ext]        ProgressBarTotalTime = %.3f (BF2 native, no action)\n",
               (argc >= 1) ? pbl_get_float(data_buf, 0) : 0.0f);
    }
    else {
        if (argc == 0 || argc > 8) {
            fn_log("[BF1Ext]        (unrecognised hash=%08x argc=%u)\n", hash, argc);
        } else {
            // Log args as floats for diagnostic purposes (texture-name args will show
            // garbage float values, but numeric params like GalaxyMapRect are readable).
            char ab[192]; int pos = 0;
            for (uint32_t i = 0; i < argc; ++i)
                pos += snprintf(ab + pos, (int)sizeof(ab) - pos,
                                " %.3f", pbl_get_float(data_buf, i));
            fn_log("[BF1Ext]        (unrecognised hash=%08x argc=%u args:%s)\n",
                   hash, argc, ab);
        }
    }
}

// Enter the sub-scope whose DATA header we just read, and drain all its DATA entries.
// Used for LoadingTextColorPallete, which contains only simple DATA entries (no nested scopes).
// tmp    : 0x300-byte scratch buffer (reused from the caller's temp_buf)
// scratch: 0x80-uint scratch buffer  (reused from the caller's data_buf)
static void pbl_skip_next_scope(void* parent, uint8_t* tmp, uint32_t* scratch)
{
    memset(tmp, 0, 0x300);
    void* sr = g_pbl_read_scope(parent, nullptr, tmp);
    if (!sr) return;
    uint8_t child[0x300];
    memset(child, 0, sizeof(child));
    g_pbl_copy_ctor(child, nullptr, sr, 1);
    while (pbl_has_more(child))
        g_pbl_read_data(child, nullptr, scratch);
}

// =============================================================================
// LoadDataFile hook — diagnostic: log tex count before/after to confirm
//                     whether tex_ chunks in the lvl are actually being read.
// LoadDisplay + 0x15f8 = running texture count incremented by ChunkProcessor
//                        each time RedTexture::Read succeeds.
// =============================================================================

static void __fastcall hooked_load_data_file(void* ecx, void* edx, const char* lvlPath)
{
    auto fn_log = get_gamelog();
    const int texBefore = ecx ? *(const int*)((const uint8_t*)ecx + 0x15f8) : -1;
    fn_log("[BF1Ext] LoadDataFile('%s') texCount before=%d\n",
           lvlPath ? lvlPath : "(null)", texBefore);

    g_orig_load_data_file(ecx, edx, lvlPath);

    const int texAfter = ecx ? *(const int*)((const uint8_t*)ecx + 0x15f8) : -1;
    fn_log("[BF1Ext] LoadDataFile('%s') texCount after=%d (+%d loaded)\n",
           lvlPath ? lvlPath : "(null)", texAfter, texAfter - texBefore);
}

// =============================================================================
// LoadDisplay::Update hook — smooth asset-loading phase to ~30 fps
// =============================================================================
// Update() has a 50 ms QPC throttle → at most 20 fps during asset loading.
// We inject a Render() call only when two conditions are both true:
//   1. Update() returned without rendering (QPC stamp unchanged).
//   2. ≥33 ms have elapsed since the last render from any source.
// This adds at most ~10 extra renders/second (30-20=10) → modest overhead.
// The g_inRealEnd guard prevents injection while End() is tearing down state.

static void __fastcall hooked_load_update(void* ecx, void* edx)
{
    if (g_inRealEnd) {
        // End() is tearing down LoadDisplay state — do NOT call Update() at all.
        // The original Update() may render internally (if its 50 ms QPC throttle
        // has elapsed), which would crash on freed textures during teardown.
        return;
    }

    // BF1 mode: redirect s_loadHeap → RunTimeHeap for the entire Update call.
    //
    // Root cause of 007E2D77 / 008024A6 crashes:
    //   LoadDisplay::Update calls _RedSetCurrentHeap(s_loadHeap = TempLoadHeap) BEFORE
    //   any rendering.  The loading-text vtable draw and ProgressIndicator::Update then
    //   push items to the SortHeap with __RedCurrHeap = TempLoadHeap.  If the SortHeap
    //   backing array is at capacity, FUN_00806920 reallocates it from TempLoadHeap.
    //   After ReleaseTempHeap the array pointer is dangling → next SortHeap write
    //   corrupts RunTimeHeap's free list → crash at 007E2D77 in _RedGetHeapFree.
    //   Similarly, any MemoryPool created during loading-screen rendering with
    //   __RedCurrHeap = TempLoadHeap has its slab in TempLoadHeap → crash at 008024A6.
    //
    // Fix: substitute RunTimeHeap (persistent) for s_loadHeap so that
    //   _RedSetCurrentHeap(s_loadHeap) inside Update switches to RunTimeHeap instead.
    //   ALL SortHeap array growth (loading text, progress bar, BF1 overlay) then
    //   allocates from RunTimeHeap.  s_loadHeap is restored after Update returns.
    //   The existing per-g_prt heap switch in hooked_render_screen is kept as
    //   defence-in-depth for code paths that bypass hooked_load_update.
    int saved_load_heap = -1;
    if (g_bf1Ext.bf1Enabled && g_s_load_heap_ptr && g_runtime_heap_idx) {
        saved_load_heap      = *g_s_load_heap_ptr;
        *g_s_load_heap_ptr   = *g_runtime_heap_idx;
    }

    const DWORD qpc_before = g_qpc_stamp ? *g_qpc_stamp : 0;
    g_orig_load_update(ecx, edx);

    if (saved_load_heap >= 0 && g_s_load_heap_ptr)
        *g_s_load_heap_ptr = saved_load_heap;

    if (!g_bf1Ext.bf1Enabled || !g_orig_load_render) return;

    if (g_qpc_stamp && *g_qpc_stamp != qpc_before) {
        // Update() rendered naturally — just record the timestamp.
        g_lastRenderMs = GetTickCount();
    } else if (ecx && *(const uint8_t*)ecx != 0
               && GetTickCount() - g_lastRenderMs >= 33u) {
        // Inject render only if LoadDisplay is still active (*ecx != 0).
        // End() zeroes the active byte (offset 0) after freeing textures; calling
        // Render() on a torn-down LoadDisplay accesses freed D3D resources → crash.
        g_lastRenderMs = GetTickCount();
        g_orig_load_render(ecx, nullptr);
    }
}

// =============================================================================
// LoadDisplay::End hook — delays teardown until animation completes
// =============================================================================
// LoadDisplay::End() is called from Lua (ScriptCB_ShowLoadDisplay(false)) when
// all assets have loaded.  Without this hook the loading screen can vanish
// before the BF1 zoom sequence has finished.
//
// When BF1 mode is active we: fill the progress bar to 100% via SetAllOn(),
// then spin-loop driving the screen until the animation completes.
// Render() is called every frame (vsync rate) for smoothness; Update() is called
// every ~200 ms to advance the "Loading..." blink timer without the 50 ms QPC cap.
//
// A 30-second hard timeout prevents locking up if something goes wrong.

static void __fastcall hooked_load_end(void* ecx, void* edx)
{
    // Guard: LoadDisplay::End() can be called more than once per level load.
    // FUN_00734040 (the main level-setup function) calls End() explicitly AFTER
    // LuaHelper::CallGlobalProc("ScriptInit"), which itself may call End() via
    // ScriptCB_ShowLoadDisplay(false).  If our hook already ran the spin-loop
    // and called g_orig_load_end() (which zeroes *ecx and frees textures), the
    // second call through our hook would call g_orig_load_end again →
    // End() → Render() → PlatformRenderTexture with a hash that was already
    // removed from the global table by RedTexture::~RedTexture → crash.
    //
    // We track this ourselves because End()'s *ecx-zeroing is conditional on
    // journal-mode checks and _g_bNoRender; if those conditions fail, *ecx stays
    // non-zero and the ecx-based guard would not fire on the second call.
    if (g_endProcessed) return;

    // Only spin when BF1 mode is active, Update() is available, and there
    // are configured planet transitions to animate.
    if (g_bf1Ext.bf1Enabled && g_animStartMs != 0 && g_orig_load_update) {
        int nTrans = 0;
        for (int i = 0; i < g_bf1Ext.planetCount; ++i) {
            const auto& e = g_bf1Ext.planets[i];
            if (e.w > 0.0f && e.h > 0.0f) nTrans++;
            else break;
        }
        if (nTrans > 0) {
            const DWORD required = (DWORD)nTrans * kAnimCycleMs;
            const DWORD deadline = GetTickCount() + 30000u; // 30 s hard timeout

            // Fill the progress bar to 100% immediately — we're taking over the render
            // loop so End()'s own SetAllOn() won't run until after our animation ends.
            if (g_orig_set_all_on && ecx)
                g_orig_set_all_on((uint8_t*)ecx + 0xd30, nullptr);

            // Spin-loop: render at ~30 fps (every 33 ms) for smoothness, and call
            // Update() every ~200 ms to advance the "Loading..." blink timer.
            // We pump Windows messages each iteration so the window stays responsive
            // during the animation (the game's main loop is blocked while we're here).
            // QPC stamp is checked around each Update() call so we don't double-render
            // when Update() renders internally on top of our 33 ms gate.
            DWORD lastUpdateMs = 0;
            while (GetTickCount() - g_animStartMs < required) {
                if (GetTickCount() >= deadline) break;
                const DWORD now = GetTickCount();

                // Drain the Windows message queue so the window doesn't appear frozen.
                MSG msg;
                while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }

                if (now - lastUpdateMs >= 200u) {
                    lastUpdateMs = now;
                    // Track whether Update() renders internally (QPC stamp changes)
                    // so the 33 ms render gate doesn't fire again in the same slot.
                    const DWORD qpc_before = g_qpc_stamp ? *g_qpc_stamp : 0;
                    // Same s_loadHeap redirect as in hooked_load_update: ensure any
                    // SortHeap growth triggered by Update's loading-text draws goes to
                    // RunTimeHeap.  We call g_orig_load_update (the trampoline to the
                    // original) directly here, bypassing hooked_load_update, so the
                    // redirect must be applied manually.
                    int saved_load_heap_end = -1;
                    if (g_s_load_heap_ptr && g_runtime_heap_idx) {
                        saved_load_heap_end    = *g_s_load_heap_ptr;
                        *g_s_load_heap_ptr     = *g_runtime_heap_idx;
                    }
                    g_orig_load_update(ecx, nullptr); // advance blink timer + bar state
                    if (saved_load_heap_end >= 0 && g_s_load_heap_ptr)
                        *g_s_load_heap_ptr = saved_load_heap_end;
                    if (g_qpc_stamp && *g_qpc_stamp != qpc_before)
                        g_lastRenderMs = GetTickCount(); // Update() rendered — reset gate
                }
                if (g_orig_load_render && GetTickCount() - g_lastRenderMs >= 33u) {
                    g_lastRenderMs = GetTickCount();
                    g_orig_load_render(ecx, nullptr); // ~30 fps rendering
                }
                Sleep(1);                             // yield CPU between frames
            }
        }
    }
    // Call the real End() from the correct context: after the loop we are still
    // inside the ScriptCB_ShowLoadDisplay → thunk → hooked_load_end call chain,
    // NOT inside a RenderScreen call.  End() calling Render() / freeing textures
    // is therefore safe.
    // g_inRealEnd causes hooked_load_update to return early (no Update(), no Render())
    // while End() is tearing down state.  Calling the original Update() here would
    // be unsafe: its internal 50 ms QPC render path fires against freed textures.
    g_endProcessed = true;  // block any subsequent End() call from re-entering
    g_inRealEnd = true;
    g_orig_load_end(ecx, edx);
    g_inRealEnd = false;
}

// =============================================================================
// LoadConfig hook
// =============================================================================
// Strategy: call original first (BF2 parses its own params into a private copy
// of the FileHandle). Because PblConfig copies fh internally, the fh pointer
// passed to us is NOT modified by the original. We can re-construct a fresh
// PblConfig from the same fh to parse the BF1-only DATA chunks.

static void __fastcall hooked_load_config(void* ecx, void* edx, uint32_t* fh)
{
    // Snapshot the 5-uint FileHandle before the original call advances fh[3] (position).
    uint32_t fh_saved[5];
    if (fh) memcpy(fh_saved, fh, sizeof(fh_saved));

    g_orig_load_config(ecx, edx, fh);

    auto fn_log = get_gamelog();

    if (!g_pbl_ctor || !g_pbl_read_data || !g_pbl_read_scope || !g_pbl_copy_ctor || !fh) {
        fn_log("[BF1Ext] LoadConfig: skipping — missing fn ptrs (ctor=%p rd=%p rs=%p cc=%p fh=%p)\n",
               (void*)g_pbl_ctor, (void*)g_pbl_read_data,
               (void*)g_pbl_read_scope, (void*)g_pbl_copy_ctor, (void*)fh);
        return;
    }

    // Restore so our second PblConfig ctor starts reading from the beginning.
    memcpy(fh, fh_saved, sizeof(fh_saved));

    g_bf1Ext.reset();
    g_animStartMs   = GetTickCount(); // restart animation from PlanetLevel 0 on each new match
    g_lastRenderMs  = 0;              // reset so first injected render fires immediately
    g_endProcessed  = false;          // re-arm End() hook for new loading screen
    s_probed        = false;          // re-arm per-loading-screen probe log

    // Current level hashes stored in the LoadDisplay object (ecx+4 = world hash, ecx+8 = map hash).
    // These are matched against Map() scope level name hashes to locate EnableBF1.
    const uint32_t lvlHash1 = *(const uint32_t*)((const uint8_t*)ecx + 4);
    const uint32_t lvlHash2 = *(const uint32_t*)((const uint8_t*)ecx + 8);

    fn_log("[BF1Ext] LoadConfig: ecx=%p lvlHash1=%08x lvlHash2=%08x fh[0..3]=%08x %08x %08x %08x\n",
           ecx, lvlHash1, lvlHash2, fh_saved[0], fh_saved[1], fh_saved[2], fh_saved[3]);

    // PblConfig stack buffers — 0x300 bytes each, matches the game's auStack_768 allocation.
    uint8_t  outer_buf[0x300]; // root-level PblConfig
    uint8_t  scope_buf[0x300]; // LoadDisplay scope
    uint8_t  map_buf  [0x300]; // Map/World scope
    uint8_t  temp_buf [0x300]; // scratch for ReadNextScope return value
    uint32_t root_data[0x80];  // root-level scope-header DATA chunks
    uint32_t data_buf [0x80];  // inner DATA chunks

    memset(outer_buf, 0, sizeof(outer_buf));
    memset(scope_buf, 0, sizeof(scope_buf));
    memset(map_buf,   0, sizeof(map_buf));

    // Construct root PblConfig; ctor reads the file NAME chunk and positions the
    // reader at the first root-level scope header.
    g_pbl_ctor(outer_buf, nullptr, fh);

    fn_log("[BF1Ext]   outer_buf[2]=%08x [3]=%08x [5]=%p\n",
           ((uint32_t*)outer_buf)[2], ((uint32_t*)outer_buf)[3], (void*)((uint32_t*)outer_buf)[5]);

    // ── Outer loop ───────────────────────────────────────────────────────────
    // The config file has multiple sibling scopes at the root level:
    //   LoadDisplay { … }
    //   Map("levelName") { … }
    //   World("levelName") { … }
    // ReadNextData reads the scope-header DATA chunk (hash = scope type, args = scope args).
    // ReadNextScope then enters the scope body.
    int outerCount = 0;
    while (pbl_has_more(outer_buf)) {
        memset(root_data, 0, sizeof(root_data));
        g_pbl_read_data(outer_buf, nullptr, root_data);

        const uint32_t root_hash = root_data[0];
        const uint32_t root_argc = root_data[1];

        fn_log("[BF1Ext]   outer[%d] hash=%08x argc=%u\n", outerCount++, root_hash, root_argc);

        // ── LoadDisplay scope — BF1 texture / sound params ───────────────────
        if (root_hash == kHash_LoadDisplay) {
            fn_log("[BF1Ext]   -> LoadDisplay: entering scope\n");
            memset(temp_buf, 0, sizeof(temp_buf));
            void* sr = g_pbl_read_scope(outer_buf, nullptr, temp_buf);
            fn_log("[BF1Ext]      ReadNextScope returned %p\n", sr);
            g_pbl_copy_ctor(scope_buf, nullptr, sr, 1);
            fn_log("[BF1Ext]      scope_buf[2]=%08x [3]=%08x [5]=%p\n",
                   ((uint32_t*)scope_buf)[2], ((uint32_t*)scope_buf)[3], (void*)((uint32_t*)scope_buf)[5]);

            int dispCount = 0;
            while (pbl_has_more(scope_buf)) {
                memset(data_buf, 0, sizeof(data_buf));
                g_pbl_read_data(scope_buf, nullptr, data_buf);

                const uint32_t hash = data_buf[0];

                fn_log("[BF1Ext]      disp[%d] hash=%08x argc=%u\n", dispCount++, hash, data_buf[1]);

                // LoadingTextColorPallete is a sub-scope: skip its body so the reader
                // stays in sync. All other entries go through parse_bf1_entry.
                if (hash == kHash_LoadingTextColorPallete) {
                    fn_log("[BF1Ext]        -> LoadingTextColorPallete: skipping scope\n");
                    pbl_skip_next_scope(scope_buf, temp_buf, data_buf);
                    continue;
                }

                parse_bf1_entry(data_buf, fn_log);
            }
            fn_log("[BF1Ext]   <- LoadDisplay done (%d entries)\n", dispCount);
        }

        // ── Map / World scope — per-map master switch ─────────────────────────
        // EnableBF1(1/0) lives here (as in the user's load.cfg).
        // arg0 of the scope header is the level name.
        else if (root_hash == kHash_Map || root_hash == kHash_World) {
            const char*    lvlName = (root_argc >= 1) ? pbl_get_str(root_data, 0) : nullptr;
            const uint32_t mHash   = lvlName ? hash_name(lvlName) : 0;
            const bool     match   = mHash && (mHash == lvlHash1 || mHash == lvlHash2);

            fn_log("[BF1Ext]   -> %s scope: lvlName='%s' lvlHash=%08x match=%d\n",
                   (root_hash == kHash_Map) ? "Map" : "World",
                   lvlName ? lvlName : "(null)", mHash, (int)match);

            memset(temp_buf, 0, sizeof(temp_buf));
            void* mr = g_pbl_read_scope(outer_buf, nullptr, temp_buf);
            fn_log("[BF1Ext]      ReadNextScope returned %p\n", mr);
            g_pbl_copy_ctor(map_buf, nullptr, mr, 1);
            fn_log("[BF1Ext]      map_buf[2]=%08x [3]=%08x [5]=%p\n",
                   ((uint32_t*)map_buf)[2], ((uint32_t*)map_buf)[3], (void*)((uint32_t*)map_buf)[5]);

            // Always drain the scope to advance the file position correctly.
            // Only act on the data when the level name matches the current map.
            int mapCount = 0;
            while (pbl_has_more(map_buf)) {
                memset(data_buf, 0, sizeof(data_buf));
                g_pbl_read_data(map_buf, nullptr, data_buf);

                const uint32_t mhash = data_buf[0];
                const uint32_t mArgc = data_buf[1];

                fn_log("[BF1Ext]      map[%d] hash=%08x argc=%u\n", mapCount++, mhash, mArgc);

                if (!match) continue; // drain but don't act on non-matching maps

                // PlanetLevel: flat entry directly in Map (no sub-scope).
                // args: levelIndex(int), texName(str), x(float), y(float), w(float), h(float)
                if (mhash == kHash_PlanetLevel && mArgc >= 2
                    && g_bf1Ext.planetCount < Bf1LoadExt::kMaxPlanets) {
                    Bf1LoadExt::PlanetEntry& e = g_bf1Ext.planets[g_bf1Ext.planetCount++];
                    e.levelIndex = pbl_get_int(data_buf, 0);
                    const char* s = pbl_get_str(data_buf, 1);
                    e.texHash = hash_name(s);
                    e.x = (mArgc >= 3) ? pbl_get_float(data_buf, 2) : 0.0f;
                    e.y = (mArgc >= 4) ? pbl_get_float(data_buf, 3) : 0.0f;
                    e.w = (mArgc >= 5) ? pbl_get_float(data_buf, 4) : 0.0f;
                    e.h = (mArgc >= 6) ? pbl_get_float(data_buf, 5) : 0.0f;
                    fn_log("[BF1Ext]        PlanetLevel idx=%d tex='%s' x=%.1f y=%.1f w=%.1f h=%.1f\n",
                           e.levelIndex, s ? s : "(null)", e.x, e.y, e.w, e.h);
                }
                else {
                    // All other BF1 params (EnableBF1, ScanLineTexture, ZoomSelectorTextures,
                    // AnimatedTextures, PlanetBackdrops, sounds, etc.)
                    parse_bf1_entry(data_buf, fn_log);
                }
            }
            fn_log("[BF1Ext]   <- %s done (%d entries)\n",
                   (root_hash == kHash_Map) ? "Map" : "World", mapCount);
        }
        else {
            fn_log("[BF1Ext]   (unrecognised root hash=%08x — no scope consumed)\n", root_hash);
        }
    }

    fn_log("[BF1Ext] LoadConfig result: bf1Enabled=%d scanLine=%08x "
           "zoomSel=%d backdrop=%d anim=%d planet=%d\n",
           (int)g_bf1Ext.bf1Enabled,
           g_bf1Ext.scanLineTexHash,
           g_bf1Ext.zoomSelCount, g_bf1Ext.backdropCount,
           g_bf1Ext.animCount, g_bf1Ext.planetCount);
}

// =============================================================================
// RenderScreen hook
// =============================================================================
// Coordinates are normalized 0-1 screen space, confirmed from BF1 Ghidra analysis.

static void __fastcall hooked_render_screen(void* ecx, void* edx)
{
    // Suppress BF2's RandomBackdrop draw when BF1 mode is active.
    // LoadDisplay::RenderScreen (0x0067a1b0) ONLY draws the RandomBackdrop hash
    // stored at ecx+0x14c0.  PlatformRenderTexture skips when hash==0, so we
    // temporarily zero it and restore it after the call.
    uint32_t savedBackdropHash = 0;
    if (g_bf1Ext.bf1Enabled && ecx) {
        uint32_t* pHash = (uint32_t*)((uint8_t*)ecx + 0x14c0);
        savedBackdropHash = *pHash;
        *pHash = 0;
    }

    g_orig_render_screen(ecx, edx);

    if (g_bf1Ext.bf1Enabled && ecx)
        *(uint32_t*)((uint8_t*)ecx + 0x14c0) = savedBackdropHash;

    // One-shot general diagnostic (first RenderScreen call ever, BF1 or not).
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        auto fn_log = get_gamelog();
        fn_log("[BF1Ext] RenderScreen first call: bf1Enabled=%d g_prt=%p g_color_ptr=%p\n",
               (int)g_bf1Ext.bf1Enabled, (void*)g_prt, g_color_ptr);
    }

    if (!g_bf1Ext.bf1Enabled || !g_prt || !g_color_ptr)
        return;


    // One-shot probe: fires on the first BF1-enabled frame per loading screen.
    // s_probed is file-scope so hooked_load_config can re-arm it on each new load.
    if (!s_probed) {
        s_probed = true;
        auto fn_log = get_gamelog();
        fn_log("[BF1Ext] RenderScreen BF1 probe: scanLine=%08x "
               "zoomSel=%d(hw=%.3f,hh=%.3f) backdrop=%d anim=%d planet=%d\n",
               g_bf1Ext.scanLineTexHash,
               g_bf1Ext.zoomSelCount, g_bf1Ext.zoomTileHalfW, g_bf1Ext.zoomTileHalfH,
               g_bf1Ext.backdropCount, g_bf1Ext.animCount, g_bf1Ext.planetCount);
        for (int pi = 0; pi < g_bf1Ext.planetCount; ++pi) {
            const Bf1LoadExt::PlanetEntry& pe = g_bf1Ext.planets[pi];
            fn_log("[BF1Ext]   planet[%d] idx=%d tex=%08x x=%.1f y=%.1f w=%.1f h=%.1f\n",
                   pi, pe.levelIndex, pe.texHash, pe.x, pe.y, pe.w, pe.h);
        }
        if (g_pbl_find && g_tex_table) {
            auto probe = [&](const char* label, uint32_t hash) {
                void* entry = g_pbl_find(g_tex_table, 0x2000, hash);
                fn_log("[BF1Ext]   tex_probe %-32s hash=%08x -> %s\n",
                       label, hash, entry ? "FOUND" : "MISSING");
            };
            for (int i = 0; i < g_bf1Ext.backdropCount; ++i)
                probe("backdrop", g_bf1Ext.backdropHashes[i]);
            probe("scanLine", g_bf1Ext.scanLineTexHash);
            for (int i = 0; i < g_bf1Ext.zoomSelCount; ++i)
                probe("zoomSel", g_bf1Ext.zoomSelHashes[i]);
            for (int i = 0; i < g_bf1Ext.animCount; ++i)
                probe("anim", g_bf1Ext.animHashes[i]);
            for (int pi = 0; pi < g_bf1Ext.planetCount; ++pi)
                probe("planet", g_bf1Ext.planets[pi].texHash);
        }
    }

    // PlatformRenderTexture uses NORMALIZED 0.0–1.0 screen coordinates.
    // (0,0,1,1) = full screen. Confirmed from disasm: game's RenderScreen
    // pushes x1=0x3f800000=1.0f for the full-width RandomBackdrop draw.
    // Last 4 args are UV transform (u_scale, v_scale, u_offset, v_offset).
    // Identity = (1,1,0,0). Game always passes (1,1,0,0) for standard draws.
    static constexpr float kW = 1.0f, kH = 1.0f;

    // Switch to RunTimeHeap so that any SortHeap array growth triggered by our
    // g_prt calls allocates from RunTimeHeap (persistent across loading), rather
    // than TempLoadHeap (which LoadDisplay::Update() has made the current heap).
    //
    // Root cause: LoadDisplay::Update calls _RedSetCurrentHeap(s_loadHeap=TempLoadHeap)
    // before Render() → our g_prt calls push to SortHeap with __RedCurrHeap=TempLoadHeap
    // → when SortHeap capacity is exceeded, FUN_00806920 allocates the backing array
    // from TempLoadHeap.  After ReleaseTempHeap destroys TempLoadHeap and fills the
    // block with 0xdede, the SortHeap array pointer is dangling into freed RunTimeHeap
    // memory.  The next SortHeap write corrupts RunTimeHeap's free list, causing a
    // crash at 007E2D77 in _RedGetHeapFree (called by FUN_00488bc0 / PostLoad).
    //
    // Fix: redirect the growth to RunTimeHeap.  _RedFreeToHeap searches all heaps,
    // so the old TempLoadHeap array (if any) is still freed correctly on next resize.
    int prevHeap = -1;
    if (g_set_current_heap && g_runtime_heap_idx)
        prevHeap = g_set_current_heap(*g_runtime_heap_idx);

    // --- PlanetBackdrops: drawn first, full-screen, opaque ------------------
    // Guard: g_prt draws a solid black quad for MISSING hashes — skip draw if
    // the texture isn't in the global table so we don't cover the BF2 background.
    if (g_bf1Ext.backdropCount > 0) {
        const int bi = (g_bf1Ext.backdropCount > 1)
                     ? (int)((GetTickCount() / 5000u) % (DWORD)g_bf1Ext.backdropCount)
                     : 0;
        const uint32_t bHash = g_bf1Ext.backdropHashes[bi];
        if (bHash && g_pbl_find && g_pbl_find(g_tex_table, 0x2000, bHash))
            g_prt(bHash, 0, 0, kW, kH, g_color_ptr, 0,
                  0,0,1,1,  1,1,0,0);
    }

    // --- AnimatedTextures: cycle frames at animFPS ----------------------------
    // Rect from animX,animY,animW,animH; w==0 falls back to full screen (0,0,1,1).
    if (g_bf1Ext.animCount > 0 && g_bf1Ext.animFPS > 0.0f) {
        const DWORD ms_per_frame = (DWORD)(1000.0f / g_bf1Ext.animFPS);
        const int frame = (ms_per_frame > 0)
                        ? (int)(GetTickCount() / ms_per_frame) % g_bf1Ext.animCount
                        : 0;
        if (g_bf1Ext.animHashes[frame]) {
            const float ax = g_bf1Ext.animW > 0.0f ? g_bf1Ext.animX : 0.0f;
            const float ay = g_bf1Ext.animW > 0.0f ? g_bf1Ext.animY : 0.0f;
            const float aw = g_bf1Ext.animW > 0.0f ? g_bf1Ext.animW : 1.0f;
            const float ah = g_bf1Ext.animH > 0.0f ? g_bf1Ext.animH : 1.0f;
            g_prt(g_bf1Ext.animHashes[frame],
                  ax, ay, ax + aw, ay + ah, g_color_ptr, 0,
                  0,0,1,1,  1,1,0,0);
        }
    }

    // --- BF1 zoom-sequence animation ------------------------------------------
    // Faithful recreation of BF1's LoadDisplay::UpdateZoom behaviour, verified
    // against BF1 Ghidra (001baa90).
    //
    // Each transition cycle has 5 sequential phases (all linear — BF1 uses
    // plain lerp throughout, confirmed from Ghidra decompile):
    //
    //   A  kTBMs  Top & bottom bands converge to target Y; L/R sit at edges.
    //   Ah kHoldMs Hold: top/bottom at target Y, left/right still at edges.
    //              (BF1 equivalent: gap between _HALF_TARGET_TIME sub-phases.)
    //   B  kLRMs  Left & right bands converge to target X; T/B stay at target Y.
    //   Bh kHoldMs Hold: all four bands at target rect — visible square pause.
    //   C  kZIMs  Next PlanetLevel zooms in from target rect to full screen;
    //              all bands simultaneously expand back to screen edges.
    //
    // During Phase A/B/Ah/Bh: current planet fills screen opaque.
    // During Phase C: next planet shown full-screen as background; current planet
    // is drawn in the expanding rect with alpha fading from 1→0 (BF1 crossfade).
    //
    // g_animStartMs is captured in hooked_load_config so every match always
    // starts from PlanetLevel 0 at t=0.
    if (g_bf1Ext.planetCount > 0) {
        const int nLev = g_bf1Ext.planetCount;

        // Count valid transitions: contiguous run of levels with non-zero target rect.
        int nTrans = 0;
        for (int i = 0; i < nLev; ++i) {
            const auto& e = g_bf1Ext.planets[i];
            if (e.w > 0.0f && e.h > 0.0f) nTrans++;
            else break;
        }

        // Per-cycle phase durations (ms) — must sum to kAnimCycleMs (file-scope).
        // kTBMs == kLRMs matches BF1's symmetrical _HALF_TARGET_TIME split.
        static constexpr DWORD kTBMs   = 1200u; // Phase A: top/bottom converge
        static constexpr DWORD kHoldMs =  400u; // Phase Ah/Bh: hold between steps
        static constexpr DWORD kLRMs   = 1200u; // Phase B: left/right converge
        static constexpr DWORD kZIMs   = 1500u; // Phase C: zoom-in + bands expand
        static constexpr DWORD kCyMs   = kAnimCycleMs; // 4700 — shared with hooked_load_end

        // Phase start offsets within one cycle
        static constexpr DWORD kOffAh = kTBMs;
        static constexpr DWORD kOffB  = kTBMs + kHoldMs;
        static constexpr DWORD kOffBh = kTBMs + kHoldMs + kLRMs;
        static constexpr DWORD kOffC  = kTBMs + kHoldMs + kLRMs + kHoldMs;

        int   bgLevel = 0;
        int   fgLevel = -1;   // >=0 during Phase C: next planet drawn inside the expanding rect
        float zx0 = 0.0f, zy0 = 0.0f, zx1 = 1.0f, zy1 = 1.0f;
        bool  animDone = false; // true once all transitions have played through

        if (nTrans > 0) {
            const DWORD totalMs    = (DWORD)nTrans * kCyMs;
            const DWORD rawElapsed = GetTickCount() - g_animStartMs;

            if (rawElapsed >= totalMs) {
                // Animation complete — freeze on the final destination planet.
                // Phase C of the last transition zooms the "next" planet to full screen;
                // hold that clean static frame with no crosshair overlay.
                animDone = true;
                bgLevel  = (nTrans < nLev) ? nTrans : nTrans - 1;
                // zx0,zy0,zx1,zy1 keep (0,0,1,1) defaults — full screen.
            } else {
                const DWORD elapsed = rawElapsed % totalMs;
                const int   ti      = (int)(elapsed / kCyMs);    // which transition [0, nTrans)
                const DWORD ph      = elapsed % kCyMs;            // ms within this cycle

                const int   nxt  = (ti + 1 < nLev) ? ti + 1 : ti;
                const auto& cur = g_bf1Ext.planets[ti];
                const float tx0  = cur.x,         ty0 = cur.y;
                const float tx1  = cur.x + cur.w, ty1 = cur.y + cur.h;
                bgLevel = ti;

                if (ph < kOffAh) {
                    // Phase A — top & bottom converge. Smoothstep gives a natural
                    // sweep-in: gentle start, quick middle, smooth settle at target.
                    const float t = anim_smoothstep((float)ph / (float)kTBMs);
                    zx0 = 0.0f;                        zx1 = 1.0f;
                    zy0 = ty0 * t;                     zy1 = 1.0f - (1.0f - ty1) * t;
                }
                else if (ph < kOffB) {
                    // Phase Ah — hold: top/bottom at target Y, left/right at screen edges.
                    zx0 = 0.0f;  zx1 = 1.0f;
                    zy0 = ty0;   zy1 = ty1;
                }
                else if (ph < kOffBh) {
                    // Phase B — left & right converge. Same smoothstep as Phase A.
                    const float t = anim_smoothstep((float)(ph - kOffB) / (float)kLRMs);
                    zx0 = tx0 * t;                     zx1 = 1.0f - (1.0f - tx1) * t;
                    zy0 = ty0;                         zy1 = ty1;
                }
                else if (ph < kOffC) {
                    // Phase Bh — hold: all four bands at the target rect.
                    zx0 = tx0;  zx1 = tx1;
                    zy0 = ty0;  zy1 = ty1;
                }
                else {
                    // Phase C — zoom: next planet appears inside rect and expands with ZoomSelectors.
                    const float a = anim_ease_out((float)(ph - kOffC) / (float)kZIMs);
                    zx0 = tx0 * (1.0f - a);            zy0 = ty0 * (1.0f - a);
                    zx1 = tx1 + (1.0f - tx1) * a;     zy1 = ty1 + (1.0f - ty1) * a;
                    fgLevel = nxt; // draw next planet inside the expanding rect
                }
            }
        }

        // Background — full-screen, opaque.
        {
            const auto& bg = g_bf1Ext.planets[bgLevel];
            if (bg.texHash) {
                g_prt(bg.texHash, 0.0f, 0.0f, 1.0f, 1.0f,
                      g_color_ptr, 0, 0,0,1,1, 1,1,0,0);
            }
        }

        // Foreground — Phase C only: next planet drawn inside the expanding rect.
        if (fgLevel >= 0) {
            const auto& fg = g_bf1Ext.planets[fgLevel];
            if (fg.texHash) {
                g_prt(fg.texHash, zx0, zy0, zx1, zy1,
                      g_color_ptr, 0, 0,0,1,1, 1,1,0,0);
            }
        }

        // ZoomSelector 16-quad crosshair drawn around the animated [zx0,zy0,zx1,zy1] rect.
        // When zx0=0/zx1=1 (Phase A/Ah) the L/R bands sit at the screen edges — barely
        // visible hairlines — which is exactly the BF1 look at the start of a cycle.
        // Suppressed once animDone so the freeze frame is a clean full-screen planet.
        if (g_bf1Ext.zoomSelCount > 0 && !animDone) {
            const float hw = g_bf1Ext.zoomTileHalfW > 0.0f ? g_bf1Ext.zoomTileHalfW : 0.02f;
            const float hh = g_bf1Ext.zoomTileHalfH > 0.0f ? g_bf1Ext.zoomTileHalfH : hw;

            const uint32_t zH = g_bf1Ext.zoomSelHashes[0];
            const uint32_t zV = g_bf1Ext.zoomSelCount > 1 ? g_bf1Ext.zoomSelHashes[1] : zH;
            const uint32_t zC = g_bf1Ext.zoomSelCount > 2 ? g_bf1Ext.zoomSelHashes[2] : zH;

            const float L0=zx0-hw, L1=zx0+hw, R0=zx1-hw, R1=zx1+hw;
            const float T0=zy0-hh, T1=zy0+hh, B0=zy1-hh, B1=zy1+hh;

            // OOB guard: skip zero-area tiles and anything with coords > 2.0
            // (the only way to get >2 now would be a misconfigured ZoomSelectorTileSize).
            auto tile = [&](uint32_t h, float x0_,float y0_,float x1_,float y1_) {
                if (!h || x1_<=x0_ || y1_<=y0_) return;
                if (x0_>2.f||y0_>2.f||x1_>2.f||y1_>2.f) return;
                g_prt(h,x0_,y0_,x1_,y1_, g_color_ptr, 0, 0,0,1,1, 1,1,0,0);
            };

            // BF1 draw order: 6 horizontal strips, 6 vertical strips, 4 corners
            tile(zH,0.0f,T0,L0,T1); tile(zH,L1,T0,R0,T1); tile(zH,R1,T0,1.f,T1);
            tile(zH,0.0f,B0,L0,B1); tile(zH,L1,B0,R0,B1); tile(zH,R1,B0,1.f,B1);
            tile(zV,L0,0.0f,L1,T0); tile(zV,L0,T1,L1,B0); tile(zV,L0,B1,L1,1.f);
            tile(zV,R0,0.0f,R1,T0); tile(zV,R0,T1,R1,B0); tile(zV,R0,B1,R1,1.f);
            tile(zC,L0,T0,L1,T1);   tile(zC,R0,T0,R1,T1);
            tile(zC,L0,B0,L1,B1);   tile(zC,R0,B0,R1,B1);
        }
    }

    // --- ScanLineTexture: full-screen overlay drawn last (on top) -----------
    if (g_bf1Ext.scanLineTexHash)
        g_prt(g_bf1Ext.scanLineTexHash,
              0, 0, kW, kH, g_color_ptr, 0,
              0,0,1,1,  1,1,0,0);

    // Restore the heap that was active before our g_prt calls (TempLoadHeap,
    // as set by LoadDisplay::Update → _RedSetCurrentHeap(s_loadHeap)).
    if (prevHeap >= 0 && g_set_current_heap)
        g_set_current_heap(prevHeap);
}

// =============================================================================
// Play a sound by its hash (FindByHashID → Snd::Sound::Play)
// =============================================================================
void bf1_play_sound(uint32_t sound_hash)
{
    if (!g_find_by_hash || !g_snd_play || !sound_hash) return;
    void* props = g_find_by_hash(sound_hash);
    if (props)
        g_snd_play(0, props, 0, 0, 0);
}

// =============================================================================
// Install / Uninstall
// =============================================================================

static const uintptr_t kUnrelocBase = 0x400000u;

static void* resolve_va(uintptr_t exe_base, uintptr_t va) {
    return (void*)((va - kUnrelocBase) + exe_base);
}

void bf1_load_ext_install(uintptr_t exe_base)
{
    using namespace lua_addrs::modtools;

    g_pbl_ctor       = (fn_pbl_ctor_t)      resolve_va(exe_base, pbl_config_ctor);
    g_pbl_copy_ctor  = (fn_pbl_copy_ctor_t) resolve_va(exe_base, pbl_config_copy_ctor);
    g_pbl_read_data  = (fn_pbl_read_data_t) resolve_va(exe_base, pbl_read_next_data);
    g_pbl_read_scope = (fn_pbl_read_scope_t)resolve_va(exe_base, pbl_read_next_scope);
    g_find_by_hash   = (fn_find_by_hash_t)  resolve_va(exe_base, snd_find_by_hash_id);
    g_snd_play       = (fn_snd_play_t)      resolve_va(exe_base, snd_sound_play);
    g_prt            = (fn_prt_t)           resolve_va(exe_base, platform_render_texture);
    g_color_ptr      = resolve_va(exe_base, color_ptr_global);
    g_set_current_heap = (fn_set_current_heap_t) resolve_va(exe_base, red_set_current_heap);
    g_runtime_heap_idx = (int*)                  resolve_va(exe_base, runtime_heap_global);
    g_s_load_heap_ptr  = (int*)                  resolve_va(exe_base, s_loadheap_global);

    g_hash_string = (fn_hash_string_t)resolve_va(exe_base, hash_string);
    g_pbl_find    = (fn_pbl_find_t)   resolve_va(exe_base, pbl_hash_table_find);
    g_tex_table   =                   resolve_va(exe_base, tex_hash_table);

    // Compute hashes for BF1Ext-only extension parameters at runtime
    // (these don't exist in vanilla BF1, so there are no compile-time constants).
    if (g_hash_string) {
        kHash_ZoomSelectorTileSize = g_hash_string("ZoomSelectorTileSize");
    }

    g_orig_load_data_file = (fn_load_data_file_t)resolve_va(exe_base, load_data_file_real);
    g_orig_load_config    = (fn_load_config_t)   resolve_va(exe_base, load_config_real);
    g_orig_render_screen  = (fn_render_screen_t) resolve_va(exe_base, render_screen_real);
    g_orig_load_end       = (fn_load_end_t)      resolve_va(exe_base, load_end_real);
    g_orig_set_all_on     = (fn_set_all_on_t)    resolve_va(exe_base, progress_set_all_on); // not Detour-hooked
    g_orig_load_update    = (fn_load_update_t)   resolve_va(exe_base, load_update_real);    // Detour-hooked below
    g_orig_load_render    = (fn_load_render_t)   resolve_va(exe_base, load_render_real);    // not Detour-hooked
    g_qpc_stamp           = (DWORD*)             resolve_va(exe_base, load_update_qpc_stamp);

    auto fn_log = get_gamelog();
    fn_log("[BF1Ext] install: exe_base=%08x\n", (unsigned)exe_base);
    fn_log("[BF1Ext]   kHash_LoadingTextColorPallete=%08x kHash_PlanetLevel=%08x\n",
           kHash_LoadingTextColorPallete, kHash_PlanetLevel);
    fn_log("[BF1Ext]   pbl_ctor=%p  copy_ctor=%p  read_data=%p  read_scope=%p\n",
           (void*)g_pbl_ctor, (void*)g_pbl_copy_ctor,
           (void*)g_pbl_read_data, (void*)g_pbl_read_scope);
    fn_log("[BF1Ext]   prt=%p  color_ptr=%p\n", (void*)g_prt, g_color_ptr);
    fn_log("[BF1Ext]   load_data_file=%p  load_config=%p  render_screen=%p\n",
           (void*)g_orig_load_data_file, (void*)g_orig_load_config, (void*)g_orig_render_screen);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_orig_load_data_file, hooked_load_data_file);
    DetourAttach(&(PVOID&)g_orig_load_config,    hooked_load_config);
    DetourAttach(&(PVOID&)g_orig_render_screen,  hooked_render_screen);
    DetourAttach(&(PVOID&)g_orig_load_end,       hooked_load_end);
    DetourAttach(&(PVOID&)g_orig_load_update,    hooked_load_update);
    LONG rc = DetourTransactionCommit();
    fn_log("[BF1Ext]   DetourTransactionCommit = %ld\n", rc);
}

void bf1_load_ext_uninstall()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (g_orig_load_data_file) DetourDetach(&(PVOID&)g_orig_load_data_file, hooked_load_data_file);
    if (g_orig_load_config)    DetourDetach(&(PVOID&)g_orig_load_config,    hooked_load_config);
    if (g_orig_render_screen)  DetourDetach(&(PVOID&)g_orig_render_screen,  hooked_render_screen);
    if (g_orig_load_end)       DetourDetach(&(PVOID&)g_orig_load_end,       hooked_load_end);
    if (g_orig_load_update)    DetourDetach(&(PVOID&)g_orig_load_update,    hooked_load_update);
    DetourTransactionCommit();
}
