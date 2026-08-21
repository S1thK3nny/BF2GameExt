#include "pch.h"
#include "voice_limit.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// Install-time logging MUST NOT go through get_gamelog(). Every section of the
// exe is PAGE_READWRITE for the whole installer sequence (dllmain.cpp:194), so
// calling the engine's logger jumps into non-executable .text and raises an EXEC
// access violation -- which surfaces as DLL_INIT_FAILED, i.e. the game refuses
// to start at all. The CRT is fine; it lives in this module.
static void install_log(const char* fmt, ...)
{
   FILE* f = nullptr;
   if (fopen_s(&f, "BF2GameExt.log", "a") != 0 || !f) return;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fclose(f);
}


// See voice_limit.hpp for the mechanism and why the probe array has to move.

int g_voiceLimit = 0;

namespace {

constexpr int kMinVoices = 33;  // below this there is nothing to gain
constexpr int kMaxVoices = 119; // PUSH imm8 of (N + 8) must stay under 128

constexpr uint32_t kVoiceStride  = 0x540; // sizeof(Snd::Voice)
constexpr uint32_t kDSBufferSize = 0x40;  // one probe element
constexpr uint32_t kProbeSpare   = 8;     // the probe asks for gMaxVoices + 8

// ---------------------------------------------------------------------------
// Sites, per build.  Every one is verified against its expected bytes before
// anything is written, and a single mismatch abandons the whole feature -- a
// half-applied voice raise would have the engine walking a pool that is not the
// size it thinks it is.
//
// Retail was located structurally, never by pattern: Snd::Engine::Open was
// identified by its nine-parameter signature and its eight reads of smVoices,
// the four pool bounds by the 0xA800 immediate (32 * sizeof(Voice)), and
// gMaxVoices by being parameter 7 at the call site AND by carrying the same
// sscanf/clamp-to-[8,32] shape.  Three places where retail is NOT modtools:
//
//   * The upper clamp is folded into a CMOVG -- `CMP EAX,0x20 / MOV ECX,0x20 /
//     CMOVG EAX,ECX` -- where modtools has `CMP / JLE / MOV [g],0x20`.  Same two
//     operands to raise, completely different bytes.
//   * The probe array is addressed EBP-relative, so all four references share
//     ONE displacement instead of modtools' four different ESP-relative ones,
//     and each LEA is 6 bytes rather than 7.
//   * The software voice count is `MOV EAX,[EBP+0x20]`, only 3 bytes, so it
//     cannot take a 5-byte MOV imm32; it is pinned with `PUSH 0x20 / POP EAX`
//     instead, which is exactly 3 and stack-neutral.
//
// Steam and GOG share the command-line clamp addresses exactly but NOT the
// gMaxVoices global (0x007E68E8 vs 0x007E78E4), and their call sites push the
// two neighbouring globals in a different order -- so GOG is not Steam plus a
// fixed delta, and each was read from its own image.
// ---------------------------------------------------------------------------
struct ArrayRef {
   uintptr_t va;
   uint8_t   movOp;   // B8 = MOV EAX, BA = MOV EDX
};

struct BuildSites {
   uintptr_t gMaxVoices;
   uintptr_t clampCmpImm8;
   uintptr_t clampValImm32;   // modtools: the stored imm32; retail: CMOVG's source

   uintptr_t poolPtrImm32;
   uint32_t  poolPtrExpect;   // the stock pool address that immediate holds

   uintptr_t openBound, updateBound, closeBound, centrePeakBound;
   uintptr_t hwCeilCmpImm8, hwCeilLoadImm32, swCeilCmpImm8, swCeilLoadImm32;
   uintptr_t probeCtorCount, probeDtorCount;

   ArrayRef  arrayRef[4];
   uint8_t   leaLen;          // 7 on modtools (ESP-relative), 6 on retail (EBP)
   uint8_t   lea[4][8];       // expected bytes, leaLen of them

   uintptr_t swPin;
   uint8_t   swPinLen;        // 7 on modtools, 3 on retail
   uint8_t   swPinExpect[8];
   uint8_t   swPinPatch[8];
};

constexpr BuildSites kModtools = {
   0x00ADD474, 0x00446A4F, 0x00446A5C,
   0x00882C49, 0x00EDFE18,
   0x00882CF7, 0x00882825, 0x00882B5A, 0x008851A0,
   0x00886B58, 0x00886B62, 0x00886BDA, 0x00886BE0,
   0x008866B5, 0x00886788,
   {{0x008866B8, 0xB8}, {0x008866F0, 0xB8}, {0x00886738, 0xBA}, {0x0088678B, 0xB8}},
   7,
   {  // four different ESP displacements for ONE array, because ESP moves
      {0x8D, 0x84, 0x24, 0xAC, 0x08, 0x00, 0x00},  // ctor
      {0x8D, 0x84, 0x24, 0xA4, 0x08, 0x00, 0x00},  // probe argument
      {0x8D, 0x94, 0x24, 0xA0, 0x08, 0x00, 0x00},  // post-probe consume
      {0x8D, 0x84, 0x24, 0xA8, 0x08, 0x00, 0x00},  // dtor
   },
   0x00886BB0, 7,
   {0x8B, 0x9C, 0x24, 0xC4, 0x12, 0x00, 0x00},     // MOV EBX,[ESP+0x12C4]
   {0xBB, 0x20, 0x00, 0x00, 0x00, 0x90, 0x90},     // MOV EBX,0x20
};

constexpr BuildSites kSteam = {
   0x007E68E8, 0x00479E47, 0x00479E49,
   0x00734428, 0x009D8420,
   0x007344B6, 0x00734605, 0x00733F39, 0x00732C4D,
   0x00732628, 0x00732632, 0x007326AB, 0x007326AF,
   0x00732146, 0x0073222B,
   {{0x00732149, 0xB8}, {0x00732183, 0xB8}, {0x007321D6, 0xB8}, {0x0073222E, 0xB8}},
   6,
   {  // EBP-relative, so all four are the SAME displacement
      {0x8D, 0x85, 0xB0, 0xED, 0xFF, 0xFF},
      {0x8D, 0x85, 0xB0, 0xED, 0xFF, 0xFF},
      {0x8D, 0x85, 0xB0, 0xED, 0xFF, 0xFF},
      {0x8D, 0x85, 0xB0, 0xED, 0xFF, 0xFF},
   },
   0x00732681, 3,
   {0x8B, 0x45, 0x20},                             // MOV EAX,[EBP+0x20]
   {0x6A, 0x20, 0x58},                             // PUSH 0x20 / POP EAX
};

constexpr BuildSites kGOG = {
   0x007E78E4, 0x00479E47, 0x00479E49,
   0x00735518, 0x009D98C0,
   0x007355A6, 0x007356F5, 0x00735029, 0x00733D3D,
   0x00733718, 0x00733722, 0x0073379B, 0x0073379F,
   0x00733236, 0x0073331B,
   {{0x00733239, 0xB8}, {0x00733273, 0xB8}, {0x007332C6, 0xB8}, {0x0073331E, 0xB8}},
   6,
   {
      {0x8D, 0x85, 0xB0, 0xED, 0xFF, 0xFF},
      {0x8D, 0x85, 0xB0, 0xED, 0xFF, 0xFF},
      {0x8D, 0x85, 0xB0, 0xED, 0xFF, 0xFF},
      {0x8D, 0x85, 0xB0, 0xED, 0xFF, 0xFF},
   },
   0x00733771, 3,
   {0x8B, 0x45, 0x20},
   {0x6A, 0x20, 0x58},
};

const BuildSites* s_sites = nullptr;

constexpr int kArrayRefCount = 4;

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
uint8_t s_savedLea[kArrayRefCount][8] = {};
uint8_t* s_leaAddr[kArrayRefCount] = {};
uint8_t  s_savedSwPin[8] = {};
uint8_t* s_swPinAddr = nullptr;


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
      install_log("[VoiceLimit] site %08X reads %08X, expected %08X -- feature off\n",
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

   switch (g_build) {
   case GameBuild::Modtools: s_sites = &kModtools; break;
   case GameBuild::Steam:    s_sites = &kSteam;    break;
   case GameBuild::GOG:      s_sites = &kGOG;      break;
   default:
      install_log("[VoiceLimit] unknown build -- feature off\n");
      return;
   }
   const BuildSites& S = *s_sites;

   int n = g_voiceLimit;
   if (n < kMinVoices) n = kMinVoices;
   if (n > kMaxVoices) n = kMaxVoices;

   const uint32_t poolBytes  = (uint32_t)n * kVoiceStride;
   const uint32_t probeCount = (uint32_t)n + kProbeSpare;

   // --- verify every site first; write nothing until all of them agree -------
   s_savedCount = 0;
   const bool ok =
      expect(exe_base, S.gMaxVoices,      4, 0x20)            &&  // 0
      expect(exe_base, S.clampCmpImm8,    1, 0x20)            &&  // 1
      expect(exe_base, S.clampValImm32,   4, 0x20)            &&  // 2
      // RELOCATION.  poolPtrExpect is an ADDRESS, and the only expectation here
      // that is one -- every other site holds a plain integer (0x20, 0xA800,
      // 0x28) that the loader never touches.  The imm32 carries a .reloc entry,
      // so on a rebased image it reads base-adjusted, not the link-time value.
      // Steam loads at 0x000E0000 in practice, which made this compare
      // 0x006B8420 against 0x009D8420 and correctly refuse the whole feature.
      expect(exe_base, S.poolPtrImm32, 4,
             (uint32_t)(uintptr_t)resolve(exe_base, S.poolPtrExpect)) &&  // 3
      expect(exe_base, S.openBound,       4, 0xA800)          &&  // 4
      expect(exe_base, S.updateBound,     4, 0xA800)          &&  // 5
      expect(exe_base, S.closeBound,      4, 0xA800)          &&  // 6
      expect(exe_base, S.centrePeakBound, 4, 0xA800)          &&  // 7
      expect(exe_base, S.hwCeilCmpImm8,   1, 0x20)            &&  // 8
      expect(exe_base, S.hwCeilLoadImm32, 4, 0x20)            &&  // 9
      expect(exe_base, S.swCeilCmpImm8,   1, 0x20)            &&  // 10
      expect(exe_base, S.swCeilLoadImm32, 4, 0x20)            &&  // 11
      expect(exe_base, S.probeCtorCount,  1, 0x28)            &&  // 12
      expect(exe_base, S.probeDtorCount,  1, 0x28);               // 13

   if (!ok) { s_savedCount = 0; return; }

   for (int i = 0; i < kArrayRefCount; ++i) {
      uint8_t* const p = reinterpret_cast<uint8_t*>(resolve(exe_base, S.arrayRef[i].va));
      if (memcmp(p, S.lea[i], S.leaLen) != 0) {
         install_log("[VoiceLimit] probe array reference %08X is not the expected LEA"
                       " -- feature off\n", (unsigned)S.arrayRef[i].va);
         s_savedCount = 0;
         return;
      }
      s_leaAddr[i] = p;
   }

   {
      uint8_t* const p = reinterpret_cast<uint8_t*>(resolve(exe_base, S.swPin));
      if (memcmp(p, S.swPinExpect, S.swPinLen) != 0) {
         install_log("[VoiceLimit] software voice-count load at %08X is not the expected"
                       " MOV -- feature off\n", (unsigned)S.swPin);
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
      install_log("[VoiceLimit] could not reserve %u bytes -- feature off\n",
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
      memcpy(s_savedLea[i], s_leaAddr[i], S.leaLen);

      // MOV r32,imm32 is 5 bytes; the LEA it replaces is 7 (modtools, ESP) or
      // 6 (retail, EBP), so the remainder is padded with NOPs either way.
      uint8_t patch[8];
      memset(patch, 0x90, sizeof(patch));
      patch[0] = S.arrayRef[i].movOp;                    // MOV EAX/EDX, imm32
      const uint32_t addr = (uint32_t)(uintptr_t)s_probeArray;
      memcpy(patch + 1, &addr, 4);
      memcpy(s_leaAddr[i], patch, S.leaLen);
   }

   // Software mixing stays at the stock 32. SoftOutput IS the mixer there and
   // ships with exactly 32 inputs, so a voice past that takes a pool slot and
   // produces nothing. Widening that mixer was implemented and reverted: the
   // engine feeds the -1 that GetUnconnectedInput returns on failure to several
   // unguarded consumers -- ConnectInput (0x0089FCE0) and the gain matrix
   // (0x00897910) among them -- and in play EVERY voice failed to obtain an input
   // once the table had filled once, 119 of 119. Guarding one consumer only moved
   // the crash to the next. See docs/RE/SoundSystem.md.
   memcpy(s_savedSwPin, s_swPinAddr, S.swPinLen);
   memcpy(s_swPinAddr, S.swPinPatch, S.swPinLen);

   s_installed = true;

   install_log("[VoiceLimit] %d voices under EAX, 32 in software mixing"
                 " (pool %u bytes at %p, probe array %u entries at %p)\n",
                 n, poolBytes, s_pool, probeCount, s_probeArray);
}

void voice_limit_uninstall()
{
   if (!s_installed) return;

   // Sections are re-protected by now, so these cannot be plain stores.
   for (int i = 0; i < s_savedCount; ++i)
      protected_write(s_saved[i].addr, &s_saved[i].value, s_saved[i].width);

   for (int i = 0; i < kArrayRefCount; ++i)
      if (s_leaAddr[i]) protected_write(s_leaAddr[i], s_savedLea[i], s_sites->leaLen);

   if (s_swPinAddr) protected_write(s_swPinAddr, s_savedSwPin, s_sites->swPinLen);
   s_swPinAddr = nullptr;

   // The engine is closed by now, but the pool is only referenced through the
   // immediate we just restored, so nothing can be pointing into it either way.
   if (s_pool) VirtualFree(s_pool, 0, MEM_RELEASE);
   if (s_probeArray) VirtualFree(s_probeArray, 0, MEM_RELEASE);
   s_pool = s_probeArray = nullptr;

   s_savedCount = 0;
   s_installed = false;
}
