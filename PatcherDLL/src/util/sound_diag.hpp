#pragma once

#include <stdint.h>

// =============================================================================
// Sound diagnostic.
//
// Two questions about BF2 audio cannot be settled by reading the disassembly,
// and both have already cost more static analysis than they were worth:
//
//   1. How many voices does this machine actually get?  The 32-voice pool is
//      not the binding limit under EAX -- Engine::Open throws the 32 away and
//      substitutes dwFreeHw3DStreamingBuffers - 8, which can legally be as low
//      as 6.  Whether a player is running 6 voices or 32 is a property of their
//      DirectSound layer, not of the binary, so it has to be read at runtime.
//
//   2. What produces the random loud, distorted burst under EAX?  Five
//      hypotheses have been checked and refuted on paper.  The two that survive
//      are both conditional on runtime state, so counting how often each
//      precondition actually occurs settles them in one session:
//
//      * DSBufferRenderer::UpdateGain converts gain to Q15 with
//        (int)(gain * 32767.0f) << 16 and no clamp.  Any gain above 1.00003
//        pushes bit 15 into the sign bit, and the per-sample smoothing
//        current += (target - current) >> 1 then subtracts across the sign
//        boundary and overflows -- a full-scale burst.  This fixed-point path
//        exists ONLY in the DirectSound renderer, i.e. only under EAX; in
//        software mixing gain stays float through SoftOutput::SetGain.  The
//        engine asserts gain <= 1.0f in StreamRenderer::Update and does not
//        enforce it, so whether content ever exceeds 1.0 is the open question.
//
//      * StreamResampler::GetPacket publishes mOutputPacket.mBufferUsed in
//        SAMPLES on the unity-rate path (modtools 0x008A59A8) but in BYTES on
//        the interpolating path (0x008A5A99), while every consumer reads bytes.
//        It only bites when the step size becomes exactly 0x10000 while an
//        input packet is still held -- mInputPacket is assigned on the
//        interpolating path only, so this needs a pitch sweep to land on unity
//        mid-packet.
//
// Counters only, and the two audio-path hooks do nothing but increment; all
// logging happens on the engine tick.  Off by default: [Fixes] SoundDiagnostic.
//
// modtools only -- the addresses are not derived for retail, and the installer
// no-ops elsewhere.
// =============================================================================

extern bool g_soundDiagEnabled;

void sound_diag_install(uintptr_t exe_base);
void sound_diag_uninstall();
