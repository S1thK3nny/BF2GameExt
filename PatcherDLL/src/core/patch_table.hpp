#pragma once

#include <stdint.h>

#include "util/slim_vector.hpp"

#define PATCH_COUNT 32
#define EXE_COUNT 3

struct patch_flags {
   /// @brief Address represents a file offset instead of a virtual an unrelocated virtual address.
   bool file_offset : 1 = false;

   /// expected_value is an unrelocated virtual address (what would be displayed in tools like Ghidra/IDA)
   bool expected_is_va : 1 = false;

   /// Compare and write only the low byte of expected_value/replacement_value (for imm8 patches)
   bool values_are_8bit : 1 = false;

   /// Compare and write only the low WORD (for imm16 patches).  Needed where the
   /// operand is a 16-bit immediate and a 32-bit store would run into the next
   /// instruction -- e.g. the modtools mMaxParticles clamp, whose `CMP AX,0x80`
   /// is immediately followed by `MOV word ptr [ESI+2],AX`.
   bool values_are_16bit : 1 = false;

   /// BOTH expected_value and replacement_value are unrelocated virtual
   /// addresses, and both are rebased onto the loaded image at apply time.
   ///
   /// Required for any patch whose VALUE is a pointer into the exe -- a vtable
   /// slot being the obvious case.  The loader applies base relocations to those,
   /// so a raw table constant only matches when the image happens to load at its
   /// preferred base.  Modtools does; the Steam build does not, and the EntityPath
   /// branch region set silently skipped there with
   ///   "site mismatch @ 79c444, expected 6dc930"
   /// because at runtime that slot held 6dc930 + the rebase delta.  It failed
   /// safe, but had it matched we would have written an unrelocated pointer into
   /// a live vtable, which is very much worse than skipping.
   ///
   /// `expected_is_va` covers only the compare side and is what code-immediate
   /// patches use; this covers both sides.  A VA is always four bytes, so this
   /// overrides the 8- and 16-bit width flags rather than combining with them.
   bool values_are_va : 1 = false;
};

struct patch {
   uintptr_t address = 0;
   uint32_t expected_value = 0;
   uint32_t replacement_value = 0;
   patch_flags flags = {};

   /// Late-bound replacement.  When set, the value written is
   /// `*replacement_base + replacement_value`, resolved at apply time instead of
   /// at static-init time, and replacement_value is read as an offset into that
   /// buffer.  This is what lets a relocation buffer be allocated only if its
   /// patch set is actually enabled — a buffer named directly by the table has to
   /// be a DLL global, and a DLL global reserves its address space at load
   /// whether the set applies or not.  See patch_set::prepare.
   const uint32_t* replacement_base = nullptr;
};

/// Runs after the INI toggle passes but before the set is verified or written.
/// Returning false skips the set exactly as a site mismatch would, so a failed
/// allocation degrades to "feature off" instead of taking the process down.
using patch_prepare_fn = bool (*)();

struct patch_set {
   const char* name = "";

   /// Optional; see patch_prepare_fn. Sets without late-bound buffers leave it null.
   patch_prepare_fn prepare = nullptr;

   slim_vector<patch> patches;
};

struct exe_patch_list {
   const char* name = "";

   bool id_address_is_file_offset = false;

   uintptr_t id_address = 0;
   uint64_t expected_id = 0;

   const patch_set patches[PATCH_COUNT];
};

extern const exe_patch_list patch_lists[EXE_COUNT];

// Renderer cache storage — redirected from s_caches[15] by the "Particle Cache
// Increase" binary patches.  The base redirect alone doesn't raise the 15-slot
// allocation clamp in SetCurrentCache; gc_visual_limits.cpp patches that clamp
// to RENDERER_CACHE_SLOTS (and spills full caches into the spare slots) when it
// detects the redirect is active.
static constexpr uint32_t RENDERER_CACHE_SLOTS = 120;
extern char g_sCaches_storage[];

// Initialize the sentinel value at the end of the relocated EntityEx::mIdMap hash table.
// The RTTI class name differs per build (different BSS layouts after mIdMap).
void init_object_limit_sentinel(const char* rtti_class_name);

// Concurrent Lua OpenAudioStream slots, raised from the stock 6 by the "Audio
// Stream Limit Increase" binary patches.  Each slot is a 0x3611BC-byte
// Snd::Stream living in snd_stream_storage, which Snd::EngineBase::smStreams is
// repointed at.  Raising this costs ~3.4 MB of address space per extra slot.
static constexpr uint32_t AUDIO_STREAM_SLOTS = 12;
static constexpr uint32_t AUDIO_STREAM_SLOTS_STOCK = 6;
static constexpr uint32_t AUDIO_STREAM_SIZE = 0x3611BC; // sizeof(Snd::Stream)

// VirtualAlloc'd by the patch set's prepare(), so a disabled AudioStreamLimit
// costs nothing.  Null until then — the runtime half only runs once it has
// confirmed the set applied, so it never sees these unset.
extern char* snd_stream_storage;
extern char* snd_stream_queue_storage; // PblListDoubleSorted[AUDIO_STREAM_SLOTS], 12 bytes each

// smPlayingProps[], relocated alongside the stream array.
char* audio_stream_playing_props();

// Relocated EntityEx::mIdMap buffer (N=2048).
// Layout: [uint32 count] [uint32 keys[2048]] [uint32 values[2048]]
// values[i] are EntityEx* pointers (0 = empty slot).
extern char EntityEx_mIdMap_new[];
static constexpr uint32_t ENTITY_MAP_BUCKETS_NEW = 2048;