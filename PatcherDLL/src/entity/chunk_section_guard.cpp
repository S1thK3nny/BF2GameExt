#include "pch.h"
#include "chunk_section_guard.hpp"
#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <detours.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// =============================================================================
// How a chunk section crashes (modtools EntityGeometryClass::SetProperty
// 0x00506580; Steam/GOG 0x004BBCE0 is the same code without the warning).
//
// CHUNKSECTION sets a static slot: "CHUNK" or "CHUNK1" is 0 up to "CHUNK9" at 8,
// anything else (CHUNK10 included) is -1 and the section is silently dropped.
//
// ChunkGeometryName then allocates the slot's descriptor and:
//   1. looks the name up as a GameModel. Missing: modtools warns, retail says
//      nothing, and both carry on;
//   2. collects collision primitives "<name>0", "<name>1", ... and with none,
//      builds a CollisionBodyMesh from the collision mesh "<name>" without a
//      null check. Its ctor (0x004284F0) reads mesh->mMask: AV READ 0x30;
//   3. sizes a fallback box from model->[+0x20] (0x00507495), the second crash
//      for a missing model.
// So the model and some collision must both exist. A msh with no p_ objects
// gets only a `coll` chunk from pc_ModelMunge, and -nocollision drops that, so
// such a chunk msh loads with no collision at all (measured on
// all_inf_wookiedreads.msh). A msh with p_ objects keeps its `prim` chunk
// either way.
//
// Seventeen other chunk properties write through the slot's descriptor having
// checked only the slot range, so any of them before a working
// ChunkGeometryName writes through null. ChunkSpeed, ChunkUpFactor,
// ChunkVelocityFactor and ChunkNodeName use arrays in the class and are safe.
//
// ExplosionClass::SetProperty (modtools 0x00600F10, Steam/GOG 0x0051CFF0) has
// one chunk and only the model dereference; CreateExplosion (modtools
// 0x0060257F) already skips a null chunk model.
//
// The fix leaves a broken chunk out: a skipped ChunkGeometryName clears the
// slot to -1, so the engine's own range check drops the rest of that section,
// and CreateChunkCluster skips a slot with no descriptor.
// =============================================================================

namespace {

constexpr uint32_t kChunkSection      = pbl_hash("ChunkSection");
constexpr uint32_t kChunkGeometryName = pbl_hash("ChunkGeometryName");
static_assert(kChunkSection == 0xDA1DE03Du && kChunkGeometryName == 0xDA328455u);

// The handlers that dereference the descriptor, mapped on modtools.
constexpr uint32_t kDescriptorProps[] = {
   pbl_hash("ChunkTerrainEffect"),   pbl_hash("ChunkTerrainCollisions"),
   pbl_hash("ChunkOmega"),           pbl_hash("ChunkAngularDamping"),
   pbl_hash("ChunkSimpleFriction"),  pbl_hash("ChunkSmokeNodeName"),
   pbl_hash("ChunkSmokeEffect"),     pbl_hash("ChunkLifetime"),
   pbl_hash("ChunkTrailEffect"),     pbl_hash("ChunkTrailNodeName"),
   pbl_hash("ChunkStickiness"),      pbl_hash("ChunkBounciness"),
   pbl_hash("ChunkKeepSoldierCollision"), pbl_hash("ChunkPhysics"),
   pbl_hash("ChunkGravity"),         pbl_hash("ChunkDeathSpeed"),
   pbl_hash("ChunkLinearDamping"),
};

constexpr int32_t  kSlots      = 10;    // descriptor pointers per class
constexpr uint32_t kTableSlots = 0x400; // _Find(table, 0x800, hash)

using SetProperty_t = void(__thiscall*)(void* cls, uint32_t hash, const char* value);

SetProperty_t   s_origGeometry  = nullptr;
SetProperty_t   s_origExplosion = nullptr;
int32_t*        s_slot          = nullptr;
uintptr_t       s_descOff       = 0;
const uint32_t* s_models        = nullptr;
const uint32_t* s_prims         = nullptr;
const uint32_t* s_meshes        = nullptr;

// The CHUNKSECTION being read, for the messages.
const void* s_sectionCls = nullptr;
char        s_section[32] = {};

// The last slot reported for orphaned properties, so a section reports once.
const void* s_orphanCls  = nullptr;
int32_t     s_orphanSlot = -1;

bool is_descriptor_prop(uint32_t hash)
{
   for (uint32_t h : kDescriptorProps)
      if (h == hash) return true;
   return false;
}

// Bounded copy of the engine's probe, so a full table cannot hang the check.
bool table_has(const uint32_t* keys, uint32_t key)
{
   if (!keys || key == 0) return false;
   uint32_t slot = key & (kTableSlots - 1);
   for (uint32_t n = 0; n < kTableSlots; ++n) {
      if (keys[slot] == key) return true;
      if (keys[slot] == 0) return false;
      slot = slot ? slot - 1 : kTableSlots - 1;
   }
   return false;
}

enum class Missing { None, Model, Collision };

Missing check_geometry(const char* name, bool needCollision)
{
   const uint32_t h = pbl_hash(name);
   if (!table_has(s_models, h)) return Missing::Model;
   if (needCollision && !table_has(s_prims, pbl_hash_append(h, "0")) && !table_has(s_meshes, h))
      return Missing::Collision;
   return Missing::None;
}

// ` of "<odf>"` where the build keeps the ODF name in the class (modtools).
void odf_suffix(const void* cls, uintptr_t nameOff, char* out, size_t cap)
{
   out[0] = 0;
   if (!cls || !nameOff) return;
   char name[33];
   const char* src = (const char*)cls + nameOff;
   size_t i = 0;
   for (; i < 32 && src[i]; ++i) name[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? src[i] : '?';
   name[i] = 0;
   if (i) _snprintf_s(out, cap, _TRUNCATE, " of \"%s\"", name);
}

void report(const char* fmt, ...)
{
   char msg[512];
   va_list ap;
   va_start(ap, fmt);
   _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
   va_end(ap);
   install_log("%s", msg);
   warn_gamelog(RED_SEVERITY_ERROR, SRC_FILE, __LINE__, "%s", msg);
}

void __fastcall hooked_geometry(void* cls, void*, uint32_t hash, const char* value)
{
   if (hash == kChunkSection) {
      s_origGeometry(cls, hash, value);
      s_sectionCls = cls;
      strncpy_s(s_section, value ? value : "", _TRUNCATE);
      s_orphanCls = nullptr;
      if (*s_slot < 0) {
         char odf[48];
         odf_suffix(cls, g_addr->entity_class_name_off, odf, sizeof(odf));
         report("[Chunks] CHUNKSECTION \"%s\"%s is not CHUNK1 to CHUNK9, so that chunk is "
                "left out.", s_section, odf);
      }
      return;
   }

   // The slot is static and never reset, so it can be left over from another
   // ODF; the guards below only care whether it is in range.
   const int32_t slot = *s_slot;
   if (slot >= 0 && slot < kSlots) {
      const bool ourSection = cls == s_sectionCls;
      const char* section = ourSection ? s_section : "a chunk section";

      if (hash == kChunkGeometryName) {
         const Missing missing = check_geometry(value, true);
         if (missing != Missing::None) {
            char odf[48];
            odf_suffix(cls, g_addr->entity_class_name_off, odf, sizeof(odf));
            if (missing == Missing::Model)
               report("[Chunks] Chunk geometry \"%s\" in %s%s is not loaded, so that chunk "
                      "is left out.", value ? value : "", section, odf);
            else
               report("[Chunks] Chunk geometry \"%s\" in %s%s has no collision, so that chunk "
                      "is left out. Give the msh a p_ collision primitive or munge it without "
                      "-nocollision.", value ? value : "", section, odf);
            *s_slot = -1;
            return;
         }
      }
      else if (is_descriptor_prop(hash)) {
         void* const* descs = (void* const*)((const uint8_t*)cls + s_descOff);
         if (descs[slot] == nullptr) {
            if (cls != s_orphanCls || slot != s_orphanSlot) {
               char odf[48];
               odf_suffix(cls, g_addr->entity_class_name_off, odf, sizeof(odf));
               if (ourSection)
                  report("[Chunks] %s%s has chunk properties before its ChunkGeometryName, "
                         "so they are ignored.", s_section, odf);
               else
                  report("[Chunks] Chunk properties%s come before any CHUNKSECTION with a "
                         "ChunkGeometryName, so they are ignored.", odf);
               s_orphanCls  = cls;
               s_orphanSlot = slot;
            }
            return;
         }
      }
   }
   s_origGeometry(cls, hash, value);
}

void __fastcall hooked_explosion(void* cls, void*, uint32_t hash, const char* value)
{
   if (hash == kChunkGeometryName && check_geometry(value, false) == Missing::Model) {
      char odf[48];
      odf_suffix(cls, g_addr->explosion_class_name_off, odf, sizeof(odf));
      report("[Chunks] Explosion chunk geometry \"%s\"%s is not loaded, so the explosion "
             "throws no chunk.", value ? value : "", odf);
      return;
   }
   s_origExplosion(cls, hash, value);
}

} // namespace

void chunk_section_guard_install(uintptr_t exe_base)
{
   const GameAddrs& a = *g_addr;
   if (!a.entity_geometry_class_set_property || !a.explosion_class_set_property ||
       !a.chunk_section_index || !a.chunk_descriptors_off || !a.game_model_table ||
       !a.collision_primitive_table || !a.collision_mesh_table)
      return;

   // PUSH EBP / MOV EBP,ESP / AND ESP,-16 opens both functions on every build.
   static const uint8_t kPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
   void* const geometry  = resolve(exe_base, a.entity_geometry_class_set_property);
   void* const explosion = resolve(exe_base, a.explosion_class_set_property);
   if (memcmp(geometry, kPrologue, sizeof(kPrologue)) != 0 ||
       memcmp(explosion, kPrologue, sizeof(kPrologue)) != 0) {
      install_log("[Chunks] not installed: SetProperty at %08X / %08X does not match",
                  (unsigned)a.entity_geometry_class_set_property,
                  (unsigned)a.explosion_class_set_property);
      return;
   }

   s_slot    = (int32_t*)resolve(exe_base, a.chunk_section_index);
   s_descOff = a.chunk_descriptors_off;
   s_models  = (const uint32_t*)resolve(exe_base, a.game_model_table);
   s_prims   = (const uint32_t*)resolve(exe_base, a.collision_primitive_table);
   s_meshes  = (const uint32_t*)resolve(exe_base, a.collision_mesh_table);
   s_origGeometry  = (SetProperty_t)geometry;
   s_origExplosion = (SetProperty_t)explosion;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(reinterpret_cast<PVOID*>(&s_origGeometry), hooked_geometry);
   DetourAttach(reinterpret_cast<PVOID*>(&s_origExplosion), hooked_explosion);
   const LONG commit = DetourTransactionCommit();
   if (commit != NO_ERROR) {
      install_log("[Chunks] not installed: DetourTransactionCommit=%ld", commit);
      s_origGeometry  = nullptr;
      s_origExplosion = nullptr;
      return;
   }
   install_log("[Chunks] installed: broken chunk sections are left out with an error");
}

void chunk_section_guard_uninstall()
{
   if (!s_origGeometry) return;
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(reinterpret_cast<PVOID*>(&s_origGeometry), hooked_geometry);
   DetourDetach(reinterpret_cast<PVOID*>(&s_origExplosion), hooked_explosion);
   DetourTransactionCommit();
   s_origGeometry  = nullptr;
   s_origExplosion = nullptr;
}
