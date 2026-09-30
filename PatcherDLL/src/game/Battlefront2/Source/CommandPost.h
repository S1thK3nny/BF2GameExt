#pragma once

#include <stdint.h>

#include "core/build_field.hpp"
#include "game/Battlefront2/Source/GameMusic.h"
#include "game/Battlefront2/Source/GameObject.h"
#include "game/Battlefront2/Source/GameSound.h"
#include "game/Battlefront2/Source/GameSoundStream.h"
#include "game/PebbleFL/Common/PblHandle.h"
#include "game/PebbleFL/Common/PblVector.h"

class CommandPostClass;
class EntityHologram;
class EntityHologramClass;
class PblQuaternion;
class RedModel;
class RedPath;
class RedRegion;

// =============================================================================
// CommandPost and CommandPostClass, every member. Names, order and types from
// the PDB; each offset is read off an instruction in each build. Modtools is a
// debug build; Steam and GOG share the release layout, and GOG was checked by
// comparing every member access in the same functions, which sit at the same
// addresses as Steam's.
//
// Release shrinks GameSound, GameSoundStream and GameMusic to 8 bytes, which is
// the whole difference: everything from mVoiceOver's end on moves up.
//
// CommandPost functions: ctor modtools 0x0064A460 / Steam 0x0047A710, Update
// 0x0064E280 / 0x0047CE30. new CommandPost pushes 0x1A60 (0x0064FF4D) and 0xB48
// (0x0047AD9B). The +0x00 Thread base is 0x18 bytes on both.
//
//   CommandPost             modtools            Steam
//   mPostIndex              0x0018 @0064a477    0x0018 @0047a748
//   mHUDPostIndex           0x001c @0064a47a    0x001c @0047a74b
//   mName                   0x0020 @0064a551    0x0020 @0047a864
//   mLabel                  0x0024 @0064a554    0x0024 @0047a86b
//   mLocalizeName           0x0028 @0064a557    0x0028 @0047a872
//   mObject                 0x002c @0064a47d    0x002c @0047a75c
//   mPosition               0x0034 @0064a62f    0x0034 @0047a94d
//   mHoldTeam               0x0040 @0064a483    0x0040 @0047a770
//   mHoldCount              0x0044 @0064a55c    0x0044 @0047a879
//   mHoldPlayerMask         0x0068 @0064a486    0x0068 @0047a777
//   mSkippedPlayerMask      0x0070 @0064a48c    0x0070 @0047a785
//   mBiasTeam               0x0078 @0064a492    0x0078 @0047a793
//   mAICanCaptureTeamMask   0x007c @0064a495    0x007c @0047a79a
//   mCaptureRegion          0x0080 @0064a49c    0x0080 @0047a7a1
//   mControlRegion          0x0084 @0064a4a2    0x0084 @0047a7ab
//   mKillRegion             0x0088 @0064a4a8    0x0088 @0047a7b5
//   mSpawnPath              0x008c @0064a4ae    0x008c @0047a7bf
//   mAllyPath               0x0090 @0064a4b4    0x0090 @0047a7c9
//   mTurretPath             0x0094 @0064a4ba    0x0094 @0047a7d3
//   mSpawnRegion            0x0098 @0064a4c0    0x0098 @0047a7dd
//   mAllyCount              0x009c @0064a4c6    0x009c @0047a7e7
//   mNeutralizeTimer        0x00a0 @0064a4cc    0x00a0 @0047a7f1
//   mCaptureTimer           0x00a4 @0064a4d2    0x00a4 @0047a7fb
//   mSpawnQueue             0x00a8 @0064a57c    0x00a8 @0047a883
//   mBleedValue             0x00b8 @0064a593    0x00b8 @0047a891
//   mStrategicAttackValue   0x00d8 @0064a5f3    0x00d8 @0047a8f3
//   mStrategicDefendValue   0x00f8 @0064a606    0x00f8 @0047a913
//   mStrategicBan           0x0118 @0064a619    0x0118 @0047a929
//   mAISpawnWeight          0x011c @0064a61f    0x011c @0047a93b
//   mVoiceOver              0x0120 @0064a4d8    0x0120 @0047a74e   stride 0x320 / 0x140
//   m_pHologram             0x1a20 @0064a501    0x0b20 @0047a810
//   mSoundAmbience          0x1a24 @0064a4fb    0x0b24 @0047a80a
//   mSoundAmbienceProps     0x1a28 @0064a50e    0x0b28 @0047a820
//   mSoundTransition        0x1a3c @0064a519    0x0b30 @0047a82b
//   mSoundTransitionProps   0x1a40 @0064a526    0x0b34 @0047a837
//   mClass                  0x1a54 @0064a537    0x0b3c @0047a854
//   mFlags                  0x1a58 @0064a531    0x0b40 @0047a842
//
// CommandPostClass is a second base of the command vehicle classes, always
// last: every one of them allocates base offset + 0x270 on modtools
// (e.g. 0x6EC + 0x270 pushed at 0x0044D154) and + 0x194 on Steam (0x690 + 0x194
// at 0x0047659A). ctor modtools 0x00649BB0 / Steam 0x0047E3E0.
//
//   CommandPostClass        modtools            Steam
//   mNeutralizeTime         0x0004 @00649c54    0x0004 @0047e491
//   mCaptureTime            0x0008 @00649c5a    0x0008 @0047e498
//   m_pHoloModel            0x000c @00649c66    0x000c @0047e4a6
//   m_pHologramClass        0x002c @00649c60    0x002c @0047e49f
//   m_fHoloTurnOnTime       0x0030 @00649c96    0x0030 @0047e4de
//   mSoundAmbience          0x0034 @00649bb5    0x0034 @0047e40a
//   mSoundDischarge         0x00d4 @00649bd5    0x0074 @0047e41d
//   mSoundCharge            0x00e8 @00649be2    0x007c @0047e42d
//   mSoundCaptured          0x00fc @00649bef    0x0084 @0047e436
//   mSoundLost              0x0110 @00649bfc    0x008c @0047e442
//   mSoundDispute           0x0124 @00649c09    0x0094 @0047e44e
//   mSoundPitchDev          0x0138 @00649ca1    0x009c @0047e4e5
//   mMusicCapture           0x013c @00649c16    0x00a0 @0047e467
//   mMusicLost              0x019c @00649c2e    0x00e0 @0047e481
//   mSpawnRotation          0x01fc @00649d2a    0x0120 @0047e4ef
//   mSpawnPosition          0x0200 @00649d36    0x0124 @0047e4f9
//   mSpawnCount             0x0204 @00649d42    0x0128 @0047e503
//   mBleedValue             0x0208 @00649d66    0x012c @0047e549
//   mStrategicAttackValue   0x0228 @00649daa    0x014c @0047e55d
//   mStrategicDefendValue   0x0248 @00649dbd    0x016c @0047e567
//   mStrategicBan           0x0268 @00649dd3    0x018c @0047e609
//   mFlags                  0x026c @00649d4e    0x0190 @0047e602
//
// HUDIndexDisplay defaults on in CommandPostClass's constructor and is copied
// to each post; stock ODFs turn it off for invisible posts and command vehicles.
// A post's team is its object's team (GameObject +0x234, low 4 bits).
// =============================================================================

namespace layout::CommandPost {

inline constexpr BuildSize kSize{0x1A60, 0xB48};

// VoiceOver: GameSoundStream mStream[8][5], one per team.
inline constexpr BuildSize kVoiceOverSize{0x320, 0x140};
constexpr int              kVoiceOverCount = 8;

inline constexpr Field<int>                    mPostIndex{0x18};
inline constexpr Field<int>                    mHUDPostIndex{0x1C};
inline constexpr Field<const char*>            mName{0x20};
inline constexpr Field<const char*>            mLabel{0x24};
inline constexpr Field<const wchar_t*>         mLocalizeName{0x28};
inline constexpr Field<PblHandle<GameObject>>  mObject{0x2C};
inline constexpr Field<PblVector3>             mPosition{0x34};
inline constexpr Field<int>                    mHoldTeam{0x40};          // 0 none, -1 more than one team
inline constexpr Field<int[8]>                 mHoldCount{0x44};
inline constexpr Field<uint64_t>               mHoldPlayerMask{0x68};
inline constexpr Field<uint64_t>               mSkippedPlayerMask{0x70};
inline constexpr Field<int>                    mBiasTeam{0x78};          // the team the capture timer counts for
inline constexpr Field<uint32_t>               mAICanCaptureTeamMask{0x7C};
inline constexpr Field<const RedRegion*>       mCaptureRegion{0x80};
inline constexpr Field<const RedRegion*>       mControlRegion{0x84};
inline constexpr Field<const RedRegion*>       mKillRegion{0x88};
inline constexpr Field<const RedPath*>         mSpawnPath{0x8C};
inline constexpr Field<const RedPath*>         mAllyPath{0x90};
inline constexpr Field<const RedPath*>         mTurretPath{0x94};
inline constexpr Field<const RedRegion*>       mSpawnRegion{0x98};
inline constexpr Field<int>                    mAllyCount{0x9C};
inline constexpr Field<float>                  mNeutralizeTimer{0xA0};
inline constexpr Field<float>                  mCaptureTimer{0xA4};
inline constexpr Field<int[2][2]>              mSpawnQueue{0xA8};
inline constexpr Field<int[8]>                 mBleedValue{0xB8};
inline constexpr Field<int[8]>                 mStrategicAttackValue{0xD8};
inline constexpr Field<int[8]>                 mStrategicDefendValue{0xF8};
inline constexpr Field<int>                    mStrategicBan{0x118};
inline constexpr Field<float>                  mAISpawnWeight{0x11C};
inline constexpr Field<uint8_t>                mVoiceOver{0x120};        // VoiceOver[8], see kVoiceOverSize
inline constexpr Field<EntityHologram*>        m_pHologram{0x1A20, 0xB20};
inline constexpr Field<GameSoundControllable>  mSoundAmbience{0x1A24, 0xB24};
inline constexpr Field<::GameSound>            mSoundAmbienceProps{0x1A28, 0xB28};
inline constexpr Field<GameSoundControllable>  mSoundTransition{0x1A3C, 0xB30};
inline constexpr Field<::GameSound>            mSoundTransitionProps{0x1A40, 0xB34};
inline constexpr Field<CommandPostClass*>      mClass{0x1A54, 0xB3C};
inline constexpr Field<uint8_t>                mFlags{0x1A58, 0xB40};

// mFlags bits.
constexpr uint8_t kIsAISpawnPoint  = 0x01;
constexpr uint8_t kDisplayHUDIndex = 0x02;
constexpr uint8_t kIsEnabled       = 0x04;

} // namespace layout::CommandPost

namespace layout::CommandPostClass {

inline constexpr BuildSize kSize{0x270, 0x194};

inline constexpr Field<float>                  mNeutralizeTime{0x04};
inline constexpr Field<float>                  mCaptureTime{0x08};
inline constexpr Field<const RedModel*[8]>     m_pHoloModel{0x0C};
inline constexpr Field<EntityHologramClass*>   m_pHologramClass{0x2C};
inline constexpr Field<float>                  m_fHoloTurnOnTime{0x30};
inline constexpr Field<::GameSound>            mSoundAmbience{0x34};       // [8]
inline constexpr Field<::GameSound>            mSoundDischarge{0xD4, 0x74};
inline constexpr Field<::GameSound>            mSoundCharge{0xE8, 0x7C};
inline constexpr Field<::GameSound>            mSoundCaptured{0xFC, 0x84};
inline constexpr Field<::GameSound>            mSoundLost{0x110, 0x8C};
inline constexpr Field<::GameSound>            mSoundDispute{0x124, 0x94};
inline constexpr Field<float>                  mSoundPitchDev{0x138, 0x9C};
inline constexpr Field<::GameMusic>            mMusicCapture{0x13C, 0xA0}; // [8]
inline constexpr Field<::GameMusic>            mMusicLost{0x19C, 0xE0};    // [8]
inline constexpr Field<PblQuaternion*>         mSpawnRotation{0x1FC, 0x120};
inline constexpr Field<PblVector3*>            mSpawnPosition{0x200, 0x124};
inline constexpr Field<int>                    mSpawnCount{0x204, 0x128};
inline constexpr Field<int[8]>                 mBleedValue{0x208, 0x12C};
inline constexpr Field<int[8]>                 mStrategicAttackValue{0x228, 0x14C};
inline constexpr Field<int[8]>                 mStrategicDefendValue{0x248, 0x16C};
inline constexpr Field<int>                    mStrategicBan{0x268, 0x18C};
inline constexpr Field<uint8_t>                mFlags{0x26C, 0x190};

// mFlags bits.
constexpr uint8_t kDisplayHUDIndex = 0x01;

} // namespace layout::CommandPostClass
