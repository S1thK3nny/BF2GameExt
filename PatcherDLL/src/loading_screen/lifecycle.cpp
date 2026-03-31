#include "pch.h"
#include "shared.hpp"
#include "core/game_addrs.hpp"

#include <detours.h>

// =============================================================================
// Tracking sound state (file-local — only accessed by sound functions below)
// =============================================================================

static GameSoundControllable g_trackCtrl   = {};
static bool                  g_trackActive = false;
static void*                 g_trackVoice  = nullptr;

// End-hook state (file-local)
static bool g_inRealEnd = false;

// =============================================================================
// Sound helpers
// =============================================================================

// TODO: Sound cuts off abruptly — needs a short fade-out to avoid being harsh on the ears.
// TODO: If the player alt-tabs at a specific moment, the sound
//       doesn't get stopped (tracking_sound_stop is likely skipped or the voice handle is stale).

// Stop the current tracking sound (if any is playing).
//
// Stop sequence:
//   1. VoiceVirtualRelease — clears loop-restart callback at +0xc0/+0xc4
//   2. Set Voice->field_0x80 |= 2 — triggers SW resampler stop on next Update
//   3. Tick audio engine — Voice::Update processes the stop
void tracking_sound_stop()
{
    if (!g_trackActive) return;

    if (g_trackVoice && g_vvrelease && g_voice_to_handle) {
        uint8_t* pVV = (uint8_t*)g_trackVoice;

        uint16_t handle = (uint16_t)g_voice_to_handle(g_trackVoice);
        g_trackCtrl.mVoiceVirtualHandle = handle;
        g_vvrelease(&g_trackCtrl, nullptr);

        void* pVoice = *(void**)(pVV + 0xa0);
        if (pVoice) {
            uint8_t* v = (uint8_t*)pVoice;
            v[0x80] |= 0x02;
        }

        if (g_snd_update) {
            g_snd_update(0.016f, 1);
            g_snd_update(0.016f, 1);
        }

        g_trackVoice = nullptr;
    }

    memset(&g_trackCtrl, 0, sizeof(g_trackCtrl));
    g_trackActive = false;
}

// Start a looping/controllable tracking sound.
// Stops any currently playing tracking sound first.
void tracking_sound_start(uint32_t hash)
{
    tracking_sound_stop();
    if (!g_snd_play_ex || !g_find_by_hash || !hash) return;
    void* props = g_find_by_hash(hash);
    if (!props) {
        auto fn_log = get_gamelog();
        fn_log("[BF1Ext] ERROR: tracking_sound_start — sound hash %08x not found\n", hash);
        return;
    }

    memset(&g_trackCtrl, 0, sizeof(g_trackCtrl));

    const uint8_t loopBit      = (*(const uint8_t*)((const uint8_t*)props + 0x1c)) & 0x10;
    g_trackCtrl.mFlags         = (uint8_t)((loopBit | 8u) >> 3);

    // Reset nextAllowedTime cooldown so Play always proceeds.
    *(float*)((uint8_t*)props + 0x68) = 0.0f;

    void* voice = g_snd_play_ex(nullptr, props, (void*)0x0040360c, &g_trackCtrl, 0);
    if (voice && g_voice_to_handle)
        g_trackCtrl.mVoiceVirtualHandle = (uint16_t)g_voice_to_handle(voice);

    g_trackVoice = voice;
    g_trackActive = true;
}

// Play a one-shot sound by its hash.
void loading_screen_play_sound(uint32_t sound_hash)
{
    if (!g_find_by_hash || !g_snd_play || !sound_hash) return;
    void* props = g_find_by_hash(sound_hash);
    if (!props) {
        auto fn_log = get_gamelog();
        fn_log("[BF1Ext] ERROR: loading_screen_play_sound — sound hash %08x not found\n", sound_hash);
        return;
    }
    *(float*)((uint8_t*)props + 0x68) = 0.0f;
    g_snd_play(0, props, 0, 0, 0);
}

// =============================================================================
// LoadDataFile hook
// =============================================================================

void __fastcall hooked_load_data_file(void* ecx, void* edx, const char* lvlPath)
{
    g_orig_load_data_file(ecx, edx, lvlPath);

    // Load the BF1-ext sound LVL once per loading screen.
    if (g_loadScreenCfg.bf1Enabled && g_loadScreenCfg.loadSoundLvl[0] && !s_sndLvlLoaded) {
        s_sndLvlLoaded = true;
        g_orig_load_data_file(ecx, edx, g_loadScreenCfg.loadSoundLvl);
        s_lastAnimPhase = -1;
        s_lastAnimCycle = -1;
    }
}

// =============================================================================
// LoadDisplay::Update hook — smooth asset-loading phase to ~30 fps
// =============================================================================

void __fastcall hooked_load_update(void* ecx, void* edx)
{
    if (g_inRealEnd) {
        return;
    }

    // BF1 mode: redirect s_loadHeap -> RunTimeHeap for the entire Update call.
    int saved_load_heap = -1;
    if (g_loadScreenCfg.bf1Enabled && g_s_load_heap_ptr && g_runtime_heap_idx) {
        saved_load_heap      = *g_s_load_heap_ptr;
        *g_s_load_heap_ptr   = *g_runtime_heap_idx;
    }

    const DWORD qpc_before = g_qpc_stamp ? *g_qpc_stamp : 0;
    g_orig_load_update(ecx, edx);

    if (saved_load_heap >= 0 && g_s_load_heap_ptr)
        *g_s_load_heap_ptr = saved_load_heap;

    // Tick the audio engine so queued voices are mixed to hardware.
    if (g_loadScreenCfg.bf1Enabled && g_snd_update) {
        const DWORD now = GetTickCount();
        const DWORD ms  = now - g_lastSndUpdateMs;
        if (ms > 0 && ms < 1000u) {
            g_snd_update((float)ms * 0.001f, 1);
        }
        g_lastSndUpdateMs = now;
    }

    if (!g_loadScreenCfg.bf1Enabled || !g_orig_load_render) return;

    if (g_qpc_stamp && *g_qpc_stamp != qpc_before) {
        g_lastRenderMs = GetTickCount();
    } else if (ecx && *(const uint8_t*)ecx != 0
               && GetTickCount() - g_lastRenderMs >= 33u) {
        g_lastRenderMs = GetTickCount();
        {
            int prevRenderHeap = -1;
            if (g_set_current_heap && g_runtime_heap_idx)
                prevRenderHeap = g_set_current_heap(*g_runtime_heap_idx);
            g_orig_load_render(ecx, nullptr);
            if (prevRenderHeap >= 0 && g_set_current_heap)
                g_set_current_heap(prevRenderHeap);
        }
    }
}

// =============================================================================
// LoadDisplay::End hook — delays teardown until animation completes
// =============================================================================

void __fastcall hooked_load_end(void* ecx, void* edx)
{
    if (g_endProcessed) return;

    if (g_loadScreenCfg.bf1Enabled && g_animStartMs != 0 && g_orig_load_update) {
        int nTrans = 0;
        for (int i = 0; i < g_loadScreenCfg.planetCount; ++i) {
            const auto& e = g_loadScreenCfg.planets[i];
            if (e.w > 0.0f && e.h > 0.0f) nTrans++;
            else break;
        }
        if (nTrans > 0) {
            const DWORD required = (DWORD)nTrans * kAnimCycleMs;
            const DWORD deadline = GetTickCount() + 30000u;

            if (g_orig_set_all_on && ecx)
                g_orig_set_all_on((uint8_t*)ecx + 0xd30, nullptr);

            DWORD lastUpdateMs = 0;
            while (GetTickCount() - g_animStartMs < required) {
                if (GetTickCount() >= deadline) break;
                const DWORD now = GetTickCount();

                MSG msg;
                while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }

                if (now - lastUpdateMs >= 200u) {
                    lastUpdateMs = now;
                    const DWORD qpc_before = g_qpc_stamp ? *g_qpc_stamp : 0;
                    int saved_load_heap_end = -1;
                    if (g_s_load_heap_ptr && g_runtime_heap_idx) {
                        saved_load_heap_end    = *g_s_load_heap_ptr;
                        *g_s_load_heap_ptr     = *g_runtime_heap_idx;
                    }
                    g_orig_load_update(ecx, nullptr);
                    if (saved_load_heap_end >= 0 && g_s_load_heap_ptr)
                        *g_s_load_heap_ptr = saved_load_heap_end;
                    if (g_qpc_stamp && *g_qpc_stamp != qpc_before)
                        g_lastRenderMs = GetTickCount();

                    if (g_snd_update) {
                        const DWORD sndNow = GetTickCount();
                        const DWORD sndMs  = sndNow - g_lastSndUpdateMs;
                        if (sndMs > 0 && sndMs < 1000u)
                            g_snd_update((float)sndMs * 0.001f, 1);
                        g_lastSndUpdateMs = sndNow;
                    }
                }
                if (g_orig_load_render && GetTickCount() - g_lastRenderMs >= 33u) {
                    g_lastRenderMs = GetTickCount();
                    int prevRenderHeap = -1;
                    if (g_set_current_heap && g_runtime_heap_idx)
                        prevRenderHeap = g_set_current_heap(*g_runtime_heap_idx);
                    g_orig_load_render(ecx, nullptr);
                    if (prevRenderHeap >= 0 && g_set_current_heap)
                        g_set_current_heap(prevRenderHeap);
                }
                Sleep(1);
            }
        }
    }

    tracking_sound_stop();
    g_endProcessed = true;
    g_inRealEnd = true;
    g_orig_load_end(ecx, edx);
    g_inRealEnd = false;
}

// =============================================================================
// Install / Uninstall
// =============================================================================

void loading_screen_install(uintptr_t exe_base)
{
    using namespace game_addrs::modtools;

    g_pbl_ctor       = (fn_pbl_ctor_t)      resolve(exe_base, pbl_config_ctor);
    g_pbl_copy_ctor  = (fn_pbl_copy_ctor_t) resolve(exe_base, pbl_config_copy_ctor);
    g_pbl_read_data  = (fn_pbl_read_data_t) resolve(exe_base, pbl_read_next_data);
    g_pbl_read_scope = (fn_pbl_read_scope_t)resolve(exe_base, pbl_read_next_scope);
    g_find_by_hash   = (fn_find_by_hash_t)  resolve(exe_base, snd_find_by_hash_id);
    g_snd_play        = (fn_snd_play_t)        resolve(exe_base, snd_sound_play);
    g_snd_play_ex     = (fn_snd_play_ex_t)     resolve(exe_base, snd_sound_play);
    g_voice_to_handle = (fn_voice_to_handle_t) resolve(exe_base, voice_to_handle);
    g_vvrelease       = (fn_vvrelease_t)       resolve(exe_base, voice_virtual_release);
    g_snd_update      = (fn_snd_eng_update_t)  resolve(exe_base, snd_engine_update);
    g_lastSndUpdateMs = GetTickCount();
    g_prt            = (fn_prt_t)           resolve(exe_base, platform_render_texture);
    g_color_ptr      = resolve(exe_base, color_ptr_global);
    g_set_current_heap = (fn_set_current_heap_t) resolve(exe_base, red_set_current_heap);
    g_runtime_heap_idx = (int*)                  resolve(exe_base, runtime_heap_global);
    g_s_load_heap_ptr  = (int*)                  resolve(exe_base, s_loadheap_global);

    g_hash_string = (fn_hash_string_t)resolve(exe_base, hash_string);
    g_pbl_find    = (fn_pbl_find_t)   resolve(exe_base, pbl_hash_table_find);
    g_tex_table   =                   resolve(exe_base, tex_hash_table);

    if (g_hash_string) {
        kHash_ZoomSelectorTileSize = g_hash_string("ZoomSelectorTileSize");
        kHash_LoadSoundLVL         = g_hash_string("LoadSoundLVL");
        kHash_PC               = g_hash_string("PC");
        kHash_PS2              = g_hash_string("PS2");
        kHash_XBOX             = g_hash_string("XBOX");
        kHash_RemoveToolTips   = g_hash_string("RemoveToolTips");
        kHash_RemoveLoadingBar = g_hash_string("RemoveLoadingBar");
    }

    g_orig_load_data_file = (fn_load_data_file_t)resolve(exe_base, load_data_file_real);
    g_orig_load_config    = (fn_load_config_t)   resolve(exe_base, load_config_real);
    g_orig_render_screen  = (fn_render_screen_t) resolve(exe_base, render_screen_real);
    g_orig_load_end       = (fn_load_end_t)      resolve(exe_base, load_end_real);
    g_orig_set_all_on     = (fn_set_all_on_t)    resolve(exe_base, progress_set_all_on);
    g_orig_load_update    = (fn_load_update_t)   resolve(exe_base, load_update_real);
    g_orig_load_render    = (fn_load_render_t)   resolve(exe_base, load_render_real);
    g_qpc_stamp           = (DWORD*)             resolve(exe_base, load_update_qpc_stamp);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_orig_load_data_file, hooked_load_data_file);
    DetourAttach(&(PVOID&)g_orig_load_config,    hooked_load_config);
    DetourAttach(&(PVOID&)g_orig_render_screen,  hooked_render_screen);
    DetourAttach(&(PVOID&)g_orig_load_end,       hooked_load_end);
    DetourAttach(&(PVOID&)g_orig_load_update,    hooked_load_update);
    DetourTransactionCommit();
}

void loading_screen_uninstall()
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
