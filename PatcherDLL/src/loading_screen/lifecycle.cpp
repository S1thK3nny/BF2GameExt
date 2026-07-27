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

// One-shot sounds are owned rather than fired and forgotten, so that
// loading_screen_stop_all_sounds() can silence them when the screen ends. Without
// this a ZoomSound or BarSound started late keeps playing into the match — most
// visibly when the player alt-tabs and the load stalls mid-sequence.
static constexpr int kMaxOneShots = 16;
struct OwnedSound {
    GameSoundControllable ctrl;
    void*                 vv;   // VoiceVirtual* as returned by Play
};
static OwnedSound g_oneShots[kMaxOneShots] = {};
static int        g_oneShotNext = 0;

// Retire one owned sound.
//
// The gentle path clears the VoiceVirtual's looping flag and then releases
// ownership without stopping anything. Snd::VoiceVirtual::Update notices the flag
// is clear once the play position passes the end of the sample and calls Stop
// itself, so the sound finishes the pass it is already playing instead of being
// cut off mid-waveform, and the FireForget callback installed by ReleaseVoice
// frees it afterwards. See the note on kVoiceVirtual_LoopByte in shared.hpp.
//
// hardStop instead runs GameSoundControllable::Stop(true) -> Snd::VoiceVirtual::Stop,
// which silences the voice on the spot. Only for cases where a sound genuinely
// must be quiet this instant.
static void snd_ctrl_retire(GameSoundControllable* ctrl, void* vv, bool hardStop = false)
{
    if (!ctrl) return;

    // Staleness guard: the pointer must still map back to the handle we hold,
    // otherwise the voice was recycled and vv now belongs to somebody else.
    if (!hardStop && vv && ctrl->mVoiceVirtualHandle != 0 && g_voice_to_handle
        && (uint16_t)g_voice_to_handle(vv) == ctrl->mVoiceVirtualHandle) {
        uint8_t* loop = (uint8_t*)vv + kVoiceVirtual_LoopByte;
        *loop &= (uint8_t)~kVoiceVirtual_LoopBit;
    }

    if (ctrl->mVoiceVirtualHandle != 0 && g_snd_ctrl_stop)
        g_snd_ctrl_stop(ctrl, nullptr, hardStop ? 1 : 0);

    memset(ctrl, 0, sizeof(*ctrl));
}

// Stop the current tracking sound, if any is playing.
void tracking_sound_stop()
{
    snd_ctrl_retire(&g_trackCtrl, g_trackVoice);
    g_trackVoice  = nullptr;
    g_trackActive = false;
}

// Release everything this module started. Called on the way out of the loading
// screen, on every path, so no loop can survive into the match.
void loading_screen_stop_all_sounds()
{
    tracking_sound_stop();

    for (int i = 0; i < kMaxOneShots; ++i) {
        snd_ctrl_retire(&g_oneShots[i].ctrl, g_oneShots[i].vv);
        g_oneShots[i].vv = nullptr;
    }
    g_oneShotNext = 0;
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

// Play a one-shot sound by its hash, keeping ownership so it can be stopped.
void loading_screen_play_sound(uint32_t sound_hash)
{
    if (!g_find_by_hash || !g_snd_play_ex || !sound_hash) return;
    void* props = g_find_by_hash(sound_hash);
    if (!props) {
        warn_gamelog(RED_SEVERITY_ERROR, SRC_FILE, __LINE__,
                     "[BF1Ext] loading_screen_play_sound - sound hash %08x not found\n",
                     sound_hash);
        return;
    }

    // Recycle the oldest slot. Retiring first matters: reusing a slot whose handle
    // is still live would orphan that voice with no way to reach it. Retiring is
    // soft, so an older one-shot still playing is left to finish.
    OwnedSound* slot = &g_oneShots[g_oneShotNext];
    g_oneShotNext = (g_oneShotNext + 1) % kMaxOneShots;
    snd_ctrl_retire(&slot->ctrl, slot->vv);
    slot->vv = nullptr;

    // Reset nextAllowedTime cooldown so Play always proceeds.
    *(float*)((uint8_t*)props + 0x68) = 0.0f;

    void* voice = g_snd_play_ex(nullptr, props, (void*)0x0040360c, &slot->ctrl, 0);
    if (voice && g_voice_to_handle)
        slot->ctrl.mVoiceVirtualHandle = (uint16_t)g_voice_to_handle(voice);
    slot->vv = voice;
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
    } else if (ecx && *load_display::at<uint8_t>(ecx, load_display::kBDisplay) != 0
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
    // Before the g_endProcessed gate, not after: if End is reached twice, the
    // second call must still be able to silence anything left running.
    loading_screen_stop_all_sounds();

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
                g_orig_set_all_on(load_display::at<uint8_t>(ecx, load_display::kProgressBar),
                                  nullptr);

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

    // Again after the spin loop, which plays sounds of its own as the animation
    // finishes.
    loading_screen_stop_all_sounds();

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
    g_snd_ctrl_stop   = (fn_snd_ctrl_stop_t)   resolve(exe_base, gamesound_controllable_stop);
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
        kHash_RemoveToolTips    = g_hash_string("RemoveToolTips");
        kHash_RemoveLoadingBar  = g_hash_string("RemoveLoadingBar");
        kHash_RemoveLoadingText = g_hash_string("RemoveLoadingText");
        kHash_RemoveMissionName = g_hash_string("RemoveMissionName");
        kHash_RemoveModeName    = g_hash_string("RemoveModeName");
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

    // Must follow the LoadConfig detour: the guard calls LoadConfig's raw entry
    // point so hooked_load_config stays in the chain, exactly as vanilla does.
    load_data_guard_install(exe_base);
}

void loading_screen_uninstall()
{
    load_data_guard_uninstall();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (g_orig_load_data_file) DetourDetach(&(PVOID&)g_orig_load_data_file, hooked_load_data_file);
    if (g_orig_load_config)    DetourDetach(&(PVOID&)g_orig_load_config,    hooked_load_config);
    if (g_orig_render_screen)  DetourDetach(&(PVOID&)g_orig_render_screen,  hooked_render_screen);
    if (g_orig_load_end)       DetourDetach(&(PVOID&)g_orig_load_end,       hooked_load_end);
    if (g_orig_load_update)    DetourDetach(&(PVOID&)g_orig_load_update,    hooked_load_update);
    DetourTransactionCommit();
}
