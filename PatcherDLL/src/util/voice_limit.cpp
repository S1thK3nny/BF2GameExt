#include "pch.h"
#include "voice_limit.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <string.h>

// See voice_limit.hpp for the mechanism and why the probe array has to move.

int g_voiceLimit = 0;
bool g_softwareVoices = false;

namespace {

constexpr int kMinVoices = 33;  // below this there is nothing to gain
constexpr int kMaxVoices = 119; // PUSH imm8 of (N + 8) must stay under 128

constexpr uint32_t kVoiceStride  = 0x540; // sizeof(Snd::Voice)
constexpr uint32_t kDSBufferSize = 0x40;  // one probe element
constexpr uint32_t kProbeSpare   = 8;     // the probe asks for gMaxVoices + 8

// ---------------------------------------------------------------------------
// Sites (modtools).  Every one is verified against its expected bytes before
// anything is written, and a single mismatch abandons the whole feature -- a
// half-applied voice raise would have the engine walking a pool that is not the
// size it thinks it is.
// ---------------------------------------------------------------------------
constexpr uintptr_t kGMaxVoices      = 0x00ADD474; // the global itself
constexpr uintptr_t kClampCmpImm8    = 0x00446A4F; // CMP EAX,0x20  (cmdline clamp)
constexpr uintptr_t kClampStoreImm32 = 0x00446A5C; // MOV [gMaxVoices],0x20

constexpr uintptr_t kPoolPtrImm32    = 0x00882C49; // MOV [smVoices],0x00EDFE18
constexpr uintptr_t kOpenBound       = 0x00882CF7; // CMP EBP,0xA800
constexpr uintptr_t kUpdateBound     = 0x00882825; // CMP ESI,0xA800
constexpr uintptr_t kCloseBound      = 0x00882B5A; // CMP ESI,0xA800
constexpr uintptr_t kCentrePeakBound = 0x008851A0; // CMP EAX,0xA800

constexpr uintptr_t kHwCeilCmpImm8   = 0x00886B58; // CMP EDI,0x20
constexpr uintptr_t kHwCeilLoadImm32 = 0x00886B62; // MOV EDI,0x20
constexpr uintptr_t kSwCeilCmpImm8   = 0x00886BDA; // CMP EBX,0x20
constexpr uintptr_t kSwCeilLoadImm32 = 0x00886BE0; // MOV EDI,0x20

constexpr uintptr_t kProbeCtorCount  = 0x008866B5; // PUSH 0x28
constexpr uintptr_t kProbeDtorCount  = 0x00886788; // PUSH 0x28

// ---------------------------------------------------------------------------
// The software mixer
// ---------------------------------------------------------------------------
// SoftOutput (static, 0x02331218) owns four per-input arrays laid out back to
// back inside itself with no slack:
//
//   +0x0CC  gain / ramp matrix   2 * N * outChannels ints   0x200 at N=32
//   +0x2CC  connection table     8 bytes per input          0x100
//   +0x3CC  packet holders       8 bytes per input          0x100
//   +0x4CC  per-input offsets    4 bytes per input          0x080
//
// Every runtime access goes through a pointer the mixer stores, so all four can
// be moved to DLL-owned buffers; the addresses are only ever formed in
// SetOutputBufferSize, as four 6-byte `LEA r32,[ESI+disp32]` that a 5-byte
// MOV r32,imm32 plus a NOP fits inside.
//
// The element constructors and destructors are deliberately NOT touched. They
// run over the original in-struct arrays with their own count of 32, which is
// exactly correct for a region that is 32 entries wide and simply goes unused
// afterwards. Leaving them alone also leaves the two exception-unwind funclets
// at 0x00A120CF / 0x00A1210F correct, which would otherwise have to be found and
// patched in lockstep. The element constructor only zeroes two dwords, and
// VirtualAlloc hands back zeroed pages, so the replacement buffers start in the
// state the constructor would have produced.
//
// Three constraints on the input count, all load-bearing:
//
//   * The mix pass clears the per-input bitmap with a `count >> 3` byte loop
//     (0x008980DF..0x008980FB), so a count that is not a multiple of 8 leaves the
//     top (count mod 8) bits stale. The mixer count is therefore always rounded
//     UP to a multiple of 8 -- which is free, because spare inputs simply go
//     unconnected.
//   * The gain matrix's second plane is indexed at outChannels * count,
//     recomputed from the CURRENT stored count on every access, so the count and
//     the gain relocation have to land together or the very first mix writes far
//     past a 0x200-byte matrix.
//   * GetUnconnectedInput does NOT consult the stored count -- it has its own
//     `CMP ESI,0x20` (0x008A0054), and since SoftOutput::ConnectInput is an
//     unchecked thunk, that compare is the only gate on how many inputs are ever
//     handed out.
constexpr uintptr_t kMixerCountImm8 = 0x0089FB3C; // PUSH 0x20 -> input count
constexpr uintptr_t kMixerGateImm8  = 0x008A0056; // CMP ESI,0x20 in GetUnconnectedInput

struct MixerArray {
   uintptr_t va;         // the LEA
   uint8_t   lea[6];     // expected bytes
   uint8_t   movOp;      // B8 EAX / B9 ECX / BA EDX
   uint32_t  perEntry;   // bytes per input
   uint32_t  multiplier; // extra factor (the gain matrix is 2 * outChannels deep)
};

// outChannels is fixed at 2 for this mixer (SetChannels(1,2) at 0x0089FB30), but
// the buffers are sized for 8 so a build that reports more cannot overrun.
constexpr MixerArray kMixerArrays[] = {
   {0x0089FB3D, {0x8D, 0x8E, 0xCC, 0x02, 0x00, 0x00}, 0xB9, 8, 1},  // conn table
   {0x0089FB4C, {0x8D, 0x96, 0xCC, 0x00, 0x00, 0x00}, 0xBA, 4, 16}, // gain matrix
   {0x0089FB62, {0x8D, 0x86, 0xCC, 0x04, 0x00, 0x00}, 0xB8, 4, 1},  // offsets
   {0x0089FB69, {0x8D, 0x8E, 0xCC, 0x03, 0x00, 0x00}, 0xB9, 8, 1},  // packet holders
};

constexpr int kMixerArrayCount = (int)(sizeof(kMixerArrays) / sizeof(kMixerArrays[0]));

// Software mixing takes gMaxVoices as its voice count directly:
//
//   00886BB0  MOV EBX,[ESP+0x12C4]   ; gMaxVoices
//   00886BC1  loop: Voice::Initialize x EBX
//
// but in that configuration SoftOutput really is the mixer, and it has 32 inputs
// in its own struct.  Voice 33 would get mSoftConnIdx = -1, occupy a pool slot,
// be handed out by the manager and produce nothing -- strictly worse than stock.
// So the software branch is pinned back to 32 while gMaxVoices stays raised for
// the probe.  The `lock the remainder` loop below it still runs over the whole
// pool (its two ceilings ARE raised), so voices 32..N-1 are locked out rather
// than left constructed-but-unusable.
constexpr uintptr_t kSwVoiceCountPin = 0x00886BB0;
constexpr uint8_t   kSwPinExpect[7]  = {0x8B, 0x9C, 0x24, 0xC4, 0x12, 0x00, 0x00};
constexpr uint8_t   kSwPinPatch[7]   = {0xBB, 0x20, 0x00, 0x00, 0x00, 0x90, 0x90};

// The four LEAs, with the register each one targets.
struct ArrayRef {
   uintptr_t va;
   uint8_t   lea[7];  // expected bytes
   uint8_t   movOp;   // B8 = MOV EAX, BA = MOV EDX
};

constexpr ArrayRef kArrayRefs[] = {
   {0x008866B8, {0x8D, 0x84, 0x24, 0xAC, 0x08, 0x00, 0x00}, 0xB8}, // ctor
   {0x008866F0, {0x8D, 0x84, 0x24, 0xA4, 0x08, 0x00, 0x00}, 0xB8}, // probe argument
   {0x00886738, {0x8D, 0x94, 0x24, 0xA0, 0x08, 0x00, 0x00}, 0xBA}, // post-probe consume
   {0x0088678B, {0x8D, 0x84, 0x24, 0xA8, 0x08, 0x00, 0x00}, 0xB8}, // dtor
};

constexpr int kArrayRefCount = (int)(sizeof(kArrayRefs) / sizeof(kArrayRefs[0]));

// ---------------------------------------------------------------------------
// Saved originals
// ---------------------------------------------------------------------------
struct Saved {
   uint8_t* addr;
   uint32_t value;
   uint32_t width;
};

Saved  s_saved[24] = {};
int    s_savedCount = 0;
uint8_t s_savedLea[kArrayRefCount][7] = {};
uint8_t* s_leaAddr[kArrayRefCount] = {};
uint8_t  s_savedSwPin[7] = {};
uint8_t* s_swPinAddr = nullptr;

uint8_t  s_savedMixerLea[kMixerArrayCount][6] = {};
uint8_t* s_mixerLeaAddr[kMixerArrayCount] = {};
uint8_t* s_mixerCountAddr = nullptr;
uint8_t* s_mixerGateAddr = nullptr;
uint8_t  s_savedMixerCount = 0;
uint8_t  s_savedMixerGate = 0;
void*    s_mixerBuffers[kMixerArrayCount] = {};
bool     s_softwareRaised = false;

void*  s_pool       = nullptr;
void*  s_probeArray = nullptr;
bool   s_installed  = false;

uint32_t read_at(const uint8_t* p, uint32_t width)
{
   uint32_t v = 0;
   memcpy(&v, p, width);
   return v;
}

// Verify-then-record.  Returns false the moment anything does not look like the
// instruction we think it is.
bool expect(uintptr_t exe_base, uintptr_t va, uint32_t width, uint32_t expected)
{
   uint8_t* const p = reinterpret_cast<uint8_t*>(resolve(exe_base, va));
   if (read_at(p, width) != expected) {
      get_gamelog()("[VoiceLimit] site %08X reads %08X, expected %08X -- feature off\n",
                    (unsigned)va, read_at(p, width), expected);
      return false;
   }
   s_saved[s_savedCount].addr  = p;
   s_saved[s_savedCount].value = expected;
   s_saved[s_savedCount].width = width;
   ++s_savedCount;
   return true;
}

void write_at(int savedIndex, uint32_t value)
{
   memcpy(s_saved[savedIndex].addr, &value, s_saved[savedIndex].width);
}

} // namespace

void voice_limit_install(uintptr_t exe_base)
{
   if (g_voiceLimit == 0) return;
   if (g_build != GameBuild::Modtools) return;

   int n = g_voiceLimit;
   if (n < kMinVoices) n = kMinVoices;
   if (n > kMaxVoices) n = kMaxVoices;

   const uint32_t poolBytes  = (uint32_t)n * kVoiceStride;
   const uint32_t probeCount = (uint32_t)n + kProbeSpare;

   // --- verify every site first; write nothing until all of them agree -------
   s_savedCount = 0;
   const bool ok =
      expect(exe_base, kGMaxVoices,      4, 0x20)       &&  // 0
      expect(exe_base, kClampCmpImm8,    1, 0x20)       &&  // 1
      expect(exe_base, kClampStoreImm32, 4, 0x20)       &&  // 2
      expect(exe_base, kPoolPtrImm32,    4, 0x00EDFE18) &&  // 3
      expect(exe_base, kOpenBound,       4, 0xA800)     &&  // 4
      expect(exe_base, kUpdateBound,     4, 0xA800)     &&  // 5
      expect(exe_base, kCloseBound,      4, 0xA800)     &&  // 6
      expect(exe_base, kCentrePeakBound, 4, 0xA800)     &&  // 7
      expect(exe_base, kHwCeilCmpImm8,   1, 0x20)       &&  // 8
      expect(exe_base, kHwCeilLoadImm32, 4, 0x20)       &&  // 9
      expect(exe_base, kSwCeilCmpImm8,   1, 0x20)       &&  // 10
      expect(exe_base, kSwCeilLoadImm32, 4, 0x20)       &&  // 11
      expect(exe_base, kProbeCtorCount,  1, 0x28)       &&  // 12
      expect(exe_base, kProbeDtorCount,  1, 0x28);          // 13

   if (!ok) { s_savedCount = 0; return; }

   for (int i = 0; i < kArrayRefCount; ++i) {
      uint8_t* const p = reinterpret_cast<uint8_t*>(resolve(exe_base, kArrayRefs[i].va));
      if (memcmp(p, kArrayRefs[i].lea, 7) != 0) {
         get_gamelog()("[VoiceLimit] probe array reference %08X is not the expected LEA"
                       " -- feature off\n", (unsigned)kArrayRefs[i].va);
         s_savedCount = 0;
         return;
      }
      s_leaAddr[i] = p;
   }

   {
      uint8_t* const p = reinterpret_cast<uint8_t*>(resolve(exe_base, kSwVoiceCountPin));
      if (memcmp(p, kSwPinExpect, 7) != 0) {
         get_gamelog()("[VoiceLimit] software voice-count load at %08X is not the expected"
                       " MOV -- feature off\n", (unsigned)kSwVoiceCountPin);
         s_savedCount = 0;
         return;
      }
      s_swPinAddr = p;
   }

   // --- allocate ------------------------------------------------------------
   // The pool cannot grow in place (smStreamStorage begins exactly 0xA800 later)
   // and neither buffer may ever move, so both are process-lifetime commits.
   s_pool = VirtualAlloc(nullptr, poolBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
   s_probeArray = VirtualAlloc(nullptr, probeCount * kDSBufferSize,
                               MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
   if (!s_pool || !s_probeArray) {
      if (s_pool) VirtualFree(s_pool, 0, MEM_RELEASE);
      if (s_probeArray) VirtualFree(s_probeArray, 0, MEM_RELEASE);
      s_pool = s_probeArray = nullptr;
      s_savedCount = 0;
      get_gamelog()("[VoiceLimit] could not reserve %u bytes -- feature off\n",
                    poolBytes + probeCount * kDSBufferSize);
      return;
   }

   // --- apply ---------------------------------------------------------------
   write_at(0,  (uint32_t)n);            // gMaxVoices: the probe limit follows it
   write_at(1,  (uint32_t)n);            // and a -voices argument cannot re-clamp
   write_at(2,  (uint32_t)n);            //   us back down to 32
   write_at(3,  (uint32_t)(uintptr_t)s_pool);
   write_at(4,  poolBytes);              // Open
   write_at(5,  poolBytes);              // Update -- miss this and the extra
   write_at(6,  poolBytes);              //   voices never tick
   write_at(7,  poolBytes);              // SetCentrePeakMode
   write_at(8,  (uint32_t)n);
   write_at(9,  (uint32_t)n);
   write_at(10, (uint32_t)n);
   write_at(11, (uint32_t)n);
   write_at(12, probeCount);
   write_at(13, probeCount);

   for (int i = 0; i < kArrayRefCount; ++i) {
      memcpy(s_savedLea[i], s_leaAddr[i], 7);

      uint8_t patch[7];
      patch[0] = kArrayRefs[i].movOp;                    // MOV EAX/EDX, imm32
      const uint32_t addr = (uint32_t)(uintptr_t)s_probeArray;
      memcpy(patch + 1, &addr, 4);
      patch[5] = 0x90;                                   // the LEA was 7 bytes and
      patch[6] = 0x90;                                   //   the MOV is 5
      memcpy(s_leaAddr[i], patch, 7);
   }

   // --- the software mixer -------------------------------------------------
   // Rounded UP to a multiple of 8 for the bitmap clear; a mixer wider than the
   // voice pool is harmless, the spare inputs are simply never connected.
   const uint32_t mixerInputs = (uint32_t)((n + 7) / 8) * 8;

   // SOFTWARE MIXER HAZARD -- why this is off by default.
   //
   // Relocating the four arrays and raising the count is correct in itself, but
   // it exposes an unguarded path in the engine. Snd::SoftOutput::ConnectInput
   // (0x0089FCE0) is a two-instruction thunk with no bounds check, and one of its
   // callers reconnects a voice on a mix-config change without testing the index:
   //
   //   0089E44D  MOV EDX,[ESI+0x484]   ; mSoftConnIdx, -1 if it never got one
   //   0089E453  PUSH EDX              ; pushed unchecked
   //   0089E45A  CALL 0x0089FCE0       ; -> 00898F5A LEA EAX,[EAX+ECX*8]
   //                                   ;    00898F61 MOV [EAX],EDX
   //
   // At index -1 that writes eight bytes below the table. Stock never reaches it
   // because 32 voices fit 32 inputs exactly, so GetUnconnectedInput cannot fail.
   // Once the pool is larger than the inputs actually available at that moment it
   // can and does: observed as a WRITE access violation at 0x00898F61 to
   // (table - 8) on the software-to-hardware mix-config switch at the main menu.
   //
   // Guarding it means detouring the reconnect path to drop -1, which is a
   // separate piece of work. Until then the software branch stays pinned at 32,
   // which is stock behaviour and safe.
   bool mixerOk = g_softwareVoices && (mixerInputs <= 0x7F);
   if (mixerOk) {
      uint8_t* const cnt = reinterpret_cast<uint8_t*>(resolve(exe_base, kMixerCountImm8));
      uint8_t* const gate = reinterpret_cast<uint8_t*>(resolve(exe_base, kMixerGateImm8));
      if (*cnt != 0x20 || *gate != 0x20) mixerOk = false;

      for (int i = 0; mixerOk && i < kMixerArrayCount; ++i) {
         uint8_t* const q = reinterpret_cast<uint8_t*>(resolve(exe_base, kMixerArrays[i].va));
         if (memcmp(q, kMixerArrays[i].lea, 6) != 0) mixerOk = false;
         else s_mixerLeaAddr[i] = q;
      }

      for (int i = 0; mixerOk && i < kMixerArrayCount; ++i) {
         const uint32_t bytes = mixerInputs * kMixerArrays[i].perEntry * kMixerArrays[i].multiplier;
         s_mixerBuffers[i] = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
         if (!s_mixerBuffers[i]) mixerOk = false;
      }

      if (mixerOk) {
         s_mixerCountAddr = cnt;
         s_mixerGateAddr = gate;
         s_savedMixerCount = *cnt;
         s_savedMixerGate = *gate;
         *cnt = (uint8_t)mixerInputs;
         *gate = (uint8_t)mixerInputs;

         for (int i = 0; i < kMixerArrayCount; ++i) {
            memcpy(s_savedMixerLea[i], s_mixerLeaAddr[i], 6);
            uint8_t patch[6];
            patch[0] = kMixerArrays[i].movOp;
            const uint32_t addr = (uint32_t)(uintptr_t)s_mixerBuffers[i];
            memcpy(patch + 1, &addr, 4);
            patch[5] = 0x90;              // the LEA was 6 bytes, the MOV is 5
            memcpy(s_mixerLeaAddr[i], patch, 6);
         }
         s_softwareRaised = true;
      }
   }

   if (!s_softwareRaised) {
      // Could not widen the mixer, so software mixing must stay at 32 -- past
      // that a voice would take a pool slot and produce nothing, which is worse
      // than stock. Pin the software branch back to 32 and keep the EAX raise.
      for (int i = 0; i < kMixerArrayCount; ++i) {
         if (s_mixerBuffers[i]) VirtualFree(s_mixerBuffers[i], 0, MEM_RELEASE);
         s_mixerBuffers[i] = nullptr;
         s_mixerLeaAddr[i] = nullptr;
      }
      memcpy(s_savedSwPin, s_swPinAddr, 7);
      memcpy(s_swPinAddr, kSwPinPatch, 7);
   } else {
      s_swPinAddr = nullptr; // nothing pinned, nothing to restore
   }

   s_installed = true;

   get_gamelog()("[VoiceLimit] %d voices, software mixing %s (pool %u bytes at %p,"
                 " probe array %u entries at %p)\n",
                 n, s_softwareRaised ? "raised to match" : "pinned at 32",
                 poolBytes, s_pool, probeCount, s_probeArray);
}

void voice_limit_uninstall()
{
   if (!s_installed) return;

   // Sections are re-protected by now, so these cannot be plain stores.
   for (int i = 0; i < s_savedCount; ++i)
      protected_write(s_saved[i].addr, &s_saved[i].value, s_saved[i].width);

   for (int i = 0; i < kArrayRefCount; ++i)
      if (s_leaAddr[i]) protected_write(s_leaAddr[i], s_savedLea[i], 7);

   if (s_swPinAddr) protected_write(s_swPinAddr, s_savedSwPin, 7);
   s_swPinAddr = nullptr;

   if (s_mixerCountAddr) protected_write(s_mixerCountAddr, &s_savedMixerCount, 1);
   if (s_mixerGateAddr) protected_write(s_mixerGateAddr, &s_savedMixerGate, 1);
   s_mixerCountAddr = s_mixerGateAddr = nullptr;

   for (int i = 0; i < kMixerArrayCount; ++i) {
      if (s_mixerLeaAddr[i]) protected_write(s_mixerLeaAddr[i], s_savedMixerLea[i], 6);
      if (s_mixerBuffers[i]) VirtualFree(s_mixerBuffers[i], 0, MEM_RELEASE);
      s_mixerLeaAddr[i] = nullptr;
      s_mixerBuffers[i] = nullptr;
   }
   s_softwareRaised = false;

   // The engine is closed by now, but the pool is only referenced through the
   // immediate we just restored, so nothing can be pointing into it either way.
   if (s_pool) VirtualFree(s_pool, 0, MEM_RELEASE);
   if (s_probeArray) VirtualFree(s_probeArray, 0, MEM_RELEASE);
   s_pool = s_probeArray = nullptr;

   s_savedCount = 0;
   s_installed = false;
}
