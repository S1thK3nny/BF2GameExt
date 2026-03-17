/*
 * EntitySoldier struct layout for Ghidra "Parse C Source" import.
 * Based on confirmed RE from EntitySoldierStruct.txt and PDB data.
 *
 * Total size: 0xFC8 (4040) bytes
 *
 * Inheritance chain (with offsets from struct_base):
 *   +0x000: EntityEx (vtable0 + EntityEx_data)
 *   +0x00C: CollisionObject (vtable1 + CollisionObject_data)
 *   +0x094: RedSceneObject (vtable2 + DynDisplayable_data + RedSceneObject_data)
 *   +0x0F0: EntityGeometry fields (mMatrix, mModel, etc.)
 *   +0x140: Damageable (vtable3 + Damageable_data)
 *   +0x200: PblHandled (vtable4 + PblHandled_data)
 *   +0x208: GameObject fields
 *   +0x240: Controllable (vtable5 + Thread_data)
 *   +0x258: Trackable (vtable6 + Trackable_data + Controllable_data)
 *   +0x3CC: PlayableEntitySounds (vtable7 + data)
 *   +0x3D4: VehicleEngine
 *   +0x424: EntitySoldier_data (the soldier-specific fields)
 *
 * IMPORTANT: entity pointer (from ctrl+0x290) = struct_base + 0x240
 * All offsets in MEMORY.md "from entity" = struct_base offset - 0x240
 */

#pragma pack(1)

/* ===== Forward declarations (opaque pointer targets) ===== */
typedef struct EntitySoldier EntitySoldier;
typedef struct EntitySoldierClass EntitySoldierClass;
typedef struct EntityClass EntityClass;
typedef struct CollisionModel_full CollisionModel_full;
typedef struct AttachedEffects AttachedEffects;
typedef struct GameModel GameModel;
typedef struct Controller Controller;
typedef struct Character Character;
typedef struct OrdnanceMissile OrdnanceMissile;
typedef struct OrdnanceGrapplingHook OrdnanceGrapplingHook;
typedef struct AcklayData AcklayData;
typedef struct TentacleSimulator TentacleSimulator;
typedef struct SoldierAnimator SoldierAnimator;
typedef struct SoldierAnimatorLowRes SoldierAnimatorLowRes;
typedef struct Weapon Weapon;
typedef struct Tracker Tracker;
typedef struct Sector Sector;
typedef struct RedSceneAABox RedSceneAABox;
typedef struct RedSceneObject RedSceneObject;
typedef struct RedLodData RedLodData;
typedef struct TreeGridStack TreeGridStack;
typedef struct CollisionObject CollisionObject;
typedef struct FLEffectObject FLEffectObject;
typedef struct GameObject GameObject;
typedef struct SoldierAnimation SoldierAnimation;
typedef struct SoldierAnimationData SoldierAnimationData;
typedef struct CollisionPrimitive CollisionPrimitive;
typedef struct ZephyrAnim ZephyrAnim;
typedef struct RedPose RedPose;
typedef struct AimerClass AimerClass;
typedef struct Controllable Controllable;

/* ===== Basic types ===== */

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

typedef struct PblSphere {
    PblVector3 center;
    float radius;
} PblSphere; /* 16 bytes */

typedef struct PblBox {
    PblVector3 min;
    PblVector3 max;
} PblBox; /* 24 bytes */

typedef struct PblQuaternion {
    float x;
    float y;
    float z;
    float w;
} PblQuaternion; /* 16 bytes */

typedef unsigned int uint;
typedef unsigned char uchar;
typedef unsigned short ushort;

typedef struct PblHash {
    uint _uiValue;
} PblHash; /* 4 bytes */

typedef struct Node {
    int _next;
    int _prev;
    void *_data;
    void *_list;
} Node; /* 16 bytes */

typedef struct Trigger {
    int mValue;
} Trigger; /* 4 bytes */

typedef struct PblHandle_GameObject {
    GameObject *mObject;
    uint mSavedHandleId;
} PblHandle_GameObject; /* 8 bytes */

typedef struct PblHandle_FLEffectObject {
    FLEffectObject *mObject;
    uint mSavedHandleId;
} PblHandle_FLEffectObject; /* 8 bytes */

typedef struct PblHandle_GameObject_const {
    void *mObject;
    uint mSavedHandleId;
} PblHandle_GameObject_const; /* 8 bytes */

typedef struct GameSoundControllable {
    ushort mVoiceVirtualHandle;
    uchar mFlags;
    uchar _pad;
} GameSoundControllable; /* 4 bytes */

typedef struct GameSound {
    uint mSoundId;
    uint mParam;
} GameSound; /* 8 bytes */

typedef struct GameMusic {
    uint mSoundId;
    uint mParam;
    uint mFlags;
} GameMusic; /* 12 bytes */

typedef struct DamageOwner {
    PblHandle_GameObject mObject;
    int mWeaponOrdnance;
    int mTeam;
    float mTimeStamp;
    int _reserved[2];
} DamageOwner; /* 28 bytes */

typedef struct EnergyBar {
    float mCurEnergy;
    float mMaxEnergy;
    float mAddEnergy;
} EnergyBar; /* 12 bytes */

/* ===== Enums ===== */

typedef enum SoldierState {
    SOLDIER_STAND = 0,
    SOLDIER_CROUCH = 1,
    SOLDIER_PRONE = 2,
    SOLDIER_SPRINT = 3,
    SOLDIER_JET_JUMP = 4,
    SOLDIER_JET_HOVER = 5,
    SOLDIER_JUMP = 6,
    SOLDIER_ROLL = 7,
    SOLDIER_TUMBLE = 10,
    SOLDIER_DEAD = 99
} SoldierState;

typedef int MAP;
typedef int ANIMATION;
typedef int CollisionState;

/* ===== Sub-object: TreeGridObject (36 bytes) ===== */

typedef struct TreeGridObject {
    TreeGridStack *mStackPtr;
    int mStackIdx;
    PblSphere mCollisionSphere;
    float m2dRadius;
    int mData[1];
    int mTreeGridCellIndex;
} TreeGridObject; /* 36 bytes */

/* ===== Sub-object: FoleyFXCollider (12 bytes) ===== */

typedef struct FoleyFXCollider {
    int mSurfaceType;
    float mSpeed;
    int mFlags;
} FoleyFXCollider; /* 12 bytes */

/* ===== Sub-object: FoleyFXCollidee (4 bytes) ===== */

typedef struct FoleyFXCollidee {
    int mSurfaceType;
} FoleyFXCollidee; /* 4 bytes */

/* ===== Sub-object: CollisionObject_data (132 bytes) ===== */

typedef struct CollisionObject_data {
    TreeGridObject mTreeGridObject;                  /* +0x00, 36 bytes */
    FoleyFXCollider mFoleyFXCollider;                /* +0x24, 12 bytes */
    FoleyFXCollidee mFoleyFXCollidee;                /* +0x30, 4 bytes */
    Node mCollisionObjectNode;                       /* +0x34, 16 bytes */
    CollisionModel_full *mCollisionModel;            /* +0x44, 4 bytes */
    CollisionObject *mParent;                        /* +0x48, 4 bytes */
    float mMass;                                     /* +0x4C, 4 bytes */
    PblBox mAABB;                                    /* +0x50, 24 bytes */
    uchar mCollisionFlags;                           /* +0x68, 1 byte (bitfield: mProcedurallyAnimated:1, mLastCollidedWithNonTerrain:1) */
    uchar _pad_co[3];                                /* +0x69, 3 bytes padding */
    PblVector3 mLastPosition;                        /* +0x6C, 12 bytes */
    CollisionObject *mLastCollidedObjs[3];           /* +0x78, 12 bytes */
} CollisionObject_data; /* 132 bytes */

/* ===== Sub-object: DynDisplayable_data (32 bytes) ===== */

typedef struct DynDisplayable_data {
    Sector *sector_;
    uint flags_;
    int aframe_;
    int pframe_;
    float nextCheckDist_;
    PblVector3 lastCheckPos_;
} DynDisplayable_data; /* 32 bytes */

/* ===== Sub-object: RedSceneObject_data (52 bytes) ===== */

typedef struct RedSceneObject_data {
    RedSceneObject *mNextSceneObject;
    RedSceneObject *mPrevSceneObject;
    RedSceneAABox *mSceneAABox;
    PblSphere _Sphere;
    RedLodData *_pLodData;
    uint _uiRenderFlags;
    uchar _bActive;                                  /* bool */
    uchar _pad_rso[3];
    float _fPriorityMod;
    int _iFrameNum[1];
    uint _uiOcclusionIndex[1];
} RedSceneObject_data; /* 52 bytes */

/* ===== Sub-object: Damageable_data (188 bytes) ===== */

typedef struct Damageable_data {
    float mCurHealth;
    float mMaxHealth;
    float mAddHealth;
    float mCurShield;
    float mMaxShield;
    float mAddShield;
    float mDisableTime;
    float mAIDamageThreshold;
    DamageOwner mDamageOwner;                        /* 28 bytes */
    PblHandle_FLEffectObject mDamageEffect[10];      /* 80 bytes */
    GameSoundControllable mDamageEffectSound[10];    /* 40 bytes */
    float mDamageRegionTime;
    uchar mHealthFlags;                              /* bitfield: mHealthType:3, mIsAlive:1, mVanishing:1, mIsShielded:1 */
    uchar _pad_dmg[3];
} Damageable_data; /* 188 bytes */

/* ===== Sub-object: AIThread (24 bytes) ===== */

typedef struct AIThread {
    uchar _data[24];
} AIThread; /* 24 bytes */

/* ===== Sub-object: TargetInfo (28 bytes) ===== */

typedef struct TargetInfo {
    uchar _data[28];
} TargetInfo; /* 28 bytes */

/* ===== Sub-object: Controllable_data (336 bytes) ===== */

typedef struct Controllable_data {
    Trigger mControlFire[2];                         /* +0x00, 8 bytes */
    Trigger mControlReload;                          /* +0x08 */
    Trigger mControlJump;                            /* +0x0C */
    Trigger mControlCrouch;                          /* +0x10 */
    Trigger mControlSprint;                          /* +0x14 */
    Trigger mControlUse;                             /* +0x18 */
    Trigger mControlZoom;                            /* +0x1C */
    Trigger mControlView;                            /* +0x20 */
    Trigger mControlLockTarget;                      /* +0x24 */
    Trigger mControlSquadCommand;                    /* +0x28 */
    Trigger mControlThrustFwd;                       /* +0x2C */
    Trigger mControlThrustBack;                      /* +0x30 */
    Trigger mControlStrafeLeft;                      /* +0x34 */
    Trigger mControlStrafeRight;                     /* +0x38 */
    Trigger mTriggerControlSwitch[2];                /* +0x3C, 8 bytes */
    float mControlMove;                              /* +0x44 */
    float mControlStrafe;                            /* +0x48 */
    float mControlTurn;                              /* +0x4C */
    float mControlPitch;                             /* +0x50 */
    float mControlSwitch[2];                         /* +0x54 */
    int mWpnChannel;                                 /* +0x5C */
    AIThread mAIThread;                              /* +0x60, 24 bytes */
    Node mNode;                                      /* +0x78, 16 bytes */
    uchar _pad_node[4];                              /* +0x88, alignment */
    Controller *mCtrl;                               /* +0x8C */
    Character *mCharacter;                           /* +0x90 */
    Controllable *mPilot;                            /* +0x94 */
    int mPlayerId;                                   /* +0x98 */
    int mInitialPlayerId;                            /* +0x9C */
    PblVector3 mEyePoint;                            /* +0xA0, 12 bytes */
    PblVector3 mEyeDir;                              /* +0xAC, 12 bytes */
    float mTurnBuildup;                              /* +0xB8 */
    float mPitchBuildup;                             /* +0xBC */
    uchar mIndirectControlFlag;                      /* +0xC0 */
    uchar mUsingTurnAdjusted;                        /* +0xC1 */
    uchar mPlayAnim;                                 /* +0xC2 */
    uchar _pad_ctrl1;                                /* +0xC3 */
    uint mAnimNameId;                                /* +0xC4 */
    float mWpnDistFactor;                            /* +0xC8 */
    PblHandle_GameObject mCollisionObj;              /* +0xCC, 8 bytes */
    float mEnemyLockedOnMeState;                     /* +0xD4 */
    float mEnemyLockedOnMeTimestamp;                 /* +0xD8 */
    OrdnanceMissile *mEnemyLockedOnMeMissile;        /* +0xDC */
    float mEnemyLockedOnMeDistance;                  /* +0xE0 */
    float mTurnAdjusted;                             /* +0xE4 */
    float mPitchAdjusted;                            /* +0xE8 */
    float mTurnAuto;                                 /* +0xEC */
    float mPitchAuto;                                /* +0xF0 */
    float mLockBreakTimer;                           /* +0xF4 */
    float mInCaptureRegionTimestamp;                  /* +0xF8 */
    PblHandle_GameObject mTargetLockedObj;           /* +0xFC, 8 bytes */
    int mAllowedLockTypes;                           /* +0x104 */
    int mPilotType;                                  /* +0x108 */
    TargetInfo mTargetInfo;                          /* +0x10C, 28 bytes */
    PblHandle_GameObject_const mReticuleTarget[2];   /* +0x128, 16 bytes */
    PblHandle_GameObject_const mAIReticuleTarget[2]; /* +0x138, 16 bytes */
    int mReticuleAffiliation[2];                     /* +0x148, 8 bytes */
} Controllable_data; /* 336 bytes */

/* ===== Sub-object: Thread_data (20 bytes) ===== */

typedef struct Thread_data {
    uchar mThreadListNode[20];                       /* Node (20 bytes in this variant) */
} Thread_data; /* 20 bytes */

/* ===== Sub-object: Trackable_data (32 bytes) ===== */

typedef struct Trackable_data {
    int whichCornerToCastForCamera;
    float minCameraCastDist[5];
    Tracker *mTracker;
    uchar _pad_trk[4];
} Trackable_data; /* 32 bytes */

/* ===== Sub-object: PlayableEntitySounds_data (4 bytes) ===== */

typedef struct PlayableEntitySoundsClass PlayableEntitySoundsClass;

typedef struct PlayableEntitySounds_data {
    PlayableEntitySoundsClass *mPESClass;
} PlayableEntitySounds_data; /* 4 bytes */

/* ===== Sub-object: SoundParameterized (40 bytes) ===== */

typedef struct SoundParameterized {
    uchar _data[40];
} SoundParameterized; /* 40 bytes */

/* ===== Sub-object: VehicleEngine (80 bytes) ===== */

typedef struct VehicleEngine {
    uchar _pad[4];                                   /* +0x00, vtable or padding */
    SoundParameterized mEngine;                      /* +0x04, 40 bytes */
    GameSoundControllable mAttached;                 /* +0x2C, 4 bytes */
    GameSound mAttachedProps;                        /* +0x30, 8 bytes */
    GameSoundControllable mBoostController;          /* +0x38, 4 bytes */
    GameSound mBoostProps;                           /* +0x3C, 8 bytes */
    float mPrevSpeed;                                /* +0x44 */
    float mTimer;                                    /* +0x48 */
    ushort mParams;                                  /* +0x4C */
    uchar mFlags;                                    /* +0x4E, bitfield */
    uchar _pad2;                                     /* +0x4F */
} VehicleEngine; /* 80 bytes */

/* ===== Sub-object: Aimer_full (532 bytes) ===== */
/* Note: PDB says 544, but EntitySoldier embeds 532 bytes of Aimer data */

typedef struct Aimer_full {
    void *_vtable;                                   /* +0x00 */
    uchar _data1[20];                                /* +0x04 */
    void *mNextAimer;                                /* +0x18 */
    AimerClass *mClass;                              /* +0x1C */
    Controllable *mOwner;                            /* +0x20 */
    PblMatrix *mParentMatrix;                        /* +0x24 */
    uchar mWithinLimitsFlag;                         /* +0x28 */
    uchar bDirect;                                   /* +0x29 */
    uchar _pad1[2];                                  /* +0x2A */
    PblVector3 mOffsetPos;                           /* +0x2C */
    float mPitch;                                    /* +0x38 */
    float mOmegaPitch;                               /* +0x3C */
    float mYaw;                                      /* +0x40 */
    float mOmegaYaw;                                 /* +0x44 */
    PblVector3 mDirection;                           /* +0x48 */
    PblVector3 mMountPos;                            /* +0x54 */
    PblQuaternion mMountRot;                         /* +0x60 */
    PblVector3 mRootPos;                             /* +0x70 */
    PblVector3 mOldRootPos;                          /* +0x7C */
    PblVector3 mFirePos;                             /* +0x88 */
    PblVector3 mPrevFirePos;                         /* +0x94 */
    PblVector3 mAngVel;                              /* +0xA0 */
    uchar _pad2[4];                                  /* +0xAC */
    PblMatrix mMountPoseMatrix;                      /* +0xB0, 64 bytes */
    PblMatrix mBarrelPoseMatrix[4];                  /* +0xF0, 256 bytes */
    RedPose *mPose;                                  /* +0x1F0 */
    float mRecoil[4];                                /* +0x1F4 */
    int mCurrentBarrel;                              /* +0x204 */
    Weapon *mWeapon;                                 /* +0x208 */
    uchar mSkip;                                     /* +0x20C */
    uchar _pad3[3];                                  /* +0x20D */
    float mSkipTime;                                 /* +0x210 */
    uchar _tail[4];                                  /* +0x214, remaining bytes */
} Aimer_full; /* 532 bytes; padded to fit within EntitySoldier at 0x644-0x858 */

/* ===== Sub-object: PostCollision (24 bytes) ===== */

typedef struct PostCollision {
    uchar _data[24];
} PostCollision; /* 24 bytes */

/* ===== Sub-object: CollisionBodyPrimitive_full (100 bytes) ===== */

typedef struct CollisionBodyPrimitive_full {
    uchar _data[100];
} CollisionBodyPrimitive_full; /* 100 bytes */

/* ===== Sub-object: CollisionModel_embedded (332 bytes) ===== */

typedef struct CollisionModel_embedded {
    uchar _data[332];
} CollisionModel_embedded; /* 332 bytes */

/* ===== Sub-object: CollisionMeshCache (316 bytes) ===== */

typedef struct CollisionMeshCache {
    uchar _data[316];
} CollisionMeshCache; /* 316 bytes */

/* ===== Sub-object: EntityCloth ===== */

typedef struct EntityCloth EntityCloth;

/* ===========================================================================
 * EntitySoldier — FULL STRUCT (0xFC8 = 4040 bytes)
 *
 * Usage: Apply to struct_base pointers.
 * entity = struct_base + 0x240 (Controllable vtable5 offset)
 * =========================================================================== */

struct EntitySoldier {

    /* ---- EntityEx (base at +0x000) ---- */
    void *vftable0_EntityEx;                         /* +0x000 */
    uint mId;                                        /* +0x004 */
    EntityClass *mEntityClass;                       /* +0x008 */

    /* ---- CollisionObject (base at +0x00C) ---- */
    void *vftable1_CollisionObject;                  /* +0x00C */
    CollisionObject_data mCollisionObjectData;       /* +0x010, 132 bytes */

    /* ---- RedSceneObject (base at +0x094) ---- */
    void *vftable2_RedSceneObject;                   /* +0x094 */
    DynDisplayable_data mDynDisplayableData;         /* +0x098, 32 bytes */
    RedSceneObject_data mRedSceneObjectData;         /* +0x0B8, 52 bytes */

    /* ---- EntityGeometry fields ---- */
    uchar _pad_geom[4];                              /* +0x0EC, unknown bytes */
    PblMatrix mMatrix;                               /* +0x0F0, 64 bytes (world matrix) */
    GameModel *mModel;                               /* +0x130 */
    float mSmallestCollisionDim;                     /* +0x134 */
    AttachedEffects *m_pAttachedEffects;              /* +0x138 */
    uchar _pad_geom2[4];                             /* +0x13C */

    /* ---- Damageable (base at +0x140) ---- */
    void *vftable3_Damageable;                       /* +0x140 */
    Damageable_data mDamageableData;                 /* +0x144, 188 bytes */

    /* ---- PblHandled (base at +0x200) ---- */
    void *vftable4_PblHandled;                       /* +0x200 */
    int mHandleId;                                   /* +0x204 */

    /* ---- GameObject fields ---- */
    uchar mControllableList[20];                     /* +0x208, PblList<Controllable> */
    int mNetUniqueId;                                /* +0x21C */
    void *mPrevFoliageObj;                           /* +0x220 */
    GameSoundControllable mGameObjectAttachedSound;  /* +0x224 */
    PblHandle_FLEffectObject mDisabledEffectHandle;  /* +0x228, 8 bytes */
    float mCreatedTime;                              /* +0x230 */
    uchar mTeamByte0;                                /* +0x234, bitfield: mTeam:4, mPerceivedTeam:4 */
    uchar mTeamByte1;                                /* +0x235, bitfield: mOwningTeam:4, mHealthTypeForLockon:4 */
    uchar _pad_go1[2];                               /* +0x236 */
    uchar mIsTargetable;                             /* +0x238, bool:1 */
    uchar _pad_go2[7];                               /* +0x239 */

    /* ---- Controllable (base at +0x240) ---- */
    /* NOTE: entity pointer = struct_base + 0x240 = &vftable5_Controllable */
    void *vftable5_Controllable;                     /* +0x240 */
    Thread_data mThreadData;                         /* +0x244, 20 bytes */

    void *vftable6_Trackable;                        /* +0x258 */
    Trackable_data mTrackableData;                   /* +0x25C, 32 bytes */
    Controllable_data mControllableData;             /* +0x27C, 336 bytes */

    /* ---- PlayableEntitySounds (base at +0x3CC) ---- */
    void *vftable7_PlayableEntitySounds;             /* +0x3CC */
    PlayableEntitySounds_data mPESData;              /* +0x3D0, 4 bytes */

    /* ---- VehicleEngine ---- */
    VehicleEngine mVehicleEngine;                    /* +0x3D4, 80 bytes */

    /* ==== EntitySoldier-specific data (base at +0x424) ==== */

    PblHandle_GameObject mStandingOnObj;             /* +0x424, 8 bytes */
    int mStandingOnBody;                             /* +0x42C */
    PblVector3 mStandingOnObjLastVelocity;           /* +0x430, 12 bytes */
    AcklayData *mAcklayData;                         /* +0x43C */
    EntitySoldierClass *mClass;                      /* +0x440 (= mEntityClass cast) */
    TentacleSimulator *mTentacles;                   /* +0x444 */
    OrdnanceGrapplingHook *mHook;                    /* +0x448 */
    PblVector3 mGroundNormal;                        /* +0x44C, 12 bytes */
    PblVector3 mAILastCollisionNormal;               /* +0x458, 12 bytes */
    PblVector3 mAvgFlyGroundCollisionNormal;         /* +0x464, 12 bytes */
    uchar mNumFlyGroundCollisions;                   /* +0x470 */
    uchar mInputFlags0;                              /* +0x471, bitfield: mAILastCollisionDirection:2, m_uiInputLockMask:3, m_bSlide:1, mAttachedToProp:1, mFlyingInFirstPerson:1 */
    uchar mInputFlags1;                              /* +0x472, bitfield: m_bPrimaryWeaponIsMelee:1, mCanChangeClass:1, mLastRenderedLod:2, mMarkedForDeath:1, z_uiReserved0:1, mSkip:1, mDoProneRotation:1 */
    uchar mTotalProneCollisions;                     /* +0x473 */
    float mInputLockTime;                            /* +0x474 */
    uchar _pad_sd1[8];                               /* +0x478, unknown gap */
    PblMatrix mLastVehicleMatrix;                    /* +0x480, 64 bytes */
    PostCollision mPostCollision;                    /* +0x4C0, 24 bytes */
    PblVector3 mVelocity;                            /* +0x4D8, 12 bytes (= struct_base+0x4EC from entity) */
    float mPitchValue;                               /* +0x4E4 [NOTE: +0x4EC is also velocity from entity perspective] */
    float mPitchOffset;                              /* +0x4E8 */
    float mPitchVelocity;                            /* +0x4EC */
    float mKickScale;                                /* +0x4F0 */
    float mKickBuild;                                /* +0x4F4 */
    float mTurnAngle;                                /* +0x4F8 */
    Aimer_full mAimer;                               /* +0x4FC, 532 bytes */
    Weapon *mWeapon[8];                              /* +0x710, 32 bytes (= entity+0x4D0) */
    uchar mWeaponIndex[2];                           /* +0x730, char[2] */
    uchar mActiveIndex;                              /* +0x732, bitfield: activeIndex:4, activeChannel:2, stanceIndex:2 */
    uchar m_uiInvisibilityAlpha;                     /* +0x733 */
    SoldierState mState;                             /* +0x734, 4 bytes */
    SoldierState mOldState;                          /* +0x738, 4 bytes */
    float mStateTimer;                               /* +0x73C */
    SoldierAnimator *mSoldierAnimator;               /* +0x740 */
    PblVector3 m_vChokeDir;                          /* +0x744, 12 bytes */
    float mAlertTimer;                               /* +0x750 */
    PblVector3 m_vLastPosition;                      /* +0x754, 12 bytes */
    float mLegAngle;                                 /* +0x760 */
    uchar _pad_sd2[12];                              /* +0x764, gap to collision bodies */
    CollisionBodyPrimitive_full mCollisionBodyHead;  /* +0x770, 100 bytes */
    CollisionBodyPrimitive_full mCollisionBodyTorso; /* +0x7D4, 100 bytes */
    CollisionModel_embedded mDynamicCollisionModel;  /* +0x838, 332 bytes */
    GameModel *mModelLowRes;                         /* +0x984 */
    SoldierAnimatorLowRes *mSoldierAnimatorLowRes;   /* +0x988 */
    uchar mFlags;                                    /* +0x98C */
    uchar mFlags2;                                   /* +0x98D, bitfield: z_uiReserved1:3, mLegAngleForward:1, mMoveAnim:2, m_uiLastDamageDir:2 */
    ushort mSoundState;                              /* +0x98E */
    float mSkipTime;                                 /* +0x990 */
    float mSkipIndirectTime;                         /* +0x994 */
    float mPrevZoom;                                 /* +0x998 */
    float mSquadCommandClearTimer;                   /* +0x99C */
    ANIMATION m_ePilotAnimation;                     /* +0x9A0 */
    MAP m_eSoldierAnimationMap;                      /* +0x9A4 */
    uchar m_uiWeaponAnimIndex;                       /* +0x9A8 */
    uchar _pad_sd3[3];                               /* +0x9A9 */
    float m_fWeaponAnimTimer;                        /* +0x9AC */
    uint mAnimationHash;                             /* +0x9B0 */
    float mForceFrameNum1;                           /* +0x9B4 */
    float mForceFrameNum2;                           /* +0x9B8 */
    float mForceFrameBlend;                          /* +0x9BC */
    float mLeftFootElevation;                        /* +0x9C0 */
    float mRightFootElevation;                       /* +0x9C4 */
    EnergyBar mEnergyBar;                            /* +0x9C8, 12 bytes */
    int mCameraSettingPrev;                           /* +0x9D4 */
    float mCameraSettingTimer;                       /* +0x9D8 */
    float mDrownTime;                                /* +0x9DC */
    float mFoliageCollisionTime;                     /* +0x9E0 */
    GameSoundControllable mLowHealthSound;           /* +0x9E4, 4 bytes */
    GameSoundControllable mLongFoleySound;           /* +0x9E8, 4 bytes */
    GameSoundControllable mAmbientSound;             /* +0x9EC, 4 bytes */
    GameSoundControllable mChokeSound;               /* +0x9F0, 4 bytes */
    GameSound mCurFoleySound;                        /* +0x9F4, 8 bytes */
    float mSpeedPrev;                                /* +0x9FC */
    float mDistanceMoved;                            /* +0xA00 */
    PblHandle_FLEffectObject mJetEffectHandle;       /* +0xA04, 8 bytes */
    PblHandle_FLEffectObject mJetIdleEffectHandle;   /* +0xA0C, 8 bytes */
    PblHandle_FLEffectObject mHealthGainEffectHandle;/* +0xA14, 8 bytes */
    PblHandle_FLEffectObject mAmmoGainEffectHandle;  /* +0xA1C, 8 bytes */
    PblHandle_FLEffectObject mEnergyGainEffectHandle;/* +0xA24, 8 bytes */
    PblHandle_FLEffectObject mBuffHealthEffectHandle;/* +0xA2C, 8 bytes */
    PblHandle_FLEffectObject mBuffOffenseEffectHandle;/* +0xA34, 8 bytes */
    PblHandle_FLEffectObject mBuffDefenseEffectHandle;/* +0xA3C, 8 bytes */
    PblHandle_FLEffectObject mDebuffEffectHandle;    /* +0xA44, 8 bytes */
    PblHandle_FLEffectObject mInvincibleEffectHandle;/* +0xA4C, 8 bytes */
    PblHandle_FLEffectObject mSmolderEffectHandle[5];/* +0xA54, 40 bytes */
    int mSmolderEffectBone[5];                       /* +0xA7C, 20 bytes */
    int mSmolderEffect[5];                           /* +0xA90, 20 bytes */
    uint mLastPilotInfoFrame;                        /* +0xAA4 */
    float mJetFuel;                                  /* +0xAA8 */
    PblVector3 m_vPosFootRight;                      /* +0xAAC, 12 bytes */
    PblVector3 m_vPosFootLeft;                       /* +0xAB8, 12 bytes */
    PblHandle_FLEffectObject mFootSlideEffectRightHandle; /* +0xAC4, 8 bytes */
    PblHandle_FLEffectObject mFootSlideEffectLeftHandle;  /* +0xACC, 8 bytes */
    PblHandle_FLEffectObject mChokeEffectHandle;     /* +0xAD4, 8 bytes */
    PblMatrix mSafeMatrix;                           /* +0xADC, 64 bytes */
    CollisionState mCollisionState;                  /* +0xB1C, 4 bytes */
    CollisionMeshCache mCollisionMeshCache[3];       /* +0xB20, 948 bytes */
    float mBuffOffenseTimer;                         /* +0xEF4 */
    float mBuffOffenseMult;                          /* +0xEF8 */
    float mBuffDefenseTimer;                         /* +0xEFC */
    float mBuffDefenseMult;                          /* +0xF00 */
    float mBuffHealthTimer;                          /* +0xF04 */
    float mBuffHealthRate;                           /* +0xF08 */
    float mDebuffDamageTimer;                        /* +0xF0C */
    float mDebuffDamageRate;                         /* +0xF10 */
    DamageOwner mDebuffDamageOwner;                  /* +0xF14, 28 bytes */
    float mSmolderTimer;                             /* +0xF30 */
    uchar mSmolderVanishDeath;                       /* +0xF34 */
    uchar _pad_sd4[3];                               /* +0xF35 */
    float mSmolderDamageRate;                        /* +0xF38 */
    DamageOwner mSmolderDamageOwner;                 /* +0xF3C, 28 bytes */
    Controllable *mPrevRemoteControllable;           /* +0xF58 */
    PblHandle_GameObject mPrevRemoteGameObj;         /* +0xF5C, 8 bytes */
    GameMusic mSoldierMusic;                         /* +0xF64, 12 bytes */
    GameMusic mAISoldierMusic;                       /* +0xF70, 12 bytes */
    uchar mNetJustChangedState;                      /* +0xF7C */
    uchar _pad_sd5[3];                               /* +0xF7D */
    EntityCloth *mClothPiece[8];                     /* +0xF80, 32 bytes */
    uchar _pad_tail[40];                             /* +0xFA0, pad to 0xFC8 */
};

/* Static assert to verify total size */
typedef char _check_EntitySoldier_size[sizeof(struct EntitySoldier) == 0xFC8 ? 1 : -1];

#pragma pack()
