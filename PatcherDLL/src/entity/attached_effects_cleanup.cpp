#include "pch.h"
#include "attached_effects_cleanup.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

// =============================================================================
// ODF AttachEffect outliving the entity that owned it.
//
// -----------------------------------------------------------------------------
// The bug
//
// An entity class can pin effects to hardpoints:
//
//     AttachEffect      = "gta_weapon_flash"
//     AttachToHardPoint = "hp_attach"
//
// EntityGeometry::BuildEffects hands the class off to
// AttachedEffectsClass::BuildEffects, which allocates AttachedEffects, then
// allocates m_aAttachData - one 12-byte record per attach entry - and creates an
// FLEffectObject into each record through the entry's ODF class factory.
//
// EntityGeometry::~EntityGeometry does delete the AttachedEffects object.  That
// part is fine.  The destructor it reaches is not:
//
//     AttachedEffects::_scalar_deleting_destructor_(this, flags)
//     {
//         Thread::~Thread(this);
//         if (flags & 1) operator delete(this);
//     }
//
// That is the whole function.  It never walks m_aAttachData, so none of the
// FLEffectObjects it created are destroyed, and the array itself is never freed
// either.  Deleting an entity therefore orphans every ODF attach effect it
// carried, and the orphans keep running.
//
// -----------------------------------------------------------------------------
// The crash
//
//     EXCEPTION C0000005 at EIP=00764A3F   AV: READ addr 0000004C
//     EAX=1AD6EDF0  EDX=0000000C  ECX=1AD6EDFC
//
// modtools 0x007649E0 is the effect's per-frame owner-matrix fetch:
//
//     00764A34  CALL 0x00763C60        ; EAX = this->mOwner
//     00764A39  MOV  EDX,[EAX + 0xC]   ; the owner's secondary-base vftable
//     00764A3C  LEA  ECX,[EAX + 0xC]
//     00764A3F  CALL [EDX + 0x40]      ; owner->GetWorldMatrix()
//
// and 0x00763C60 is `return *(void**)(this + 0x24)` - a raw owner pointer with
// no validation.  Once the owner is freed, [EAX+0xC] is recycled heap (0xC in
// the reported case), so the call reads [0x4C] and dies.
//
// This is an oversight rather than a design.  The object stores a handle pair at
// +0x24/+0x28, and the sibling branch of the same function - the one taken when
// the owner is a GameObject - does check `owner[0x81] == this[0x28]` before using
// it.  The EntityGeometry branch skips the check entirely.  EntityGeometry's
// destructor also already calls PAAnimationGroup::GeometryDeleted, so the engine
// has a "this geometry died" notification pattern; effects just never got one.
//
// Note the distinction: *killing* a GameObject (health 0 + the death virtual)
// does not free the entity, so its effects legitimately keep playing.  Only an
// actual delete produces the orphan.  This fix only runs on destruction.
//
// -----------------------------------------------------------------------------
// The fix
//
// Release the effects the class owns before the destructor runs, using the same
// virtual the engine's own Lua RemoveEffect uses:
//
//     Lua_Callbacks::RemoveEffect(L)
//     {
//         void* fx = lua_touserdata(L, 1);
//         if (fx) (*(void(__thiscall**)(void*))(*(void***)fx + 0x20))(fx);
//     }
//
// so deregistration from the effect manager is the engine's business, not ours.
//
// Installed by replacing slot 0 of the AttachedEffects vftable rather than by
// patching code.  The slot is exact - it cannot catch another class - the
// original pointer is what we chain to, and uninstall is a single dword store.
// The replaced value is verified against the derived original first, so a build
// whose address has not been checked, or a modified executable, no-ops.
//
// The 12-byte-per-entry m_aAttachData array is still leaked; freeing it would
// mean matching the engine's operator delete[] against a pointer that carries a
// 4-byte count prefix, which is more risk than a bounded handful of bytes per
// deleted entity is worth.  The crash and the stuck effect are what this fixes.
//
// -----------------------------------------------------------------------------
// Layout, verified identical on all three builds
//
//     AttachedEffects      +0x18  AttachedEffectsClass* m_pClass
//                          +0x2C  AttachData*           m_aAttachData
//     AttachedEffectsClass +0x00  AttachClassData*      entries
//                          +0x04  uint8_t               count
//     AttachData (0xC)     +0x00  FLEffectObject*       effect
//                          +0x04  uint32_t              savedHandleId
//                          +0x08  float                 timer
//     FLEffectObject       +0x1C  uint32_t              handle id
//                          vftable + 0x20               destroy (__thiscall)
//
// Read off AttachedEffects::Update on each build - modtools 0x004C1FF0,
// Steam 0x00446F70, GOG 0x00446F50 - which uses every one of these offsets:
//
//     MOV   ECX,[ESI + 0x18]      ; m_pClass
//     MOV   EAX,[ECX + 0x4]       ; count
//     MOVZX EAX,AL
//     ...
//     MOV   ECX,[ESI + 0x2C]      ; m_aAttachData
//     ADD   ECX,EDI               ; + i * 0xC
//     MOV   EAX,[ECX]             ; effect
//     TEST  EAX,EAX
//     MOV   EAX,[EAX + 0x1C]      ; its handle id
//     CMP   EAX,[ECX + 0x4]       ; against the saved one
//
// The three builds differ only in the two addresses in game_addrs.hpp.
// =============================================================================

namespace {

// AttachedEffects
constexpr uint32_t kOffClass      = 0x18;
constexpr uint32_t kOffAttachData = 0x2C;
// AttachedEffectsClass
constexpr uint32_t kOffCount = 0x04;
// AttachData
constexpr uint32_t kAttachDataStride = 0x0C;
constexpr uint32_t kOffSavedId       = 0x04;
// FLEffectObject
constexpr uint32_t kOffEffectId  = 0x1C;
constexpr uint32_t kDestroySlot  = 0x20;

// The original slot-0 value, resolved. Also the chain target.
void* s_origDtor = nullptr;
// The vftable slot we overwrote, so uninstall can put it back.
void** s_slot = nullptr;

// Destroy every live effect this AttachedEffects owns, then blank the records so
// nothing downstream can find them again. Runs before the engine's destructor.
//
// Every entry is considered: AttachedEffects::Update reaches m_aAttachData[i] on
// both sides of its `entries[i].flags & 1` test, so the flag does not tell us
// whether a record holds an effect. The handle check does.
void __fastcall release_attached_effects(uint8_t* self)
{
   if (!self) return;

   uint8_t* cls  = *(uint8_t**)(self + kOffClass);
   uint8_t* data = *(uint8_t**)(self + kOffAttachData);
   if (!cls || !data) return;

   const uint32_t count = *(uint8_t*)(cls + kOffCount);

   for (uint32_t i = 0; i < count; ++i) {
      uint8_t* rec = data + i * kAttachDataStride;
      uint8_t* fx  = *(uint8_t**)rec;
      if (!fx) continue;

      // Same validation the engine does before touching one of these: a stale
      // handle means the effect already died and the slot was not cleared.
      if (*(uint32_t*)(fx + kOffEffectId) != *(uint32_t*)(rec + kOffSavedId)) {
         *(uint32_t*)rec               = 0;
         *(uint32_t*)(rec + kOffSavedId) = 0;
         continue;
      }

      void** vtbl = *(void***)fx;
      typedef void(__thiscall* Destroy_t)(void*);
      ((Destroy_t)vtbl[kDestroySlot / sizeof(void*)])(fx);

      *(uint32_t*)rec                 = 0;
      *(uint32_t*)(rec + kOffSavedId) = 0;
   }
}

// __thiscall(this, uint flags), callee-cleans (RET 4). Entered by the engine's
// own `(**(code**)vtbl)(1)`, so on entry ECX = this and the stack is
// [retaddr][flags]. We leave both exactly as they were and tail-jump to the
// original, which does its own RET 4.
__declspec(naked) void attached_effects_dtor_hook()
{
   __asm {
      push ecx                        // this, across the helper
      call release_attached_effects   // __fastcall: ECX = this
      pop  ecx
      jmp  [s_origDtor]
   }
}

} // namespace

// -----------------------------------------------------------------------------
void attached_effects_cleanup_install(uintptr_t exe_base)
{
   uintptr_t vftableVA = 0, dtorSlotVA = 0;
   switch (g_build) {
   case GameBuild::Modtools:
      vftableVA  = game_addrs::modtools::attached_effects_vftable;
      dtorSlotVA = game_addrs::modtools::attached_effects_dtor_slot;
      break;
   case GameBuild::Steam:
      vftableVA  = game_addrs::steam::attached_effects_vftable;
      dtorSlotVA = game_addrs::steam::attached_effects_dtor_slot;
      break;
   case GameBuild::GOG:
      vftableVA  = game_addrs::gog::attached_effects_vftable;
      dtorSlotVA = game_addrs::gog::attached_effects_dtor_slot;
      break;
   default:
      return; // unknown build
   }
   if (vftableVA == 0 || dtorSlotVA == 0) return; // not derived on this build

   void** slot     = (void**)resolve(exe_base, vftableVA);
   void*  expected = resolve(exe_base, dtorSlotVA);

   // Positively identify the vftable before writing into it. A build whose
   // address has not been derived, or an executable that is not stock, fails
   // this and the fix no-ops rather than corrupting a dispatch table.
   if (*slot != expected) return;

   // .rdata is RW for the whole install window (dllmain re-protects afterwards),
   // so no VirtualProtect here.
   s_origDtor = expected;
   s_slot     = slot;
   *slot      = (void*)&attached_effects_dtor_hook;
}

void attached_effects_cleanup_uninstall()
{
   if (!s_slot) return;

   protected_write(s_slot, &s_origDtor, sizeof(void*));
   s_slot     = nullptr;
   s_origDtor = nullptr;
}
