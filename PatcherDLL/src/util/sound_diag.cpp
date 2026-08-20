#include "pch.h"
#include "sound_diag.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <detours.h>
#include <stdio.h>
#include <stdarg.h>

// See sound_diag.hpp for what this measures and why it has to be measured.

bool g_soundDiagEnabled = false;

namespace {

// ---------------------------------------------------------------------------
// modtools addresses
// ---------------------------------------------------------------------------
// Globals read out of Snd::Engine::Open:
//   00886B14  MOV EAX,[0x02339F84]   ; tSoundOpt::mMixerMode
//   00886B1B  CMP EAX,0x2            ; 2 == DirectSoundHardware, the EAX path
//   00886B24  MOV EDI,[0x02339F48]   ; dwFreeHw3DStreamingBuffers
//   00886B2A  ADD EDI,-0x8           ; the reserve; usable voices = value - 8
//   00886B56  CMP EDI,0x20           ; and then capped at 32 either way
constexpr uintptr_t kMixConfig       = 0x02339F84;
constexpr uintptr_t kHw3dFreeBuffers = 0x02339F48;

// Snd::EngineBase::smVoiceVirtualManager.  VoiceVirtualManager::Update walks its
// active list from +0xC (0089DEB6 MOV ESI,[EBP+0xC] and 0089DEBC LEA EDI,
// [EBP+0xC], so +0xC is both the head pointer and the terminator), and the
// intrusive node sits at object + 4 (0089DEC9 LEA EAX,[ESI-0x4]).
// mNumManagedVoices is +0x1C -- cross-checked against Open, which writes the
// manager address into every Voice (00882CE1 MOV [ESI+0x78],0x02331170) and
// bumps 0x0233118C once per construction.
// The active list is the manager's own embedded node: VoiceVirtualManager::
// Update counts it with `for (p = *mgr; p != mgr && p - 1 != mgr; p = *p)` and
// derives each object as `p - 0x25` in float** units, i.e. node - 0x94.  The
// grant/evict pass then reads mVoice as `object[0x28]` == object + 0xA0.
//
// The field offset was right all along; the NODE offset was not.  Walking from
// manager+0xC with node == object+4 read mVoice from 0x90 bytes off the end of
// each object, which is why the first readings claimed 93 bound voices out of a
// pool of 32 and then, once range-checked, zero.
constexpr uintptr_t kVoiceManager  = 0x02331170;
constexpr uint32_t  kMgrListHead   = 0x00; // the manager IS the terminator node
constexpr uint32_t  kMgrNumManaged = 0x1C;
constexpr uint32_t  kVVNodeOffset  = 0x94; // object = node - 0x94
constexpr uint32_t  kVVVoice       = 0xA0; // VoiceVirtual::mVoice, null = starved

// Snd::EngineBase::smVoices -- a POINTER to the pool (00882C50 MOV EAX,
// [0x02331088] inside Open's construct loop), which is why a voice-count raise
// can relocate the pool without moving anything else.
constexpr uintptr_t kVoicesPtr    = 0x02331088;
constexpr uint32_t  kVoiceStride  = 0x540;
// Sized for the largest pool [LimitIncreases] VoiceLimit will build (119),
// with headroom.  This was 64, and when the pool went to 119 the guard below
// quietly fell back to 32 -- so every voice from slot 32 up was reported as an
// unrecognised pointer and `pool slots used` pinned at 32.  The engine was
// fine; the counter was not.
constexpr int       kMaxPoolSlots = 128;

// The renderer is embedded at Voice + 0xE8 (0089E326 LEA ECX,[ESI+0xE8] in
// Voice::Initialize), so a renderer `this` can be turned back into a voice index.
constexpr uint32_t kVoiceToRenderer = 0xE8;

constexpr uintptr_t kSndUpdate  = 0x008827B0; // Snd::EngineBase::Update
constexpr uintptr_t kUpdateGain = 0x008997C0; // Snd::DSBufferRenderer::UpdateGain
constexpr uintptr_t kWriteData  = 0x0089A120; // Snd::DSBufferRenderer::WriteData

constexpr uint32_t kRendererGain     = 0x2C;  // the float UpdateGain multiplies by
constexpr uint32_t kRendererFlags    = 0x18C; // bit 0x10 picks float gain over Q15
constexpr uint32_t kRendererPktRead  = 0x168; // WriteData's cursor into the packet
constexpr uint32_t kRendererGainTgt  = 0x190; // Q15 target, <<16
constexpr uint32_t kRendererGainCur  = 0x198; // Q15 current, smoothed toward target

// ---------------------------------------------------------------------------
// Symptom detection
// ---------------------------------------------------------------------------
// The counters above only test two theories.  If both are wrong a clean log says
// nothing about what the artefact actually is, so this also watches the output
// itself, theory-free: WriteData's mixing loop saturates every sample to
// +/-0x7FFF (0089A21E CMP EAX,0x7FFF ...  0089A233 MOV EAX,0xFFFF8000), and a
// burst of samples pinned at the rail is what "sudden and much louder than
// everything else" looks like in the PCM.
//
// Occasional saturation is normal on loud content, so only a sustained run
// counts.  The scan is capped at a small window because `dest` is a locked
// DirectSound region and reading write-combined memory back is slow.
constexpr uint32_t kSatLevel     = 0x7F00; // "at the rail" allowing for rounding
constexpr uint32_t kSatScanBytes = 2048;   // at most 1024 samples looked at
constexpr uint32_t kSatStride    = 2;      // ... of which every other one
constexpr uint32_t kSatBurst     = 96;     // hits within the window to call it a burst

// ---------------------------------------------------------------------------
// Event ring
// ---------------------------------------------------------------------------
// The audio hooks never touch the log: an fopen on the refill thread would be
// audible in its own right.  They queue into a fixed ring and the engine tick
// drains it, so every event still lands with a wall-clock time next to it.
enum EventKind { EV_SATURATION = 1, EV_GAIN_OVER = 2, EV_CURSOR = 3 };

struct DiagEvent {
   uint32_t kind;
   uint32_t voice;
   uint32_t a;
   uint32_t b;
   float    f;
};

constexpr LONG kEventRing = 64;
DiagEvent    s_events[kEventRing] = {};
volatile LONG s_evWrite = 0;
LONG          s_evRead  = 0;

void queue_event(uint32_t kind, uint32_t voice, uint32_t a, uint32_t b, float f)
{
   const LONG idx = InterlockedIncrement(&s_evWrite) - 1;
   DiagEvent& e = s_events[idx % kEventRing];
   e.kind  = kind;
   e.voice = voice;
   e.a     = a;
   e.b     = b;
   e.f     = f;
}

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------
// The call totals matter as much as the event counts: without them a zero
// reading cannot be told apart from a hook that never fired.
volatile LONG s_gainCalls     = 0; // UpdateGain entries, any path
volatile LONG s_gainQ15Calls  = 0; // ... that took the unclamped fixed-point path
volatile LONG s_gainOverUnity = 0; // ... whose gain product exceeded 1.0
volatile LONG s_gainOverBadly = 0; // ... and exceeded 2.0
volatile LONG s_gainMaxMilli  = 0; // largest product seen at all, x1000

volatile LONG s_writeCalls    = 0; // WriteData entries
volatile LONG s_maxPacketRead = 0; // largest mPacketRead; a runaway grows unbounded
volatile LONG s_satBursts     = 0; // WriteData calls whose window was mostly rail
volatile LONG s_satSamples    = 0; // total saturated samples seen in the windows
volatile LONG s_peakAbsSample = 0; // loudest sample written all session

int s_peakWanting  = 0; // most VoiceVirtuals in the active list at once
int s_peakBound    = 0; // ... holding any non-null mVoice at all
int s_peakSounding = 0; // ... holding a pointer that really is a pool Voice
int s_peakStale    = 0; // ... holding one that is not: a stale mVoice
int s_peakSlots    = 0; // distinct pool slots claimed at once -- the true voice count
int s_peakStarved  = 0; // worst (wanting - sounding) gap
int s_starvedTicks = 0; // engine ticks where at least one was starved

int  s_ticks       = 0;
bool s_headerDone  = false;
bool s_dumpedVoice = false;

void now_string(char* out, size_t n)
{
   SYSTEMTIME st;
   GetLocalTime(&st);
   _snprintf_s(out, n, _TRUNCATE, "%02u:%02u:%02u.%03u",
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

void diag_log(const char* fmt, ...)
{
   FILE* f = nullptr;
   if (fopen_s(&f, "BF2GameExt.log", "a") != 0 || !f) return;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fclose(f);
}

using fn_snd_update_t  = void(__cdecl*)(float dt, char full);
using fn_update_gain_t = void(__fastcall*)(void* self, void* edx, uint32_t a0, float gain);
using fn_write_data_t  = uint32_t(__fastcall*)(void* self, void* edx, void* dest,
                                               uint32_t bytes, uint32_t mode);

fn_snd_update_t  g_origSndUpdate  = nullptr;
fn_update_gain_t g_origUpdateGain = nullptr;
fn_write_data_t  g_origWriteData  = nullptr;

uint32_t* g_mixConfig       = nullptr;
uint32_t* g_hw3dFreeBuffers = nullptr;
uint8_t*  g_voiceManager    = nullptr;
uint8_t** g_voicesPtr       = nullptr;

void bump_max(volatile LONG* slot, LONG value)
{
   LONG seen = *slot;
   while (value > seen) {
      const LONG prev = InterlockedCompareExchange(slot, value, seen);
      if (prev == seen) break;
      seen = prev;
   }
}

// Turn a renderer `this` back into a pool slot index, or 0xFFFFFFFF if it is not
// one of the pool voices.
uint32_t voice_index_of(void* renderer)
{
   if (!g_voicesPtr || !*g_voicesPtr) return 0xFFFFFFFFu;
   const uint8_t* const pool = *g_voicesPtr;
   const uint8_t* const voice = static_cast<uint8_t*>(renderer) - kVoiceToRenderer;
   if (voice < pool) return 0xFFFFFFFFu;
   const uintptr_t offset = static_cast<uintptr_t>(voice - pool);
   if (offset % kVoiceStride != 0) return 0xFFFFFFFFu;
   const uint32_t index = static_cast<uint32_t>(offset / kVoiceStride);
   return (index < kMaxPoolSlots) ? index : 0xFFFFFFFFu;
}

// Both audio-path hooks take __fastcall's ECX/EDX so a __thiscall with stack
// arguments can be intercepted without a naked shim: ECX carries `this`, the
// declared parameters land on the stack, and the callee cleans them -- which is
// what the originals' RET 0x8 / RET 0xC already do.

void __fastcall hooked_update_gain(void* self, void* edx, uint32_t a0, float gain)
{
   InterlockedIncrement(&s_gainCalls);

   const uint8_t flags = *(static_cast<uint8_t*>(self) + kRendererFlags);
   if ((flags & 0x10) == 0) InterlockedIncrement(&s_gainQ15Calls);

   const float mGain = *reinterpret_cast<const float*>(static_cast<char*>(self) + kRendererGain);

   // Of UpdateGain's four convert sites, two multiply the argument by mGain and
   // one uses mGain alone, so the larger of the two is what can reach the Q15
   // conversion on this call.
   float product = gain * mGain;
   if (mGain > product) product = mGain;

   if (product > 0.0f) bump_max(&s_gainMaxMilli, static_cast<LONG>(product * 1000.0f));
   if (product > 1.0f) {
      const LONG n = InterlockedIncrement(&s_gainOverUnity);
      if (product > 2.0f) InterlockedIncrement(&s_gainOverBadly);
      if (n <= 16) queue_event(EV_GAIN_OVER, voice_index_of(self), 0, 0, product);
   }

   g_origUpdateGain(self, edx, a0, gain);
}

uint32_t __fastcall hooked_write_data(void* self, void* edx, void* dest,
                                      uint32_t bytes, uint32_t mode)
{
   InterlockedIncrement(&s_writeCalls);

   const uint32_t result = g_origWriteData(self, edx, dest, bytes, mode);

   // WriteData computes `remaining = packet->mBufferUsed - mPacketRead` unsigned
   // and unguarded (0089A17D SUB EDI,EAX), and only releases the packet on exact
   // equality (0089A7A7 CMP EAX,[ESP+0x24] / JNZ).  If mPacketRead ever gets past
   // mBufferUsed the subtraction wraps, the clamp picks the whole requested
   // region, and the cursor never comes back.  A packet holds at most
   // SNDSTREAM_PACKETSIZE, so a cursor far above that is the runaway itself.
   const uint32_t read = *reinterpret_cast<uint32_t*>(static_cast<char*>(self) + kRendererPktRead);
   if (read < 0x40000000u) {
      bump_max(&s_maxPacketRead, static_cast<LONG>(read));
      if (read > 0x1B000u) queue_event(EV_CURSOR, voice_index_of(self), read, bytes, 0.0f);
   }

   // --- symptom watch: how much of what we just wrote is pinned at the rail ---
   if (dest && result >= 4) {
      uint32_t scan = (result < kSatScanBytes) ? result : kSatScanBytes;
      const int16_t* pcm = static_cast<const int16_t*>(dest);
      const uint32_t samples = scan / 2;

      uint32_t hits = 0, peak = 0;
      for (uint32_t i = 0; i < samples; i += kSatStride) {
         int32_t v = pcm[i];
         if (v < 0) v = -v;
         const uint32_t a = static_cast<uint32_t>(v);
         if (a > peak) peak = a;
         if (a >= kSatLevel) ++hits;
      }

      bump_max(&s_peakAbsSample, static_cast<LONG>(peak));
      if (hits) InterlockedAdd(&s_satSamples, static_cast<LONG>(hits));

      if (hits >= kSatBurst) {
         const LONG n = InterlockedIncrement(&s_satBursts);
         if (n <= 24) {
            const uint32_t cur = *reinterpret_cast<uint32_t*>(
               static_cast<char*>(self) + kRendererGainCur);
            queue_event(EV_SATURATION, voice_index_of(self), hits, cur, 0.0f);
         }
      }
   }

   return result;
}

// Walk the manager's active list.  Every entry is something that wants to be
// heard; an entry whose mVoice is null is being heard by nobody.  mVoice is
// range-checked against the pool because a bare non-null test cannot tell a live
// binding from a pointer left behind by a steal -- and the first reading came
// back claiming 100 sounding voices out of a pool of 32, which it has to be one
// or the other.
void sample_voice_pressure()
{
   if (!g_voiceManager || !g_voicesPtr) return;

   const uint8_t* const pool = *g_voicesPtr;
   const uint32_t managed = *reinterpret_cast<uint32_t*>(g_voiceManager + kMgrNumManaged);

   // Clamp to what the slot map can hold rather than dropping to a stock 32:
   // guessing low here does not fail safe, it silently under-reports the very
   // thing this function exists to measure.
   uint32_t slots = managed;
   if (slots == 0 || slots > (uint32_t)kMaxPoolSlots) slots = (uint32_t)kMaxPoolSlots;

   bool claimed[kMaxPoolSlots] = {};

   uint8_t* const head = g_voiceManager + kMgrListHead;
   uint8_t*       node = *reinterpret_cast<uint8_t**>(head);

   // The validated count came back as zero-with-31-stale, which cannot be right:
   // 31 of 32 voices bound is exactly what a busy match should look like, so the
   // range check is wrong, not the engine.  Dump the raw values once and read the
   // relationship off them instead of reasoning about it again.
   if (!s_dumpedVoice) {
      s_dumpedVoice = true;
      diag_log("[SndDiag] pool base=%p  stride=%u  managed=%u  (smVoices at %p)",
               (const void*)pool, kVoiceStride, managed, (void*)g_voicesPtr);

      uint8_t* probe = *reinterpret_cast<uint8_t**>(head);
      for (int i = 0, shown = 0; i < 256 && shown < 8 && probe
           && probe != head && probe != head + 4; ++i) {
         const uint8_t* const obj = probe - kVVNodeOffset;
         const uint8_t* const v = *reinterpret_cast<const uint8_t* const*>(obj + kVVVoice);
         if (v) {
            const ptrdiff_t d = v - pool;
            diag_log("[SndDiag]   vv=%p  mVoice=%p  delta=%td  delta/stride=%td rem=%td",
                     (const void*)obj, (const void*)v, d, d / (ptrdiff_t)kVoiceStride,
                     d % (ptrdiff_t)kVoiceStride);
            ++shown;
         }
         probe = *reinterpret_cast<uint8_t**>(probe);
      }
   }

   int wanting = 0, sounding = 0, stale = 0;
   // The engine walks this same list every tick; the bound is a safety net
   // against reading a torn pointer, not an expected limit.
   for (int i = 0; i < 4096 && node && node != head && node != head + 4; ++i) {
      const uint8_t* const object = node - kVVNodeOffset;
      ++wanting;

      const uint8_t* const voice = *reinterpret_cast<const uint8_t* const*>(object + kVVVoice);
      if (voice) {
         const uintptr_t offset = static_cast<uintptr_t>(voice - pool);
         const uint32_t index = static_cast<uint32_t>(offset / kVoiceStride);
         if (pool && voice >= pool && index < slots && offset % kVoiceStride == 0) {
            ++sounding;
            claimed[index] = true;
         } else {
            ++stale;
         }
      }

      node = *reinterpret_cast<uint8_t**>(node);
   }

   int used = 0;
   for (uint32_t i = 0; i < slots; ++i) if (claimed[i]) ++used;

   if (wanting > s_peakWanting)   s_peakWanting  = wanting;
   if (sounding + stale > s_peakBound) s_peakBound = sounding + stale;
   if (sounding > s_peakSounding) s_peakSounding = sounding;
   if (stale > s_peakStale)       s_peakStale    = stale;
   if (used > s_peakSlots)        s_peakSlots    = used;
   if (wanting - sounding > s_peakStarved) s_peakStarved = wanting - sounding;
   if (wanting > sounding) ++s_starvedTicks;
}

void drain_events()
{
   char when[32];

   while (s_evRead < s_evWrite) {
      // Overrun: the ring wrapped before the tick could drain it.  Say so rather
      // than replaying whatever is in the slots now.
      if (s_evWrite - s_evRead > kEventRing) {
         diag_log("[SndDiag] (event ring overran, %ld events lost)",
                  s_evWrite - s_evRead - kEventRing);
         s_evRead = s_evWrite - kEventRing;
      }

      const DiagEvent e = s_events[s_evRead % kEventRing];
      ++s_evRead;
      now_string(when, sizeof(when));

      switch (e.kind) {
         case EV_SATURATION:
            diag_log("[SndDiag] %s  SATURATION voice=%d  %u of %u sampled at the rail"
                     "  gainCurrent=%08X",
                     when, (int)e.voice, e.a, kSatScanBytes / 2 / kSatStride, e.b);
            break;
         case EV_GAIN_OVER:
            diag_log("[SndDiag] %s  GAIN OVER UNITY voice=%d  product=%.4f"
                     "  (Q15 convert overflows past 1.00003)",
                     when, (int)e.voice, (double)e.f);
            break;
         case EV_CURSOR:
            diag_log("[SndDiag] %s  PACKET CURSOR RUNAWAY voice=%d  cursor=%u request=%u",
                     when, (int)e.voice, e.a, e.b);
            break;
         default:
            break;
      }
   }
}

void report()
{
   const uint32_t mixConfig = g_mixConfig ? *g_mixConfig : 0xFFFFFFFFu;
   const uint32_t hwbufs    = g_hw3dFreeBuffers ? *g_hw3dFreeBuffers : 0xFFFFFFFFu;
   const uint32_t managed   = g_voiceManager
      ? *reinterpret_cast<uint32_t*>(g_voiceManager + kMgrNumManaged) : 0xFFFFFFFFu;

   const char* mixName = "?";
   switch (mixConfig) {
      case 1: mixName = "Software";            break;
      case 2: mixName = "DirectSoundHardware"; break; // the EAX path
      case 3: mixName = "DirectSoundSoftware"; break;
      case 4: mixName = "Disabled";            break;
   }

   char when[32];
   now_string(when, sizeof(when));

   diag_log("[SndDiag] %s  tick %d  mixConfig=%u (%s)  hwFree3D=%u  managedVoices=%u",
            when, s_ticks, mixConfig, mixName, hwbufs, managed);
   diag_log("[SndDiag]   voices: peak wanting=%d  with mVoice=%d  (of those,"
            " in-pool=%d  unrecognised=%d)  pool slots used=%d  starved ticks=%d",
            s_peakWanting, s_peakBound, s_peakSounding, s_peakStale,
            s_peakSlots, s_starvedTicks);
   diag_log("[SndDiag]   gain: calls=%ld (Q15 path %ld)  over 1.0=%ld  over 2.0=%ld"
            "  max seen=%ld.%03ld",
            s_gainCalls, s_gainQ15Calls, s_gainOverUnity, s_gainOverBadly,
            s_gainMaxMilli / 1000, s_gainMaxMilli % 1000);
   diag_log("[SndDiag]   output: writes=%ld  saturation bursts=%ld  rail samples=%ld"
            "  loudest=%ld/32767  max packet cursor=%ld",
            s_writeCalls, s_satBursts, s_satSamples, s_peakAbsSample, s_maxPacketRead);
}

void __cdecl hooked_snd_update(float dt, char full)
{
   g_origSndUpdate(dt, full);

   if (!s_headerDone) {
      s_headerDone = true;
      diag_log("[SndDiag] --- first engine tick ---");
      diag_log("[SndDiag] gain calls=0 means the hook never fired and the over-1.0 count"
               " proves nothing; calls>0 with over-1.0=0 is a real negative.");
      diag_log("[SndDiag] SATURATION lines are theory-free: they mean the output was"
               " pinned at full scale, whatever put it there. Note the time you hear"
               " anything odd and compare.");
      report();
      return;
   }

   sample_voice_pressure();
   drain_events();

   // ~10 s at 60 Hz.  Often enough to correlate a report with something the
   // player just heard, rare enough not to bloat the log across a match.
   if (++s_ticks % 600 == 0) report();
}

} // namespace

void sound_diag_install(uintptr_t exe_base)
{
   if (!g_soundDiagEnabled) return;

   if (g_build != GameBuild::Modtools) {
      diag_log("[SndDiag] install skipped: not the modtools build");
      return;
   }

   g_mixConfig       = reinterpret_cast<uint32_t*>(resolve(exe_base, kMixConfig));
   g_hw3dFreeBuffers = reinterpret_cast<uint32_t*>(resolve(exe_base, kHw3dFreeBuffers));
   g_voiceManager    = reinterpret_cast<uint8_t*>(resolve(exe_base, kVoiceManager));
   g_voicesPtr       = reinterpret_cast<uint8_t**>(resolve(exe_base, kVoicesPtr));

   g_origSndUpdate  = reinterpret_cast<fn_snd_update_t>(resolve(exe_base, kSndUpdate));
   g_origUpdateGain = reinterpret_cast<fn_update_gain_t>(resolve(exe_base, kUpdateGain));
   g_origWriteData  = reinterpret_cast<fn_write_data_t>(resolve(exe_base, kWriteData));

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG a1 = DetourAttach(reinterpret_cast<PVOID*>(&g_origSndUpdate),  hooked_snd_update);
   const LONG a2 = DetourAttach(reinterpret_cast<PVOID*>(&g_origUpdateGain), hooked_update_gain);
   const LONG a3 = DetourAttach(reinterpret_cast<PVOID*>(&g_origWriteData),  hooked_write_data);
   const LONG rc = DetourTransactionCommit();

   diag_log("[SndDiag] install: Update=%p UpdateGain=%p WriteData=%p"
            " attach=(%ld,%ld,%ld) commit=%ld",
            (void*)g_origSndUpdate, (void*)g_origUpdateGain, (void*)g_origWriteData,
            a1, a2, a3, rc);

   if (rc != NO_ERROR) g_origSndUpdate = nullptr;
}

void sound_diag_uninstall()
{
   if (!g_origSndUpdate) return;

   drain_events();
   diag_log("[SndDiag] --- final ---");
   report();

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(reinterpret_cast<PVOID*>(&g_origSndUpdate),  hooked_snd_update);
   DetourDetach(reinterpret_cast<PVOID*>(&g_origUpdateGain), hooked_update_gain);
   DetourDetach(reinterpret_cast<PVOID*>(&g_origWriteData),  hooked_write_data);
   DetourTransactionCommit();
   g_origSndUpdate = nullptr;
}
