#pragma once

#include <stdint.h>

// =============================================================================
// Concurrent voice limit.
//
// Stock BF2 plays at most 32 sounds at once.  Measured in a real match, 120
// VoiceVirtuals wanted a voice against those 32, on every single engine tick of
// the session -- so roughly ninety sounds a second are losing the arbitration
// and simply never reaching the speakers.
//
// Under EAX (mixConfig 2) the 32 is not even the real cap.  Engine::Open throws
// it away and uses dwFreeHw3DStreamingBuffers - 8 instead.  That global is not
// what the sound card reports, though: it is what the engine's own capability
// probe managed to create, and the probe is told to stop at gMaxVoices + 8 and
// hands it a stack array of exactly that many DSBuffers:
//
//   008866B4  PUSH 0x28              ; 40 element array...
//   008866E4  MOV  EDX,[ESP+0x12C4]  ; ...and gMaxVoices...
//   008866EB  ADD  EDX,0x8           ; ...+ 8 as the probe's limit
//   0088670D  CALL 0x00894430        ; returns min(creatable, limit)
//
// so it returns 40, 40 - 8 = 32, and Engine::Open caps at 32 anyway.  On a
// machine reporting 129 free hardware 3D buffers the engine asks for 40 of them
// and throws the rest away.  Three self-imposed limits stacked on hardware that
// has the headroom sitting idle.
//
// This raises all three together.  gMaxVoices is a global with an existing
// command-line writer, so the probe's limit follows it for free -- but the
// probe's ARRAY has to grow with it or the probe overruns a stack buffer, and
// enlarging a 0x1298-byte stack frame means re-deriving every ESP-relative
// offset in a 2300-byte function.  So the array is moved off the stack instead:
// all four references to it are 7-byte `LEA r32,[ESP+disp32]`, which a 5-byte
// `MOV r32,imm32` plus two NOPs fits inside exactly.  The frame is untouched.
//
//   008866B8  ctor       LEA EAX,[ESP+0x8AC]
//   008866F0  probe arg  LEA EAX,[ESP+0x8A4]
//   00886738  post-probe LEA EDX,[ESP+0x8A0]
//   0088678B  dtor       LEA EAX,[ESP+0x8A8]
//
// (Four different displacements for one array because ESP moves underneath it.
// Missing any one of them would leave the engine constructing, probing,
// consuming and destroying two different buffers.  That is also why the probe
// cannot simply be hooked to report a bigger number: 0x00886740 is handed the
// array AND the returned count, so a count larger than the array is an overrun.)
//
// The Voice pool itself moves the same way the audio stream arrays already do --
// Engine::Open writes smVoices from an immediate (0x00882C43), so repointing
// that immediate makes the engine install our buffer on every open, and the four
// loop bounds that walk the pool by byte offset are widened to match.
//
// What is NOT a blocker, despite looking like one: SoftOutput's mixer has a hard
// 32 inputs, and Voice::Initialize takes one per voice.  Past 32 it gets -1 --
// and then falls straight through to creating the voice's own DirectSound buffer
// anyway (0089E2DF JZ -> 0089E2F2), which is what actually carries audio under
// EAX.  So the mixer cap costs nothing in mixConfig 2.  In mixConfig 1 SoftOutput
// IS the mixer, voices past 32 would be silent, and this feature stays off.
//
// modtools only -- addresses are not derived for retail, and the installer
// no-ops elsewhere.
// =============================================================================

// 0 = stock (32).  Otherwise the desired voice count, clamped to [33, 119]:
// the ceiling compares are sign-extended imm8, and the probe's array count is a
// PUSH imm8 of value + 8.
extern int g_voiceLimit;

// Widen the software mixer too, so voices past 32 are audible in mixConfig 1.
// OFF by default: see the SOFTWARE MIXER HAZARD note in voice_limit.cpp. The
// relocation itself is correct, but the engine reconnects voices on a mix-config
// change without testing for the -1 that GetUnconnectedInput returns when it has
// no free input, and writes table[-1]. Do not enable until that is guarded.
extern bool g_softwareVoices;

void voice_limit_install(uintptr_t exe_base);
void voice_limit_uninstall();
