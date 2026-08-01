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
inline constexpr uintptr_t kTeam1Num           = 0x0018;  // int      m_team1Num
inline constexpr uintptr_t kTeam2Num           = 0x001c;  // int      m_team2Num
inline constexpr uintptr_t kRandomBackDropName = 0x00a0;  // char[64] m_randomBackDropFileName
inline constexpr uintptr_t kTeamModelOmega     = 0x010c;  // float    m_teamModelOmega
inline constexpr uintptr_t kLoadingScreen      = 0x01c0;  // RedInterfaceScreen m_loadingScreen
inline constexpr uintptr_t kModelTeamIcon      = 0x0430;  // Red3DModelElement[4]
inline constexpr uintptr_t kTextLoading        = 0x01f0;  // RedTextElement, the blinking "Loading"
inline constexpr uintptr_t kTextMissionName    = 0x02b0;  // RedTextElement
inline constexpr uintptr_t kTextModeName       = 0x0370;  // RedTextElement
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

// ---- The interface screen every element lives on ---------------------------
// m_loadingScreen is the RedInterfaceScreen the four corner groups (and hence
// the team icons) are children of.  Confirmed at +0x1c0 from PlatformRender
// (mt 0x006d05b0, `LEA ECX,[ESI + 0x1c0]` straight into
// RedInterfaceScreen::Render 0x00817870).  Its first two fields are the pixel
// dimensions, and the LoadDisplay ctor copies them verbatim from
// RedInterfaceScreen::s_screenFull, so they are the real device resolution.
//
// **This is the unit of every element position.**  RedInterfaceScreen::Render
// places each group at ((relX - 0.5) * m_uiWidthInPixels,
// (relY - 0.5) * -m_uiHeightInPixels, -m_uiWidthInPixels / sfWidth) and builds
// the camera frustum so that one world unit at that depth is one device pixel.
// Child offsets add into the same space, which is why the engine's own
// RedInterfaceElement::SetPosition calls pass things like `textExtent + 10.0`.
// **Element +y is DOWN**, measured in game 2026-08-01. (An earlier reading of
// the Y negation above talked me into y-up; it is not. Trust the measurement.)
inline constexpr uintptr_t kScreenWidthPx  = kLoadingScreen + 0x00;  // uint m_uiWidthInPixels
inline constexpr uintptr_t kScreenHeightPx = kLoadingScreen + 0x04;  // uint m_uiHeightInPixels

// Safe zone, in screen-relative units. A group with `+0x98` bit 0 set (which
// m_groupBottomRight is) has its screen-relative position remapped through this
// before Render uses it: eff = rel * size + offset. Needed to know where the
// bottom-right anchor actually sits when the safe zone is inset.
inline constexpr uintptr_t kSafeZoneOffsetX = kLoadingScreen + 0x08;  // float
inline constexpr uintptr_t kSafeZoneOffsetY = kLoadingScreen + 0x0c;  // float
inline constexpr uintptr_t kSafeZoneWidth   = kLoadingScreen + 0x10;  // float
inline constexpr uintptr_t kSafeZoneHeight  = kLoadingScreen + 0x14;  // float

// ---- Team icon models ------------------------------------------------------
// m_modelTeamIcon is Red3DModelElement[4], one slot per faction.  Two separate
// engine paths index the same array with the same numbering:
//
//   LoadConfig's TeamModel(teamName, modelName) -> GetTeamNum(hash of teamName)
//   LoadDisplay::SetLoadState -> MissionPlayList::GetCurrentTeamNames
//
// GetTeamNum's four accepted names were resolved from its hash constants
// (FNV-1a, case-insensitive) 2026-07-28:  all=0, rep=1, cis=2, imp=3.
// Anything else returns -1 and TeamModel silently does nothing.
//
// PostLoad parents the slots named by m_team1Num / m_team2Num to
// m_groupBottomRight at x = -100 / +100, sets m_OmegaY = +/- m_teamModelOmega,
// and then calls Enable(false) on them.  Nothing ever calls Enable(true), which
// is the whole reason stock loading screens never show a team model.
//
// **A slot left at -1 is not merely hidden - PostLoad skips the entire block, so
// it is never AddElement'd to the group and is not in the render list at all.**
// That is why the extension writes m_team1Num/m_team2Num itself from
// hooked_load_config, which runs after SetLoadState and before PostLoad.
//
// m_groupBottomRight itself is placed by PostLoad at screen-relative (1.0, 1.0)
// with the safe-zone flag (+0x98 bit 0) set, i.e. the bottom-right corner of the
// safe zone.  Those hardcoded -100/+100 are pixels along x from that corner, so
// stock placement puts the second model 100 px past the right edge anyway.
//
// The array is faction-indexed only by convention (GetTeamNum maps all/rep/cis/
// imp to 0..3); nothing in the render path cares, so the extension uses slots 0
// and 1 as plain positions and never consults a faction name.
inline constexpr uintptr_t kTeamIconStride = 0x0150;  // sizeof(Red3DModelElement)
inline constexpr int       kNumTeamIcons   = 4;

inline constexpr uintptr_t team_icon(int slot) {
    return kModelTeamIcon + kTeamIconStride * (uintptr_t)slot;
}

// Red3DModelElementLite fields, relative to the element base.  Read off modtools
// Red3DModelElementLite::SetModel 0x00839ea0, which agrees field-for-field with
// the Phantom PDB struct.  SetModel stores the hash, looks it up in the global
// RedModel table, and sets kElemSearchForModel when the name did not resolve -
// which is what happens if the model is not in load.lvl.
inline constexpr uintptr_t kElemModelHash  = 0x74;  // uint32_t m_uiModelHash
inline constexpr uintptr_t kElemModel      = 0x78;  // RedModel* m_Model, null if unresolved
inline constexpr uintptr_t kElemModelFlags = 0x80;
inline constexpr uint8_t   kElemSearchForModel = 0x02;

// Red3DModelElement's own fields.
//
// kElemScale / kElemOmegaY come from the Phantom PDB struct (336 bytes = 0x150,
// which is where kTeamIconStride comes from) and kElemOmegaY is confirmed on
// modtools two ways: PostLoad writes m_teamModelOmega straight to +0x94, and a
// live dump read exactly the configured TeamModelRotationSpeed back out of it
// on the one slot PostLoad touched.
//
// **Position is NOT where the Phantom struct puts it.** Phantom has
// m_translation at +0xac; the modtools position setter (0x008285a0, the one
// PostLoad calls with -/+100) writes a homogeneous vec4 at +0x60 instead:
//     [0x60]=x  [0x64]=y  [0x68]=z  [0x6c]=1.0
// Reading +0xac on modtools just returns zeros.  The element classes diverge
// between the debug and release builds here even though the total size matches,
// so do not port element-internal offsets from Phantom without checking.
inline constexpr uintptr_t kElemScale    = 0x90;  // float m_scale, 1.0 from the ctor
inline constexpr uintptr_t kElemOmegaY   = 0x94;  // float m_OmegaY
inline constexpr uintptr_t kElemPosition = 0x60;  // float[4] x,y,z,w - w is 1.0

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
// Visibility is a flag bit rather than a bool, and the bit is directly writable.
//
// The setter is RedInterfaceElement::Enable(bool) - element vtable slot 1, one
// instruction: flags = (arg << 8) | (flags & ~0x100).  There is a *second*,
// separate setter, Visible(bool) at slot 2, which drives bit 10 - but Enable is
// the one the engine actually toggles (PostLoad shows the progress bar with
// Enable(true) and nothing else), so kElemVisible below is the Enable bit and
// is what governs whether an element draws.
inline constexpr uintptr_t kElemFlags     = 0x14;
inline constexpr uint32_t  kElemVisible   = 0x100;  // Enable(bool),  slot 1, bit 8
inline constexpr uint32_t  kElemVisibleAlt = 0x400; // Visible(bool), slot 2, bit 10

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
