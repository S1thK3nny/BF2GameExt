#include "pch.h"
#include "audio_stream_limit.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/patch_table.hpp"
#include "core/resolve.hpp"

#include <detours.h>

// =============================================================================
// Audio Stream Limit Increase — runtime half
// =============================================================================
// The binary patch set of the same name raises the concurrent Lua
// OpenAudioStream limit from 6 to AUDIO_STREAM_SLOTS: it repoints
// Snd::EngineBase::smStreams (a pointer, not the array) and the five
// Snd::SoundStream arrays indexed by stream slot at our own buffers, and widens
// every loop bound — most of which are byte bounds (6 * 0x3611BC = 0x1446A68)
// rather than counts.
//
// Snd::SoundStream::Init is the one place that cannot be widened by rewriting
// immediates: it *unrolls* its per-slot work six times,
//
//     mov dword ptr [smPlayingProps + k*4], 0     ; k = 0..5
//     or  byte  ptr [smQueue[k].mFlags], 1        ; k = 0..5
//
// so patching those twelve operands only ever covers slots 0..5.  Init runs on
// every engine open and close, so the extra slots would keep a stale Properties*
// across an engine restart and would never get the queue's sort-order flag set
// by PblListDoubleSorted.  Prefix the function and do slots 6..N-1 ourselves.
//
// The queue *objects* themselves need no construction here — the CRT vector
// ctor/dtor iterators that build smQueue are counted loops, so the patch set
// widens them to AUDIO_STREAM_SLOTS and they run before any of this.
//
// The second hook here is diagnostic only: it logs when a script is handed a
// stream slot that would not exist on a stock install.
// =============================================================================

namespace {

using fn_soundstream_init_t = void(__cdecl*)();
fn_soundstream_init_t original_soundstream_init = nullptr;

using fn_get_free_stream_t = void*(__cdecl*)();
fn_get_free_stream_t original_get_free_stream = nullptr;

constexpr uint32_t kQueueStride    = 12; // sizeof(PblListDoubleSorted)
constexpr uint32_t kQueueFlagsOff  = 8;  // PblListDoubleSorted::mFlags
constexpr uint8_t  kQueueSortedBit = 1;

// One warning per engine session rather than one per stream — Init runs on every
// engine open/close, so this re-arms for each mission load.
bool reported_this_session = false;

void __cdecl init_extra_slots()
{
   uint32_t* props = reinterpret_cast<uint32_t*>(audio_stream_playing_props());

   for (uint32_t i = AUDIO_STREAM_SLOTS_STOCK; i < AUDIO_STREAM_SLOTS; ++i) {
      props[i] = 0;
      snd_stream_queue_storage[i * kQueueStride + kQueueFlagsOff] |= kQueueSortedBit;
   }

   reported_this_session = false;
}

// Init takes no arguments and returns void, but it is reached from LTCG code on
// the retail builds, so save and restore everything rather than trusting the
// standard volatile set, then tail-jump into the trampoline.
__declspec(naked) void __cdecl hooked_soundstream_init()
{
   __asm {
      pushad
      pushfd
      call init_extra_slots
      popfd
      popad
      jmp  [original_soundstream_init]
   }
}

// ---------------------------------------------------------------------------
// Vanilla-compatibility warning
// ---------------------------------------------------------------------------
// GetFreeStream is the only thing that hands out a stream slot, and the Lua
// OpenAudioStream callback is its only caller — so every handle a script gets
// passes through here exactly once.  Anything from slot 6 up exists only because
// this extension is installed: the same script would get a null stream and the
// engine's "Maximum number of open audio streams exceeded" on a stock install.
// Say so in the log while the modder can still see it.

void __cdecl report_extension_slot(void* stream)
{
   if (!stream || reported_this_session) return;

   const uintptr_t offset = (uintptr_t)stream - (uintptr_t)&snd_stream_storage[0];
   const uint32_t slot = (uint32_t)(offset / AUDIO_STREAM_SIZE);

   if (slot >= AUDIO_STREAM_SLOTS || slot < AUDIO_STREAM_SLOTS_STOCK) return;

   reported_this_session = true;
   warn_gamelog(RED_SEVERITY_INFO, SRC_FILE, __LINE__,
                "[AudioStreamExt] Detected more than %u OpenAudioStream calls. "
                "Please keep in mind, you will be missing ambiance without BF2GameExt.",
                AUDIO_STREAM_SLOTS_STOCK);
}

// Returns Stream* in EAX.  PUSHAD after the call captures that return value and
// POPAD restores it, so the logging call cannot disturb what the caller sees.
__declspec(naked) void* __cdecl hooked_get_free_stream()
{
   __asm {
      call [original_get_free_stream]
      pushad
      pushfd
      push eax
      call report_extension_slot
      add  esp, 4
      popfd
      popad
      ret
   }
}

} // namespace

void audio_stream_limit_install(uintptr_t exe_base)
{
   if (AUDIO_STREAM_SLOTS <= AUDIO_STREAM_SLOTS_STOCK) return;
   if (!g_addr->snd_soundstream_init || !g_addr->snd_stream_slot_count_imm8) return;

   // Only hook if the patch set actually landed — it is INI-toggleable, and
   // apply_patches skips a whole set whose sites fail verification.  Without it
   // the game still owns the original 6-slot arrays and our buffers are unused.
   const uint8_t slots =
      *reinterpret_cast<uint8_t*>(resolve(exe_base, g_addr->snd_stream_slot_count_imm8));
   if (slots != AUDIO_STREAM_SLOTS) return;

   original_soundstream_init =
      reinterpret_cast<fn_soundstream_init_t>(resolve(exe_base, g_addr->snd_soundstream_init));

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_soundstream_init, hooked_soundstream_init);
   if (DetourTransactionCommit() != NO_ERROR)
      original_soundstream_init = nullptr;

   if (!g_addr->snd_engine_get_free_stream) return;

   original_get_free_stream =
      reinterpret_cast<fn_get_free_stream_t>(resolve(exe_base, g_addr->snd_engine_get_free_stream));

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_get_free_stream, hooked_get_free_stream);
   if (DetourTransactionCommit() != NO_ERROR)
      original_get_free_stream = nullptr;
}

void audio_stream_limit_uninstall()
{
   if (original_soundstream_init) {
      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourDetach(&(PVOID&)original_soundstream_init, hooked_soundstream_init);
      DetourTransactionCommit();
      original_soundstream_init = nullptr;
   }

   if (original_get_free_stream) {
      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourDetach(&(PVOID&)original_get_free_stream, hooked_get_free_stream);
      DetourTransactionCommit();
      original_get_free_stream = nullptr;
   }
}
