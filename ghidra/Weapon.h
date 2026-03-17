/*
 * Weapon & WeaponClass struct layouts for Ghidra "Parse C Source" import.
 * Based on PDB data verified against Ghidra decompilation.
 *
 * Weapon:      320 bytes (0x140). Base: CDC (32 bytes, vtable + opaque data).
 * WeaponClass: 772 bytes (0x304). Base: Factory<Weapon,WeaponClass,WeaponDesc> (32 bytes).
 *
 * PDB offsets verified 1:1 against Ghidra constructor & Update decompilation.
 * Every named field from PDB +0x20 onward matches the Ghidra struct exactly.
 *
 * Key offsets (from MEMORY.md, confirmed):
 *   Weapon+0x060 = WeaponClass* mStart / mClass / mRenderClass
 *   Weapon+0x088 = AmmoCounter* m_pAmmoCounter  (was thought to be OrdnanceFactory*)
 *   Weapon+0x0C8 = MAP mSoldierAnimationMap
 *   WeaponClass+0x020 = WEAPON mSoldierAnimationWeapon
 */

#pragma pack(1)

/* ===== Forward declarations ===== */
typedef struct Weapon Weapon;
typedef struct WeaponClass WeaponClass;
typedef struct Controllable Controllable;
typedef struct Aimer Aimer;
typedef struct Trigger Trigger;
typedef struct AmmoCounter AmmoCounter;
typedef struct EnergyBar EnergyBar;
typedef struct OrdnanceClass OrdnanceClass;
typedef struct RedModel RedModel;
typedef struct FLEffectClass FLEffectClass;
typedef struct ParticleEmitterObject ParticleEmitterObject;
typedef struct GameObject GameObject;

/* ===== Basic types (shared with EntitySoldier.h) ===== */
#ifndef BF2_BASIC_TYPES
#define BF2_BASIC_TYPES

typedef struct PblVector3 {
    float x;
    float y;
    float z;
} PblVector3;

typedef struct PblVector4 {
    float x;
    float y;
    float z;
    float w;
} PblVector4;

typedef struct PblMatrix {
    PblVector4 right;
    PblVector4 up;
    PblVector4 forward;
    PblVector4 trans;
} PblMatrix; /* 64 bytes */

typedef unsigned int uint;
typedef unsigned char uchar;
typedef unsigned short ushort;

typedef struct PblHash {
    uint _uiValue;
} PblHash; /* 4 bytes */

typedef struct GameSoundControllable {
    ushort mVoiceVirtualHandle;
    uchar mFlags;
    uchar _pad;
} GameSoundControllable; /* 4 bytes */

typedef struct GameSound {
    uint mSoundId;
    uint mParam;
    uint _data[3];
} GameSound; /* 20 bytes */

typedef struct RedColor {
    uint packed;
} RedColor; /* 4 bytes */

typedef int MAP;
typedef int WEAPON;
typedef int BANK;
typedef int ANIMATION;

typedef struct PblHandle_ParticleEmitterObject {
    ParticleEmitterObject *mObject;
    uint mSavedHandleId;
} PblHandle_ParticleEmitterObject; /* 8 bytes */

typedef struct PblHandle_GameObject_const {
    void *mObject;
    uint mSavedHandleId;
} PblHandle_GameObject_const; /* 8 bytes */

#endif /* BF2_BASIC_TYPES */

/* ===== Sub-structs ===== */

typedef struct AmmoCounterClass {
    int mRoundsPerClip;
    float mAmmoPerRound;
} AmmoCounterClass; /* 8 bytes */

typedef struct EnergyBarClass {
    float m_fEnergyMax;
    float m_fEnergyMin;
    float m_fEnergyOverheat;
} EnergyBarClass; /* 12 bytes */

/* ===== Weapon::WeaponState enum ===== */
typedef enum WeaponState {
    WPN_IDLE = 0,
    WPN_FIRE = 1,
    WPN_RELOAD = 2,
    WPN_OVERHEAT = 3
} WeaponState;

/* ===== SoldierAnimationBank::WEAPON enum ===== */
typedef enum SoldierAnimWeapon {
    INVALID_WEAPON = -1,
    WEAPON_RIFLE = 0,
    WEAPON_BAZOOKA = 1,
    WEAPON_TOOL = 2,
    WEAPON_PISTOL = 3,
    WEAPON_MELEE = 4,
    NUM_STANDARD_WEAPONS = 5,
    MAX_WEAPONS = 20
} SoldierAnimWeapon;

/* ===========================================================================
 * Weapon — 320 bytes (0x140)
 *
 * Base class: CDC (32 bytes) — vtable + opaque data.
 * Ghidra applies this to Weapon* pointers.
 * =========================================================================== */

struct Weapon {

    /* ---- CDC base (0x00-0x1F, 32 bytes) ---- */
    void *vftable;                                       /* +0x000 */
    uchar _cdc_data[28];                                 /* +0x004 */

    /* ---- Weapon fields (0x20 onward, from PDB) ---- */
    PblMatrix mFirePointMatrix;                          /* +0x020, 64 bytes */
    WeaponClass *mStart;                                 /* +0x060 */
    WeaponClass *mClass;                                 /* +0x064 */
    WeaponClass *mRenderClass;                           /* +0x068 */
    Controllable *mOwner;                                /* +0x06C */
    Aimer *mAimer;                                       /* +0x070 */
    Trigger *mTrigger;                                   /* +0x074 */
    Trigger *mReload;                                    /* +0x078 */
    PblVector3 mFirePos;                                 /* +0x07C, 12 bytes */
    AmmoCounter *m_pAmmoCounter;                         /* +0x088 */
    EnergyBar *m_pEnergyBar;                             /* +0x08C */
    float mCurChargeStrengthHeavy;                       /* +0x090 */
    float mCurChargeStrengthLight;                       /* +0x094 */
    float mCurChargeDelayHeavy;                          /* +0x098 */
    float mCurChargeDelayLight;                          /* +0x09C */
    float mCurChargeRateLight;                           /* +0x0A0 */
    float mCurChargeRateHeavy;                           /* +0x0A4 */
    float mCurTimeAtMaxCharge;                           /* +0x0A8 */
    uint mBitfield_0xAC;                                 /* +0x0AC, bitfield: mHideWeapon:1, mFiredFlag:1, mSelectedFlag:1, m_iSoldierState:6, ... */
    WeaponState mState;                                  /* +0x0B0 */
    float mStateTimer;                                   /* +0x0B4 */
    float mStateLimit;                                   /* +0x0B8 */
    float mZoom;                                         /* +0x0BC */
    float mZoomTurnScale;                                /* +0x0C0 */
    float mMuzzleFlashStartTime;                         /* +0x0C4 */
    MAP mSoldierAnimationMap;                            /* +0x0C8 */
    GameSoundControllable mSoundControl;                 /* +0x0CC, 4 bytes */
    GameSoundControllable mSoundControlFire;             /* +0x0D0, 4 bytes */
    GameSoundControllable mFoleyFXControl;               /* +0x0D4, 4 bytes */
    GameSound mSoundProps;                               /* +0x0D8, 20 bytes */
    GameSound mSoundPropsFire;                           /* +0x0EC, 20 bytes */
    GameSound mFoleyFXProps;                             /* +0x100, 20 bytes */
    PblHandle_ParticleEmitterObject mChargeUpEmitter;    /* +0x114, 8 bytes */
    float mLastFireTime;                                 /* +0x11C */
    float mSkipTime;                                     /* +0x120 */
    uchar mSkipCharged;                                  /* +0x124, bitfield: mSkip:1, mCharged:1 */
    uchar mDeactivateScheduled;                          /* +0x125 */
    uchar _pad_126[2];                                   /* +0x126 */
    PblHandle_GameObject_const mTarget;                  /* +0x128, 8 bytes */
    int mTargetBodyID;                                   /* +0x130 */
    uchar _tail[12];                                     /* +0x134, pad to 0x140 */
};

/* Static assert for Weapon size */
typedef char _check_Weapon_size[sizeof(struct Weapon) == 0x140 ? 1 : -1];

/* ===========================================================================
 * WeaponClass — 772 bytes (0x304)
 *
 * Base class: Factory<Weapon,WeaponClass,WeaponDesc> (32 bytes).
 * =========================================================================== */

struct WeaponClass {

    /* ---- Factory base (0x00-0x1F, 32 bytes) ---- */
    void *vftable;                                       /* +0x000 */
    uchar _factory_node[16];                             /* +0x004, PblList<Factory>::Node */
    void *mParent;                                       /* +0x014, Factory* */
    uint mId;                                            /* +0x018 */
    uint mNetIndex;                                      /* +0x01C */

    /* ---- WeaponClass fields (0x20 onward, from PDB) ---- */
    WEAPON mSoldierAnimationWeapon;                      /* +0x020 */
    PblVector3 mFirePointOffset;                         /* +0x024, 12 bytes */
    char mFilename[32];                                  /* +0x030 */
    ushort *mLabel;                                      /* +0x050 */
    uint mNameHash;                                      /* +0x054 */
    uint mHUDNameHash;                                   /* +0x058 */
    float mStrengthLimit;                                /* +0x05C */
    WeaponClass *mNextCharge;                            /* +0x060 */
    RedModel *mModel;                                    /* +0x064 */
    RedModel *mHighResModel;                             /* +0x068 */
    uint mIconTexture;                                   /* +0x06C */
    uint mReticuleTexture;                               /* +0x070 */
    uint mScopeTexture;                                  /* +0x074 */
    OrdnanceClass *mOrdnanceClass;                       /* +0x078 */
    float mRoundsPerShot;                                /* +0x07C */
    float mHeatPerShot;                                  /* +0x080 */
    float mHeatRecoverRate;                              /* +0x084 */
    AmmoCounterClass mAmmoCounterClass;                  /* +0x088, 8 bytes */
    EnergyBarClass mEnergyBarClass;                      /* +0x090, 12 bytes */
    float mReloadTime;                                   /* +0x09C */
    float mZoomMin;                                      /* +0x0A0 */
    float mZoomMax;                                      /* +0x0A4 */
    float mZoomRate;                                     /* +0x0A8 */
    float mZoomTurnDivisorMin;                           /* +0x0AC */
    float mZoomTurnDivisorMax;                           /* +0x0B0 */
    float mRecoilStrengthHeavy;                          /* +0x0B4 */
    float mRecoilStrengthLight;                          /* +0x0B8 */
    float mRecoilLengthLight;                            /* +0x0BC */
    float mRecoilLengthHeavy;                            /* +0x0C0 */
    float mRecoilDelayLight;                             /* +0x0C4 */
    float mRecoilDelayHeavy;                             /* +0x0C8 */
    float mRecoilDecayLight;                             /* +0x0CC */
    float mRecoilDecayHeavy;                             /* +0x0D0 */
    float mChargeRateLight;                              /* +0x0D4 */
    float mChargeRateHeavy;                              /* +0x0D8 */
    float mMaxChargeStrengthHeavy;                       /* +0x0DC */
    float mMaxChargeStrengthLight;                       /* +0x0E0 */
    float mChargeDelayLight;                             /* +0x0E4 */
    float mChargeDelayHeavy;                             /* +0x0E8 */
    float mTimeAtMaxCharge;                              /* +0x0EC */
    float mExtremeRange;                                 /* +0x0F0 */
    float mLockOnRange;                                  /* +0x0F4 */
    float mLockOnAngle;                                  /* +0x0F8 */
    float mLockOffAngle;                                 /* +0x0FC */
    float mMinRange;                                     /* +0x100 */
    float mOptimalRange;                                 /* +0x104 */
    float mMaxRange;                                     /* +0x108 */
    int mTargetSides;                                    /* +0x10C */
    int mTargetTypes;                                    /* +0x110 */
    int mAITargetTypes;                                  /* +0x114 */
    GameSound mFireSound;                                /* +0x118, 20 bytes */
    GameSound mFireLoopSound;                            /* +0x12C, 20 bytes */
    GameSound mFireEmptySound;                           /* +0x140, 20 bytes */
    GameSound mReloadSound;                              /* +0x154, 20 bytes */
    GameSound mChargeSound;                              /* +0x168, 20 bytes */
    float mChargeSoundPitch;                             /* +0x17C */
    GameSound mChargedSound;                             /* +0x180, 20 bytes */
    GameSound mWeaponChangeSound;                        /* +0x194, 20 bytes */
    GameSound mOverheatSound;                            /* +0x1A8, 20 bytes */
    float mOverheatSoundPitch;                           /* +0x1BC */
    uint mFireSoundStop;                                 /* +0x1C0 */
    GameSound mFoleyFX[10];                              /* +0x1C4, 200 bytes */
    FLEffectClass *mChargeUpEffect;                      /* +0x28C */
    float mFlashLength;                                  /* +0x290 */
    float mEnergyDrain;                                  /* +0x294 */
    float mAutoPitchScreenDist;                          /* +0x298 */
    float mAutoTurnScreenDist;                           /* +0x29C */
    float mTargetLockMaxDistance;                        /* +0x2A0 */
    float mTargetLockMaxDistanceLose;                    /* +0x2A4 */
    short mScoreForMedalsType;                           /* +0x2A8 */
    short mMedalsTypeToUnlock;                           /* +0x2AA */
    short mMedalsTypeToLock;                             /* +0x2AC */
    uchar _pad_2AE[2];                                   /* +0x2AE */
    uint mBitfield_0x2B0;                                /* +0x2B0, bitfield: mFireAnim:2, mTriggerSingle:1, mZoomFirstPerson:1, mSniperScope:1, mMaxRangeDefault:1, mInstantPlayFireAnim:1, mIsOffhand:1, ... */
    uint mBitfield_0x2B4;                                /* +0x2B4, bitfield: mHashWeaponName:1, mDisplayRefire:1, mPostLoadInit:1, mWarnedAboutLocalization:1, mReticuleInAimingOnly:1, mNoFirstPersonFireAnim:1, mStopChargeSound:1, mAIUseBubbleCircle:1 */
    float mAIBubbleSizeMultiplier;                       /* +0x2B8 */
    float mAIBubbleScaleDistDivider;                     /* +0x2BC */
    float mAIBubbleScaleClamp;                           /* +0x2C0 */
    RedModel *mMuzzleFlashModel;                         /* +0x2C4 */
    FLEffectClass *mMuzzleFlashEffect;                   /* +0x2C8 */
    float mFlashRadius;                                  /* +0x2CC */
    float mFlashWidth;                                   /* +0x2D0 */
    RedColor mFlashColor;                                /* +0x2D4 */
    RedColor mFlashLightColor;                           /* +0x2D8 */
    float mFlashLightRadius;                             /* +0x2DC */
    float mFlashLightDuration;                           /* +0x2E0 */
    int mBarrageMin;                                     /* +0x2E4 */
    int mBarrageMax;                                     /* +0x2E8 */
    float mBarrageDelay;                                 /* +0x2EC */
    char mName[20];                                      /* +0x2F0, 20 bytes, to 0x304 */
};

/* Static assert for WeaponClass size */
typedef char _check_WeaponClass_size[sizeof(struct WeaponClass) == 0x304 ? 1 : -1];

#pragma pack()
