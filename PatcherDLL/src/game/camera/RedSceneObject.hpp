#pragma once

#include <cstdint>
#include <cstddef>

#include "../pbl/PblSphere.hpp"

namespace game {

struct RedSceneAABox;  // forward declaration
struct PblMatrix;
struct PblVector3;

// -------------------------------------------------------------------------
// RedSceneObject_vftable — 25 virtual methods (verified Steam)
// -------------------------------------------------------------------------
// Full class hierarchy: BaseDisplayable → DynDisplayable → RedSceneObject.
// The RedSceneObject struct below is the DATA portion only (no vtable field).
// Objects with this vtable have it at +0x00, with data at build-specific offsets.
// Slot 19 (Render) is pure virtual on base — overridden by derived classes.

struct RedSceneObject_vftable {
    void           (__thiscall* OnTraverse)(void* self, void* traverser);       //  0
    void           (__thiscall* SetActive)(void* self, int p1);                 //  1
    bool           (__thiscall* IsActive)(void* self, int p1);                  //  2
    void           (__thiscall* SetProcessed)(void* self, int* p1);             //  3
    bool           (__thiscall* IsProcessed)(void* self, int p1);               //  4
    void           (__thiscall* GetAABound)(void* self, void* box);             //  5
    void           (__thiscall* GetSphereBound)(void* self, void* sphere);      //  6
    bool           (__thiscall* PortalClip)(void* self);                        //  7
    void           (__thiscall* SetPortalClip)(void* self, bool val);           //  8
    bool           (__thiscall* PortalActive)(void* self);                      //  9
    void           (__thiscall* SetPortalActive)(void* self, bool val);         // 10
    bool           (__thiscall* Dynamic)(void* self);                           // 11
    void           (__thiscall* SetDynamic)(void* self, bool val);              // 12
    void*          (__thiscall* GetSector)(void* self);                         // 13
    void           (__thiscall* SetSector)(void* self, void* sector);           // 14
    void           (__thiscall* Update)(void* self, void* manager);             // 15
    void*          (__thiscall* Dtor)(void* self, uint32_t flags);              // 16
    void           (__thiscall* Activate)(void* self);                          // 17
    void           (__thiscall* Deactivate)(void* self);                        // 18
    void           (__thiscall* Render)(void* self, int mode, float opacity,    // 19  pure virtual on base
                                        uint32_t flags);
    PblMatrix*     (__thiscall* GetBBoxMatrix)(void* self);                     // 20
    void*          (__thiscall* GetBBox)(void* self);                           // 21
    bool           (__thiscall* IsReflected)(void* self);                       // 22
    void           (__thiscall* SetRenderPos)(void* self, PblVector3* pos);     // 23
    void           (__thiscall* SetRenderRadius)(void* self, float radius);     // 24
};
static_assert(sizeof(RedSceneObject_vftable) == 100);

struct RedLodData {
    char     _pad0[0x14];
    int      mLodClass;     // +0x14 - LOD class (0-3)
    uint8_t  mLowRezFlags;  // +0x18
};
static_assert(offsetof(RedLodData, mLodClass) == 0x14);
static_assert(offsetof(RedLodData, mLowRezFlags) == 0x18);

// -------------------------------------------------------------------------
// RedSceneObject
// -------------------------------------------------------------------------

struct RedSceneObject {
    RedSceneObject* mNextSceneObject;    // +0x00
    RedSceneObject* mPrevSceneObject;    // +0x04
    RedSceneAABox*  mSceneAABox;         // +0x08
    PblSphere       _Sphere;             // +0x0C
    RedLodData*     _pLodData;           // +0x1C
    uint32_t        _uiRenderFlags;      // +0x20
    bool            _bActive;            // +0x24
    char            _pad25[0x03];        // +0x25
    float           _fPriorityMod;       // +0x28
    int             _iFrameNum[1];       // +0x2C
    uint32_t        _uiOcclusionIndex[1];// +0x30
};
static_assert(sizeof(RedSceneObject) == 0x34);
static_assert(offsetof(RedSceneObject, mSceneAABox) == 0x08);
static_assert(offsetof(RedSceneObject, _Sphere) == 0x0C);
static_assert(offsetof(RedSceneObject, _pLodData) == 0x1C);
static_assert(offsetof(RedSceneObject, _uiRenderFlags) == 0x20);
static_assert(offsetof(RedSceneObject, _bActive) == 0x24);
static_assert(offsetof(RedSceneObject, _fPriorityMod) == 0x28);
static_assert(offsetof(RedSceneObject, _uiOcclusionIndex) == 0x30);

} // namespace game
