#pragma once

#include <cstdint>
#include <cstddef>

#include "../pbl/PblVector3.hpp"

namespace game {

// -------------------------------------------------------------------------
// CameraTrackSetting
// -------------------------------------------------------------------------

struct CameraTrackSetting {
    PblVector3 mCameraEyePointOffset;    // +0x00
    PblVector3 mCameraTrackCenter;       // +0x0C
    PblVector3 mCameraTrackOffset;       // +0x18
    PblVector3 mCameraSafePoint;         // +0x24
    float      mCameraMoveTensionL;      // +0x30
    float      mCameraMoveTensionR;      // +0x34
    float      mCameraMoveTensionU;      // +0x38
    float      mCameraMoveTensionD;      // +0x3C
    float      mCameraMoveTensionF;      // +0x40
    float      mCameraMoveTensionB;      // +0x44
    float      mCameraAimTension;        // +0x48
    float      mCameraTiltValue;         // +0x4C
};
static_assert(sizeof(CameraTrackSetting) == 0x50);
static_assert(offsetof(CameraTrackSetting, mCameraEyePointOffset) == 0x00);
static_assert(offsetof(CameraTrackSetting, mCameraTrackCenter) == 0x0C);
static_assert(offsetof(CameraTrackSetting, mCameraTrackOffset) == 0x18);
static_assert(offsetof(CameraTrackSetting, mCameraSafePoint) == 0x24);
static_assert(offsetof(CameraTrackSetting, mCameraMoveTensionL) == 0x30);
static_assert(offsetof(CameraTrackSetting, mCameraAimTension) == 0x48);
static_assert(offsetof(CameraTrackSetting, mCameraTiltValue) == 0x4C);

} // namespace game
