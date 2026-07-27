#pragma once

#include <stdint.h>

// =============================================================================
// LoadDisplay / ProgressIndicator field offsets
// =============================================================================
// Field names come from the Phantom build's real PDB.  The modtools, Steam and
// GOG layouts are IDENTICAL to each other and equal the Phantom layout with a
// constant -1872 (0x750) shift applied from m_progressBar onward; the whole
// divergence is BorderedBox m_tipsBox being 608 bytes here versus 2480 there.
//
// Verified 2026-07-27 against modtools disassembly (PostLoad 0x67bd50,
// LoadDataChunk 0x67dea0, LoadData 0x67e360) and Steam LoadDataChunk 0x5776e0.
// Full table and derivation: docs/LoadDisplaySystem.md.
//
// One table serves all three builds — do NOT add per-build variants without
// re-deriving, the whole point is that they agree.

namespace load_display {

// ---- LoadDisplay, relative to the LoadDisplay* (ecx) -----------------------

inline constexpr uintptr_t kBDisplay           = 0x0000;  // bool     m_bDisplay
inline constexpr uintptr_t kMissionHash        = 0x0004;  // uint32_t m_missionHash
inline constexpr uintptr_t kEntireMissionHash  = 0x0008;  // uint32_t m_entireMissionHash
inline constexpr uintptr_t kRandomBackDropName = 0x00a0;  // char[64] m_randomBackDropFileName
inline constexpr uintptr_t kGroupLoadingTips   = 0x0970;  // RedScreenGroupElement
inline constexpr uintptr_t kProgressBar        = 0x0d30;  // ProgressIndicator
inline constexpr uintptr_t kGroupTopLeft       = 0x0e00;  // holds m_textMissionName
inline constexpr uintptr_t kGroupTopRight      = 0x0ea0;  // holds m_textModeName
inline constexpr uintptr_t kGroupBottomLeft    = 0x0f40;  // holds m_textLoading + m_progressBar
inline constexpr uintptr_t kGroupBottomRight   = 0x0fe0;  // holds the team icon models
inline constexpr uintptr_t kBackDropHash       = 0x14c0;  // uint32_t m_backDropHash
inline constexpr uintptr_t kTipTimer           = 0x14cc;  // float    m_tipTimer
inline constexpr uintptr_t kTotalTime          = 0x14d0;  // float    m_totalTime
inline constexpr uintptr_t kModels             = 0x14dc;  // RedModel*[10]
inline constexpr uintptr_t kTextures           = 0x1504;  // RedTexture*[50]
inline constexpr uintptr_t kSkeletons          = 0x15cc;  // RedSkeleton*[10]
inline constexpr uintptr_t kNumModels          = 0x15f4;  // int
inline constexpr uintptr_t kNumTextures        = 0x15f8;  // int
inline constexpr uintptr_t kNumSkeletons       = 0x15fc;  // int

// Capacities of the three arrays above.  LoadDataChunk appends to them with no
// bounds check of its own; see loading_screen/data_guard.cpp.
inline constexpr int kMaxModels    = 10;
inline constexpr int kMaxTextures  = 50;
inline constexpr int kMaxSkeletons = 10;

// ---- ProgressIndicator, relative to its own base (LoadDisplay + kProgressBar)

namespace pi {
inline constexpr uintptr_t kPLED       = 0x0090;  // RedBitmapElement* m_pLED
inline constexpr uintptr_t kPIntensity = 0x0094;  // float*            m_pIntensity
inline constexpr uintptr_t kPColor     = 0x0098;  // RedColor*         m_pColor
inline constexpr uintptr_t kCurOnLED   = 0x009c;  // int               m_curOnLED
inline constexpr uintptr_t kDirection  = 0x00a0;  // int               m_progressBarDirection
inline constexpr uintptr_t kLEDTexHash = 0x00a4;  // uint32_t          m_LEDTexHash
inline constexpr uintptr_t kNumLEDs    = 0x00ac;  // int               m_numLEDs
inline constexpr uintptr_t kOnFraction = 0x00b0;  // float             m_OnLEDFraction
inline constexpr uintptr_t kLEDSpeed   = 0x00b4;  // float             m_LEDSpeed
inline constexpr uintptr_t kMode       = 0x00c0;  // LEDModeT          m_mode
} // namespace pi

// ---- RedInterfaceElement ---------------------------------------------------
// Visibility is a flag bit rather than a bool; the engine flips it through the
// element vtable's slot 4 setter, but the bit is directly writable.
inline constexpr uintptr_t kElemFlags   = 0x14;
inline constexpr uint32_t  kElemVisible = 0x100;

// ---- Accessors -------------------------------------------------------------

template <typename T>
inline T* at(void* loadDisplay, uintptr_t off)
{
    return (T*)((uint8_t*)loadDisplay + off);
}

// Offset of a ProgressIndicator field relative to the LoadDisplay base.
inline constexpr uintptr_t bar(uintptr_t piOff) { return kProgressBar + piOff; }

inline void set_element_visible(void* loadDisplay, uintptr_t elemOff, bool visible)
{
    uint32_t* flags = at<uint32_t>(loadDisplay, elemOff + kElemFlags);
    if (visible) *flags |= kElemVisible;
    else         *flags &= ~kElemVisible;
}

} // namespace load_display
