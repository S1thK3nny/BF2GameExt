// Auto-generated EntityFlyer vftable definitions
// Generated from: BF2_modtools_MemExt.exe

/* EntityFlyer_vftable0 - Entity (primary) */
/* Vtable at: 0x00A3CDC8, 72 entries */
struct EntityFlyer_vftable0 {
    bool (__thiscall *IsRtti)(void * this, int param_1);  /* [ 0] 0x004166A3 */
    undefined4 (__thiscall *GetDerivedRtti)(void * this);  /* [ 1] 0x0040D32D */
    undefined (__thiscall *GetDerivedRttiName)(void * this);  /* [ 2] 0x0040A99D */
    int * * (__thiscall *thunk_FUN_004f66f0)(void * this, byte param_1);  /* [ 3] 0x0040BFD2 */
    void (__thiscall *SetProperty)(void * this, uint param_1, char * param_2);  /* [ 4] 0x00401ACD */
    void (__thiscall *Init)(void * this);  /* [ 5] 0x0040971E */
    undefined4 (__thiscall *thunk_FUN_004d77d0)(void * this, undefined4 param_1);  /* [ 6] 0x00413FA7 */
    undefined4 (__thiscall *thunk_FUN_004d77c0)(void * this, undefined4 param_1);  /* [ 7] 0x00416739 */
    undefined4 (__thiscall *thunk_FUN_004d77b0)(void * this, undefined4 param_1);  /* [ 8] 0x00408D0A */
    undefined4 (__thiscall *thunk_FUN_004d77a0)(void * this, undefined4 param_1);  /* [ 9] 0x00402AD1 */
    EntityClass * (__thiscall *GetEntityClass)(void * this);  /* [10] 0x00406514 */
    EntityClass * (__thiscall *GetEntityClass_1)(void * this);  /* [11] 0x00414AE2 */
    CollisionObject * (__thiscall *GetCollisionObject)(void * this);  /* [12] 0x0040880A */
    CollisionObject * (__thiscall *GetCollisionObject_1)(void * this);  /* [13] 0x0040E9AD */
    GameModel * (__thiscall *GetModel)(void * this);  /* [14] 0x00403FCB */
    void (__thiscall *SetModel)(void * this, GameModel * param_1);  /* [15] 0x00408481 */
    void (__thiscall *SetupPose)(void * this, RedPose * param_1);  /* [16] 0x00405C09 */
    undefined (__thiscall *thunk_FUN_004d7060)(void * this);  /* [17] 0x00407D9C */
    undefined (__thiscall *thunk_FUN_004d77f0)(void * this);  /* [18] 0x00409507 */
    int (__thiscall *GetTargetBodyID)(void * this, PblVector3 * param_1, PblVector3 * param_2);  /* [19] 0x00402A9A */
    PblVector3 * (__thiscall *GetTargetPoint)(void * this, PblVector3 * param_1, PblVector3 * param_2, PblVector3 * param_3, int param_4);  /* [20] 0x00407A40 */
    PblVector3 * (__thiscall *GetTargetPointMissVector)(void * this, PblVector3 * param_1);  /* [21] 0x00406DFC */
    undefined (__thiscall *thunk_FUN_004f2d70)(void * this, int param_1);  /* [22] 0x004068F2 */
    bool (__thiscall *IsUnbuilt)(void * this);  /* [23] 0x0041507D */
    float (__thiscall *GetMaxSpeed)(void * this);  /* [24] 0x00406AF0 */
    float (__thiscall *GetMaxStrafe)(void * this);  /* [25] 0x004122A1 */
    undefined (__thiscall *thunk_FUN_004bee50)(void * this);  /* [26] 0x00407040 */
    int (__thiscall *thunk_FUN_004d7860)(void * this, int param_1);  /* [27] 0x0040F425 */
    int (__thiscall *thunk_FUN_004d7840)(void * this, int param_1);  /* [28] 0x0040DF80 */
    undefined (__thiscall *thunk_FUN_004bee90)(void * this);  /* [29] 0x00416B99 */
    undefined4 (__thiscall *thunk_FUN_004bee80)(void * this);  /* [30] 0x00404BBA */
    undefined (__thiscall *thunk_FUN_004beea0)(void * this);  /* [31] 0x00413462 */
    bool (__thiscall *ShouldShowReticule)(void * this);  /* [32] 0x0040E65B */
    void (__thiscall *ApplyRadiusDamage)(void * this, PblVector3 * param_1, float param_2, float param_3, DamageDesc * param_4);  /* [33] 0x0040E09D */
    undefined (__thiscall *thunk_FUN_004beec0)(void * this);  /* [34] 0x0041696E */
    undefined (__thiscall *thunk_FUN_004beed0)(void * this);  /* [35] 0x00414600 */
    void (__thiscall *SetTeam)(void * this, uint param_1);  /* [36] 0x00416414 */
    bool (__thiscall *IsVisibleToMe)(void * this, GameObject * param_1);  /* [37] 0x0040BDED */
    undefined (__thiscall *thunk_FUN_004beee0)(void * this);  /* [38] 0x00408D64 */
    undefined (__thiscall *thunk_FUN_004f54f0)(void * this);  /* [39] 0x0040BFEB */
    undefined (__thiscall *thunk_FUN_004bef00)(void * this);  /* [40] 0x00404B5B */
    void (__thiscall *ActivatePhysics)(void * this);  /* [41] 0x00416BD0 */
    void (__thiscall *DeactivatePhysics)(void * this);  /* [42] 0x00411BA8 */
    undefined (__thiscall *thunk_FUN_004d78f0)(void * this);  /* [43] 0x00403EDB */
    undefined (__thiscall *thunk_FUN_004bef30)(void * this);  /* [44] 0x0040C8D8 */
    undefined (__thiscall *thunk_FUN_004bef40)(void * this);  /* [45] 0x0041574E */
    undefined (__thiscall *thunk_FUN_004bef50)(void * this);  /* [46] 0x00411400 */
    undefined (__thiscall *thunk_FUN_004bef60)(void * this);  /* [47] 0x0041510E */
    undefined (__thiscall *thunk_FUN_004bef70)(void * this);  /* [48] 0x00405394 */
    undefined (__thiscall *thunk_FUN_004bef80)(void * this);  /* [49] 0x004107AD */
    undefined (__thiscall *thunk_FUN_004bef90)(void * this);  /* [50] 0x004078DD */
    undefined (__thiscall *thunk_FUN_004d79d0)(void * this, int param_1_00, float param_2);  /* [51] 0x0041749F */
    undefined4 (__thiscall *thunk_FUN_004d79f0)(void * this, int param_1);  /* [52] 0x0040E80E */
    undefined (__thiscall *thunk_FUN_004befc0)(void * this);  /* [53] 0x00408F62 */
    undefined (__thiscall *thunk_FUN_004d79c0)(void * this);  /* [54] 0x004094DA */
    undefined4 (__thiscall *GetExplosionClass)(void * this);  /* [55] 0x0041151D */
    undefined (__thiscall *thunk_FUN_004d1190)(void * this);  /* [56] 0x00415ACD */
    undefined (__thiscall *thunk_FUN_004d7a40)(void * this);  /* [57] 0x0040D215 */
    undefined (__thiscall *thunk_FUN_004d7a20)(void * this);  /* [58] 0x0040388C */
    undefined (__thiscall *thunk_FUN_004d7a60)(void * this);  /* [59] 0x004010FA */
    undefined (__thiscall *thunk_FUN_0071d460)(void * this);  /* [60] 0x0041652C */
    undefined (__thiscall *thunk_FUN_0071d470)(void * this);  /* [61] 0x00408C38 */
    undefined (__thiscall *thunk_FUN_00726650)(void * this);  /* [62] 0x0040B2A8 */
    undefined (__thiscall *thunk_FUN_00726fe0)(void * this, int * param_1_00, uint param_2, undefined4 param_3);  /* [63] 0x0040939F */
    undefined (__thiscall *thunk_FUN_0071d4a0)(void * this);  /* [64] 0x00407AAE */
    undefined (__thiscall *thunk_FUN_0071d4b0)(void * this);  /* [65] 0x004068F7 */
    uint (__thiscall *thunk_FUN_0071d4c0)(void * this, uint param_1);  /* [66] 0x0040A72C */
    undefined (__thiscall *thunk_FUN_00726500)(void * this);  /* [67] 0x00414358 */
    undefined (__thiscall *thunk_FUN_0071d930)(void * this);  /* [68] 0x00414894 */
    undefined (__thiscall *thunk_FUN_0072c4b0)(void * this);  /* [69] 0x0040233D */
    undefined (__thiscall *thunk_FUN_0072c9f0)(void * this, int param_1_00, uint param_2);  /* [70] 0x004054D4 */
    bool (__thiscall *PostCollisionUpdate)(void * this, float param_1);  /* [71] 0x0040C6D5 */
};

/* EntityFlyer_vftable1 - CollisionObject */
/* Vtable at: 0x00A3CD68, 20 entries */
struct EntityFlyer_vftable1 {
    void * (__thiscall *func_00408CCE)(void * this);  /* [ 0] 0x00408CCE */
    PblVector3 * (__thiscall *GetPosition)(void * this);  /* [ 1] 0x0040A4F2 */
    void * (__thiscall *func_00407103)(void * this);  /* [ 2] 0x00407103 */
    void * (__thiscall *func_00403A7B)(void * this);  /* [ 3] 0x00403A7B */
    undefined (__thiscall *thunk_FUN_00436730)(void * this);  /* [ 4] 0x004166E4 */
    float10 (__thiscall *GetSmallestCollisionDim)(void * this, EntityGeometry * param_1);  /* [ 5] 0x00415AC3 */
    void * (__thiscall *func_00407469)(void * this);  /* [ 6] 0x00407469 */
    float10 (__thiscall *thunk_FUN_00435e50)(void * this, int * param_1_00, undefined4 param_2, undefined4 param_3, float param_4, undefined4 * param_5, int * param_6, char param_7, undefined4 param_8);  /* [ 7] 0x004013CF */
    undefined4 (__thiscall *thunk_FUN_00435ec0)(void * this, int * param_1_00, undefined4 param_2, undefined4 param_3, undefined4 param_4);  /* [ 8] 0x004171C5 */
    undefined (__thiscall *thunk_FUN_00435f10)(void * this, int * param_1_00, int * param_2, undefined4 param_3, uint param_4);  /* [ 9] 0x004076A8 */
    undefined (__thiscall *thunk_FUN_00436740)(void * this);  /* [10] 0x004029E6 */
    void * (__thiscall *func_004141D2)(void * this);  /* [11] 0x004141D2 */
    void * (__thiscall *func_00412A08)(void * this);  /* [12] 0x00412A08 */
    void * (__thiscall *func_0040EE62)(void * this);  /* [13] 0x0040EE62 */
    void * (__thiscall *func_004143E9)(void * this);  /* [14] 0x004143E9 */
    undefined (__thiscall *thunk_FUN_00436790)(void * this);  /* [15] 0x004100C3 */
    int (__thiscall *thunk_FUN_004d77e0)(void * this, int param_1);  /* [16] 0x004035A8 */
    void * (__thiscall *func_0040952F)(void * this);  /* [17] 0x0040952F */
    undefined (__thiscall *thunk_FUN_004367b0)(void * this);  /* [18] 0x0040794B */
    undefined (__thiscall *thunk_FUN_00436810)(void * this, int param_1);  /* [19] 0x004063A7 */
};

/* EntityFlyer_vftable2 - RedSceneObject */
/* Vtable at: 0x00A3CCF0, 17 entries */
struct EntityFlyer_vftable2 {
    void (__thiscall *OnTraverse)(void * this);  /* [ 0] 0x0041323C */
    void (__thiscall *SetActive)(void * this, undefined4 param_1);  /* [ 1] 0x00404818 */
    bool (__thiscall *IsActive)(void * this, int param_1);  /* [ 2] 0x004156D6 */
    void (__thiscall *SetProcessed)(void * this, int param_1);  /* [ 3] 0x00402F4A */
    bool (__thiscall *IsProcessed)(void * this, int param_1);  /* [ 4] 0x0040E79B */
    void (__thiscall *GetAABound)(void * this);  /* [ 5] 0x004148D5 */
    void (__thiscall *GetSphereBound)(void * this, Sphere * param_1);  /* [ 6] 0x00408D2D */
    uint (__thiscall *PortalClip)(void * this);  /* [ 7] 0x0040A5B5 */
    void (__thiscall *SetPortalClip)(void * this, bool param_1);  /* [ 8] 0x004098AE */
    uint (__thiscall *PortalActive)(void * this);  /* [ 9] 0x00404223 */
    void (__thiscall *SetPortalActive)(void * this, bool param_1);  /* [10] 0x00404CCD */
    uint (__thiscall *Dynamic)(void * this);  /* [11] 0x00410C8F */
    void (__thiscall *SetDynamic)(void * this, bool param_1);  /* [12] 0x00401A64 */
    Sector * (__thiscall *GetSector)(void * this);  /* [13] 0x0040A637 */
    undefined (__thiscall *SetSector)(void * this, int param_1);  /* [14] 0x007F4990 */
    void (__thiscall *Update)(void * this);  /* [15] 0x007F5810 */
    void * (__thiscall *func_004043EA)(void * this);  /* [16] 0x004043EA */
};

/* EntityFlyer_vftable3 - Damageable */
/* Vtable at: 0x00A3CCC0, 10 entries */
struct EntityFlyer_vftable3 {
    void * (__thiscall *func_0041550A)(void * this);  /* [ 0] 0x0041550A */
    undefined (__thiscall *thunk_FUN_004f2c40)(void * this, int param_1);  /* [ 1] 0x0040EA48 */
    void (__thiscall *Respawn)(void * this);  /* [ 2] 0x004053B7 */
    void * (__thiscall *func_004056C3)(void * this);  /* [ 3] 0x004056C3 */
    void * (__thiscall *func_00408611)(void * this);  /* [ 4] 0x00408611 */
    uint (__thiscall *ApplyDamage)(void * this, float * param_1, int param_2, undefined4 param_3, undefined4 param_4);  /* [ 5] 0x0041040B */
    void (__thiscall *InstantDeath)(void * this, DamageOwner * param_1, bool param_2);  /* [ 6] 0x0040EEA8 */
    float (__thiscall *GetDamageMultiplier)(void * this, int param_1);  /* [ 7] 0x00413183 */
    void * (__thiscall *func_0040EDC7)(void * this);  /* [ 8] 0x0040EDC7 */
    void (__thiscall *ShieldChangeCallback)(void * this, float param_1);  /* [ 9] 0x0040FCE5 */
};

/* EntityFlyer_vftable4 - PblHandled */
/* Vtable at: 0x00A3CCBC, 1 entries */
struct EntityFlyer_vftable4 {
    void * (__thiscall *func_00411A5E)(void * this);  /* [ 0] 0x00411A5E */
};

/* EntityFlyer_vftable5 - Controllable::Thread */
/* Vtable at: 0x00A3CBC8, 51 entries */
struct EntityFlyer_vftable5 {
    void * (__thiscall *func_0040C793)(void * this);  /* [ 0] 0x0040C793 */
    bool (__thiscall *Update)(void * this, float param_2);  /* [ 1] 0x00412AD0 */
    void (__thiscall *ActivateThread)(void * this, int param_1, ThreadType param_2);  /* [ 2] 0x00415587 */
    void (__thiscall *DeactivateThread)(void * this);  /* [ 3] 0x00405EF2 */
    bool (__thiscall *IsThreadActive)(void * this);  /* [ 4] 0x00414880 */
    void * (__thiscall *func_00403F8F)(void * this);  /* [ 5] 0x00403F8F */
    void * (__thiscall *func_0041187E)(void * this);  /* [ 6] 0x0041187E */
    void * (__thiscall *func_004022B6)(void * this);  /* [ 7] 0x004022B6 */
    void * (__thiscall *func_00410BD1)(void * this);  /* [ 8] 0x00410BD1 */
    undefined (__thiscall *thunk_FUN_004d7600)(void * this);  /* [ 9] 0x00405AA6 */
    void * (__thiscall *func_0040B1DB)(void * this);  /* [10] 0x0040B1DB */
    void * (__thiscall *func_00410F3C)(void * this);  /* [11] 0x00410F3C */
    void * (__thiscall *func_00403779)(void * this);  /* [12] 0x00403779 */
    void * (__thiscall *func_004052D1)(void * this);  /* [13] 0x004052D1 */
    void * (__thiscall *func_00415393)(void * this);  /* [14] 0x00415393 */
    void * (__thiscall *func_00408AF8)(void * this);  /* [15] 0x00408AF8 */
    void * (__thiscall *func_0041424F)(void * this);  /* [16] 0x0041424F */
    void * (__thiscall *func_004168F1)(void * this);  /* [17] 0x004168F1 */
    void * (__thiscall *func_00410708)(void * this);  /* [18] 0x00410708 */
    void * (__thiscall *func_00414D53)(void * this);  /* [19] 0x00414D53 */
    void * (__thiscall *func_00402117)(void * this);  /* [20] 0x00402117 */
    void * (__thiscall *func_0040397C)(void * this);  /* [21] 0x0040397C */
    void * (__thiscall *func_004066A9)(void * this);  /* [22] 0x004066A9 */
    void * (__thiscall *func_0040A8AD)(void * this);  /* [23] 0x0040A8AD */
    void * (__thiscall *func_0040EF43)(void * this);  /* [24] 0x0040EF43 */
    void * (__thiscall *func_0040C2A7)(void * this);  /* [25] 0x0040C2A7 */
    void * (__thiscall *func_0040E610)(void * this);  /* [26] 0x0040E610 */
    void * (__thiscall *func_0040C400)(void * this);  /* [27] 0x0040C400 */
    void * (__thiscall *func_0040991C)(void * this);  /* [28] 0x0040991C */
    void * (__thiscall *func_0040BFF0)(void * this);  /* [29] 0x0040BFF0 */
    void * (__thiscall *func_00411950)(void * this);  /* [30] 0x00411950 */
    void * (__thiscall *func_00410500)(void * this);  /* [31] 0x00410500 */
    undefined (__thiscall *thunk_FUN_005e0890)(void * this, int * param_1_00, int param_2);  /* [32] 0x00402B67 */
    void * (__thiscall *func_00401EBA)(void * this);  /* [33] 0x00401EBA */
    undefined (__thiscall *thunk_FUN_005dffe0)(void * this, int param_1_00, undefined4 param_2, undefined4 * param_3);  /* [34] 0x00410B3B */
    undefined (__thiscall *thunk_FUN_004d76b0)(void * this);  /* [35] 0x00407DDD */
    void * (__thiscall *func_0040A3CB)(void * this);  /* [36] 0x0040A3CB */
    void * (__thiscall *func_0041297C)(void * this);  /* [37] 0x0041297C */
    void * (__thiscall *func_004054E8)(void * this);  /* [38] 0x004054E8 */
    void * (__thiscall *func_00413E94)(void * this);  /* [39] 0x00413E94 */
    void * (__thiscall *func_0041492A)(void * this);  /* [40] 0x0041492A */
    void * (__thiscall *func_00404EDF)(void * this);  /* [41] 0x00404EDF */
    void * (__thiscall *func_0040AC5E)(void * this);  /* [42] 0x0040AC5E */
    void * (__thiscall *func_00401D98)(void * this);  /* [43] 0x00401D98 */
    void * (__thiscall *func_0040CB17)(void * this);  /* [44] 0x0040CB17 */
    void * (__thiscall *func_0040128F)(void * this);  /* [45] 0x0040128F */
    void * (__thiscall *func_0040C7E3)(void * this);  /* [46] 0x0040C7E3 */
    void * (__thiscall *func_0040DB98)(void * this);  /* [47] 0x0040DB98 */
    void * (__thiscall *func_00404980)(void * this);  /* [48] 0x00404980 */
    void * (__thiscall *func_0040CF45)(void * this);  /* [49] 0x0040CF45 */
    void * (__thiscall *func_004042AA)(void * this);  /* [50] 0x004042AA */
};

/* EntityFlyer_vftable6 - Controllable::Trackable */
/* Vtable at: 0x00A3CB70, 17 entries */
struct EntityFlyer_vftable6 {
    void * (__thiscall *func_0040594D)(void * this);  /* [ 0] 0x0040594D */
    void * (__thiscall *func_00417378)(void * this);  /* [ 1] 0x00417378 */
    void * (__thiscall *func_004093B8)(void * this);  /* [ 2] 0x004093B8 */
    void * (__thiscall *func_00410807)(void * this);  /* [ 3] 0x00410807 */
    void * (__thiscall *func_00408189)(void * this);  /* [ 4] 0x00408189 */
    void * (__thiscall *func_00402F77)(void * this);  /* [ 5] 0x00402F77 */
    void * (__thiscall *func_0040E77D)(void * this);  /* [ 6] 0x0040E77D */
    void * (__thiscall *func_00403B1B)(void * this);  /* [ 7] 0x00403B1B */
    void * (__thiscall *func_00403B39)(void * this);  /* [ 8] 0x00403B39 */
    void * (__thiscall *func_00407C75)(void * this);  /* [ 9] 0x00407C75 */
    void * (__thiscall *func_00401523)(void * this);  /* [10] 0x00401523 */
    void * (__thiscall *func_00415E2E)(void * this);  /* [11] 0x00415E2E */
    void * (__thiscall *func_00409697)(void * this);  /* [12] 0x00409697 */
    void * (__thiscall *func_0040AF5B)(void * this);  /* [13] 0x0040AF5B */
    undefined (__thiscall *thunk_FUN_004bab90)(void * this, int param_1, char param_2);  /* [14] 0x0040DAFD */
    void * (__thiscall *func_0041754E)(void * this);  /* [15] 0x0041754E */
    bool (__thiscall *IsForcedFirstPerson)(void * this);  /* [16] 0x00415A50 */
};

/* EntityFlyer_vftable7 - EntityPathFollower */
/* Vtable at: 0x00A3CB44, 8 entries */
struct EntityFlyer_vftable7 {
    void (__thiscall *thunk_FUN_004f2be0)(void * this, byte param_2);  /* [ 0] 0x00405097 */
    int (__thiscall *GetEntitySquadronClass)(void * this);  /* [ 1] 0x00413791 */
    undefined (__thiscall *thunk_FUN_004d7be0)(void * this, int param_1);  /* [ 2] 0x00409DA9 */
    undefined4 (__thiscall *thunk_FUN_004d7910)(void * this, int param_1);  /* [ 3] 0x00409DAE */
    undefined4 (__thiscall *thunk_FUN_004d7930)(void * this, int param_1);  /* [ 4] 0x00416BF8 */
    undefined (__thiscall *thunk_FUN_00721920)(void * this, int param_1_00, char * param_2, undefined4 param_3, float param_4, float param_5);  /* [ 5] 0x00412332 */
    undefined (__thiscall *thunk_FUN_00721a70)(void * this, int param_1_00, uint param_2, undefined4 param_3, float param_4, float param_5);  /* [ 6] 0x004104FB */
    undefined (__thiscall *thunk_FUN_004f52d0)(void * this, undefined4 * param_1);  /* [ 7] 0x004049AD */
};

