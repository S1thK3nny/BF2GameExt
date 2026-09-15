#include "pch.h"
#include "dual_cannon.hpp"
#include "barrel_fire_origin.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <detours.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <unordered_map>

// =============================================================================
// ClassLabel "dualcannon"
//
// A weapon ODF's ClassLabel resolves by a linear scan of Factory<Weapon>::sList for
// a matching PblHash, so registering one more base class object is all it takes for
// the label to load. GameState::CreateBaseWeaponClasses builds the stock list on every
// mission load (PostStateCleanup unlinks it and resets sCounter), so we hook it and
// append ours after the original returns.
//
// The object is a real WeaponCannonClass, built by the engine's own constructor, and
// then re-pointed at a DLL-owned copy of the cannon class vtable. Every per-ODF class
// the loader derives from it goes through our Derive, and every weapon it builds
// through our Build, so both carry our vtables from then on while inheriting all
// cannon behaviour.
//
// Derive and Build CHAIN to whatever the live cannon vtable slot held when we copied
// it, rather than allocating themselves. held_ordnance_effect hooks Derive (and
// SetProperty and the destructor) to keep side tables in step with class inheritance,
// and allocating here would silently skip that. Extra per-class and per-weapon state
// therefore lives in side tables too, never in a larger object.
//
// Multiplayer: the base constructor post-increments Factory::sCounter into
// Factory::mNetIndex (+0x1C), which WriteNetEvent sends in 8 bits for CREATE_ORDNANCE
// and ReadWeaponClass resolves back. One extra registration would shift the index of
// every ODF class after it, so an unmodded peer would attribute kills to the wrong
// weapon. The base class object itself is never a fire weapon, so it takes 0xFF (the
// "no class" value, which lookups reject before scanning) and gives its counter slot
// back. Maps that do not use dualcannon then index exactly as they would without us.
//
// ODF properties (on top of every cannon property):
//   OffhandGeometryName  second model. OdfMunge adds any *GeometryName to the .req.
//   OffhandHardPoint     skeleton hardpoint the second model hangs on, e.g. hp_weapons2
//   OffhandFirePointName fire point on the second model (default hp_fire, as stock)
//   AlternateMode        "shot": switch gun per trigger pull (per salvo)
//                        "salvo": switch gun on every shot within a salvo
//
// Alternating fire. WeaponCannon::Fire is non-virtual but has a single caller and
// reads Aimer::mFirePos and mDirection on every call, so a detour on it (a vptr compare
// for every other cannon) can move each shot to the gun that fires it. Around the call
// it also sets Weapon::mState to FIRE for gun 1 and FIRE2 for gun 2:
// SoldierAnimatorClass::WeaponStateToAnimation maps those to shoot / shoot2 (and
// shoot_secondary / shoot_secondary2 in the secondary slot), the first person
// animator to its shoot states 2 / 3, and Weapon::Write already replicates mState.
// FireAnim, which the engine uses to pick that state, is therefore ignored.
//
// The muzzle flash belongs to the gun that fired last: Weapon::Render draws it at the
// main fire point, so for gun 2 the flash timer is held back around that call and the
// flash drawn at the offhand fire point instead.
//
// FIRE2 makes a latent engine bug reachable. First person picks its animation from
// FirstPerson::mAnim[weaponClass * 11 + state], and FirstPerson::Init fills it on every
// ingame.lvl load: slots with a name are looked up, and any slot still null afterwards
// gets humanfp_tool_idle. The shoot2 slot (state 3) has no name for the rifle, bazooka
// and tool classes, so Init never clears it: it keeps the previous level's pointer, which
// is freed memory by then. Stock never reaches those slots, because only a grenade
// enters FIRE2. We clear the table before Init runs so every unnamed slot is refilled
// from the level being loaded.
//
// The spawn screen preview soldier has no Weapon instance: SoldierElement draws its
// selected weapon with the non-virtual WeaponClass::Render, which none of the vtable
// overrides reach. A detour on it (keyed on the side table, so stock classes pass
// straight through) draws the offhand model there too.
// =============================================================================

namespace {

// PblHash("dualcannon"), from ToolsFL\bin\Hash.exe.
constexpr uint32_t kDualCannonHash = 0x14064EC2;

constexpr unsigned kCannonClassSize   = 0x3DC;
constexpr unsigned kClassVtableSlots  = 13;
constexpr unsigned kWeaponVtableSlots = 61;
constexpr unsigned kFactoryNetIndex   = 0x1C;
constexpr uint32_t kNoNetIndex        = 0xFF;

// Class vtable slots.
constexpr unsigned kSlotDestroyClass = 0;
constexpr unsigned kSlotDerive       = 1;
constexpr unsigned kSlotBuild        = 2;
constexpr unsigned kSlotSetProperty  = 6;

// Weapon vtable slots.
constexpr unsigned kSlotDestroyWeapon = 0;
constexpr unsigned kSlotRender        = 0x8C / 4;

// WeaponClass fields (modtools).
constexpr unsigned kClassName         = 0x30;  // mName, printed by the engine's own warnings
constexpr unsigned kClassFlashLength  = 0x290;
constexpr unsigned kClassShotsPerSalvo = 0x354;
constexpr unsigned kClassShotsPerShot = 0x358;

// Weapon fields (modtools, see Weapon::Render 0x61DFA0 and WeaponCannon::UpdateFire 0x6274C0).
constexpr unsigned kWeaponClass        = 0x64;
constexpr unsigned kWeaponRenderClass  = 0x68;
constexpr unsigned kWeaponAimer        = 0x70;
constexpr unsigned kWeaponHideFlags    = 0xAC; // bit 0 = mHideWeapon
constexpr unsigned kWeaponState        = 0xB0;
constexpr unsigned kWeaponFlashStart   = 0xC4; // mMuzzleFlashStartTime
constexpr unsigned kWeaponSalvoCount   = 0x144;

// Weapon::WeaponState
constexpr uint32_t kStateFire  = 1;
constexpr uint32_t kStateFire2 = 2;

// Aimer fields.
constexpr unsigned kAimerDirection = 0x48;
constexpr unsigned kAimerFirePos   = 0x88;

// RedPose: Weapon::Render looks hardpoints up with _Find(pose + 4, 0x100, crc).
constexpr unsigned kPoseTable     = 4;
constexpr int      kPoseTableSize = 0x100;

// RedModel vtable +4: Render(PblMatrix* world, int, RedColor* color, uint flags, int).
constexpr unsigned kSlotModelRender = 1;

constexpr uint32_t pbl_hash(const char* text)
{
   uint32_t hash = 0x811C9DC5;
   for (; *text; ++text) hash = (hash ^ (static_cast<uint8_t>(*text) | 0x20u)) * 0x01000193u;
   return hash;
}

// PblTEMPHash: case-sensitive CRC-32/BZIP2, the key for model and pose hardpoints.
constexpr uint32_t temp_hash(const char* text)
{
   uint32_t hash = 0xFFFFFFFF;
   for (; *text; ++text) {
      hash ^= static_cast<uint32_t>(static_cast<uint8_t>(*text)) << 24;
      for (unsigned bit = 0; bit != 8; ++bit)
         hash = (hash << 1) ^ ((hash & 0x80000000u) ? 0x04C11DB7u : 0);
   }
   return hash ^ 0xFFFFFFFF;
}

constexpr uint32_t kPropOffhandGeometry  = pbl_hash("OffhandGeometryName");
constexpr uint32_t kPropOffhandHardPoint = pbl_hash("OffhandHardPoint");
constexpr uint32_t kPropOffhandFirePoint = pbl_hash("OffhandFirePointName");
constexpr uint32_t kPropAlternateMode    = pbl_hash("AlternateMode");
constexpr uint32_t kPropFireAnim         = pbl_hash("FireAnim");

// WeaponClass::SetProperty falls back to this when an ODF names no FirePointName.
constexpr uint32_t kDefaultFirePoint = temp_hash("hp_fire");

enum class alternate_mode : uint8_t { shot, salvo };

struct class_ext {
   void*          model          = nullptr; // RedModel*
   uint32_t       hardPoint      = 0;       // pose key, 0 = unset
   uint32_t       firePoint      = kDefaultFirePoint;
   bool           firePointNamed = false;
   bool           firePointFound = false;
   float          firePointOffset[3] = {};
   alternate_mode mode           = alternate_mode::shot;
};

struct weapon_ext {
   uint8_t barrel     = 0;     // gun the next new shot starts from, before switching
   uint8_t lastFired  = 0;     // gun of the most recent shot, owns the muzzle flash
   bool    anyShot    = false;
   bool    offhandValid = false;
   float   offhandFirePos[3] = {};
   // What identifies a new shot rather than another ShotsPerShot pellet of the same one.
   int      lastSalvoCount = 0;
   float    lastFireTime   = 0.0f;
   unsigned pellets        = 0;
};

using fn_create_base_classes_t = void(__cdecl*)();
using fn_operator_new_t        = void*(__cdecl*)(unsigned size);
using fn_class_ctor_t          = void*(__thiscall*)(void* self, uint32_t hash);
using fn_destroy_t             = void*(__thiscall*)(void* self, unsigned flags);
using fn_derive_t              = void*(__thiscall*)(void* self, uint32_t hash);
using fn_build_t               = void*(__thiscall*)(void* self, void* desc);
using fn_set_property_t        = void(__thiscall*)(void* self, uint32_t hash, const char* value);
using fn_render_t              = void(__thiscall*)(void* self, const float* world, void* pose,
                                                   const uint8_t* color, uint32_t flags,
                                                   uint32_t highRes);
using fn_fire_t                = bool(__fastcall*)(void* self, void* edx);
using fn_find_model_t          = void*(__cdecl*)(uint32_t hash);
using fn_get_hardpoint_t       = uint32_t(__thiscall*)(void* model, uint32_t crc, float* out);
using fn_hash_table_find_t     = void*(__cdecl*)(void* table, int size, uint32_t hash);
using fn_model_render_t        = void(__thiscall*)(void* model, const float* world, int,
                                                   const uint8_t* color, uint32_t flags, int);
using fn_render_flash_t        = void(__thiscall*)(void* cls, const float* pos, const float* dir,
                                                   float t);
using fn_mission_time_t        = float(__cdecl*)();

using fn_first_person_init_t   = void(__cdecl*)();
using fn_class_render_t        = void(__fastcall*)(void* cls, void* edx, const float* world,
                                                   void* pose, const uint8_t* color,
                                                   uint32_t flags, uint32_t highRes);

constexpr unsigned kFirstPersonAnimSlots = 48;

fn_create_base_classes_t original_create_base_classes = nullptr;
fn_fire_t                original_fire                = nullptr;
fn_first_person_init_t   original_first_person_init   = nullptr;
fn_class_render_t        original_class_render        = nullptr;
void**                   s_firstPersonAnims           = nullptr;
fn_operator_new_t        s_operatorNew     = nullptr;
fn_class_ctor_t          s_classCtor       = nullptr;
fn_find_model_t          s_findModel       = nullptr;
fn_get_hardpoint_t       s_getHardPoint    = nullptr;
fn_hash_table_find_t     s_hashTableFind   = nullptr;
fn_render_flash_t        s_renderFlash     = nullptr;
fn_mission_time_t        s_missionTime     = nullptr;
void**                   s_cannonClassVtable  = nullptr;
void**                   s_cannonWeaponVtable = nullptr;
uint32_t*                s_factoryCounter     = nullptr;

// DLL-owned vtables, filled from the live cannon tables on first registration. That is
// long after every install-time vtable patch (barrel_fire_origin, held_ordnance_effect)
// has landed, so the copies inherit those hooks.
void* s_classVtable[kClassVtableSlots]   = {};
void* s_weaponVtable[kWeaponVtableSlots] = {};
bool  s_vtablesReady = false;

// The implementations our overrides chain to.
fn_destroy_t      s_baseDestroyClass  = nullptr;
fn_derive_t       s_baseDerive        = nullptr;
fn_build_t        s_baseBuild         = nullptr;
fn_set_property_t s_baseSetProperty   = nullptr;
fn_destroy_t      s_baseDestroyWeapon = nullptr;
fn_render_t       s_baseRender        = nullptr;

// Keyed by WeaponClass* and Weapon*. Cleared on every mission load, because
// PostStateCleanup unlinks the classes without destroying them and the next state
// reuses the addresses.
std::unordered_map<void*, class_ext>  s_classes;
std::unordered_map<void*, weapon_ext> s_weapons;
std::mutex                            s_mutex;

template<class T>
T& at(void* p, unsigned offset)
{
   return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + offset);
}

void install_log(const char* fmt, ...)
{
   // CRT only: install runs while every section is mapped PAGE_READWRITE, so
   // calling back into the engine's logger would EXEC-fault.
   FILE* f = nullptr;
   if (fopen_s(&f, "BF2GameExt.log", "a") != 0 || !f) return;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fclose(f);
}

bool is_dual_weapon(void* weapon)
{
   return weapon && *static_cast<void***>(weapon) == s_weaponVtable;
}

// Re-resolve the offhand fire point after the model or the name changed. Mirrors
// WeaponClass::SetProperty: GeometryName probes hp_fire quietly, FirePointName warns.
// Returns false only when an explicitly named fire point is missing.
bool resolve_fire_point(class_ext& ext)
{
   ext.firePointFound = false;
   if (!ext.model) return true;
   ext.firePointFound = s_getHardPoint(ext.model, ext.firePoint, ext.firePointOffset) != 0;
   return ext.firePointFound || !ext.firePointNamed;
}

void __fastcall dual_set_property(void* cls, void* /*edx*/, uint32_t hash, const char* value)
{
   if (hash != kPropOffhandGeometry && hash != kPropOffhandHardPoint &&
       hash != kPropOffhandFirePoint && hash != kPropAlternateMode && hash != kPropFireAnim) {
      s_baseSetProperty(cls, hash, value);
      return;
   }
   if (!cls || !value) return;

   const char* warning = nullptr;
   {
      std::lock_guard<std::mutex> lock(s_mutex);
      class_ext& ext = s_classes[cls];

      if (hash == kPropFireAnim) {
         warning = "FireAnim \"%s\" is ignored: the gun that fires picks shoot or shoot2";
      }
      else if (hash == kPropOffhandGeometry) {
         ext.model = *value ? s_findModel(pbl_hash(value)) : nullptr;
         if (*value && !ext.model)
            warning = "OffhandGeometryName model \"%s\" is not loaded";
         else if (!resolve_fire_point(ext))
            warning = "OffhandFirePointName not found on OffhandGeometryName \"%s\"";
      }
      else if (hash == kPropOffhandHardPoint) {
         ext.hardPoint = *value ? temp_hash(value) : 0;
      }
      else if (hash == kPropOffhandFirePoint) {
         ext.firePointNamed = *value != 0;
         ext.firePoint      = *value ? temp_hash(value) : kDefaultFirePoint;
         if (!resolve_fire_point(ext))
            warning = "OffhandFirePointName \"%s\" does not exist on the offhand model";
      }
      else if (_stricmp(value, "shot") == 0) {
         ext.mode = alternate_mode::shot;
      }
      else if (_stricmp(value, "salvo") == 0) {
         ext.mode = alternate_mode::salvo;
      }
      else {
         warning = "AlternateMode \"%s\" is not \"shot\" or \"salvo\", using \"shot\"";
         ext.mode = alternate_mode::shot;
      }
   }

   if (warning) {
      char message[256];
      _snprintf_s(message, sizeof(message), _TRUNCATE, warning, value);
      warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__, "[DualCannon] '%s' %s\n",
                   &at<char>(cls, kClassName), message);
   }
}

void* __fastcall dual_destroy_class(void* cls, void* /*edx*/, unsigned flags)
{
   {
      std::lock_guard<std::mutex> lock(s_mutex);
      s_classes.erase(cls);
   }
   return s_baseDestroyClass(cls, flags);
}

void* __fastcall dual_derive(void* self, void* /*edx*/, uint32_t hash)
{
   void* derived = s_baseDerive(self, hash);
   if (!derived) return derived;

   *static_cast<void***>(derived) = s_classVtable;

   // The native copy constructor inherits every stock field; do the same for ours.
   std::lock_guard<std::mutex> lock(s_mutex);
   auto parent = s_classes.find(self);
   if (parent != s_classes.end())
      s_classes[derived] = parent->second;
   else
      s_classes.erase(derived);
   return derived;
}

void* __fastcall dual_build(void* self, void* /*edx*/, void* desc)
{
   void* weapon = s_baseBuild(self, desc);
   if (weapon) *static_cast<void***>(weapon) = s_weaponVtable;
   return weapon;
}

void* __fastcall dual_destroy_weapon(void* weapon, void* /*edx*/, unsigned flags)
{
   {
      std::lock_guard<std::mutex> lock(s_mutex);
      s_weapons.erase(weapon);
   }
   return s_baseDestroyWeapon(weapon, flags);
}

// D3DXMatrixMultiply(out, a, b): out = a * b, row-major.
void matrix_multiply(float* out, const float* a, const float* b)
{
   for (unsigned r = 0; r != 4; ++r)
      for (unsigned c = 0; c != 4; ++c)
         out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                          a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
}

// D3DXVec3TransformCoord for an affine row-major matrix.
void transform_coord(float* out, const float* v, const float* m)
{
   for (unsigned c = 0; c != 3; ++c)
      out[c] = v[0] * m[0 * 4 + c] + v[1] * m[1 * 4 + c] + v[2] * m[2 * 4 + c] + m[3 * 4 + c];
}

bool matrix_is_mirrored(const float* m)
{
   const float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
                     m[2] * (m[4] * m[9] - m[5] * m[8]);
   return det < 0.0f;
}

// Draw the offhand model the way the engine draws the main one: hardpoint matrix * world,
// then the model's own Render. `outWorld` receives the offhand world matrix.
bool draw_offhand(void* model, uint32_t hardPoint, const float* world, void* pose,
                  const uint8_t* color, uint32_t flags, float* outWorld)
{
   if (!model || !hardPoint || !world || !pose || !color) return false;

   const auto* node = static_cast<const float*>(
      s_hashTableFind(static_cast<uint8_t*>(pose) + kPoseTable, kPoseTableSize, hardPoint));
   if (!node) return false;

   matrix_multiply(outWorld, node, world);

   const auto render =
      reinterpret_cast<fn_model_render_t>((*static_cast<void***>(model))[kSlotModelRender]);
   render(model, outWorld, 0, color, flags, 0);
   return true;
}

// The spawn screen preview: WeaponClass::Render has drawn the main model (or bailed on
// an invisible colour or a missing hp_weapons), so add the offhand under the same rules.
void __fastcall hooked_class_render(void* cls, void* edx, const float* world, void* pose,
                                    const uint8_t* color, uint32_t flags, uint32_t highRes)
{
   original_class_render(cls, edx, world, pose, color, flags, highRes);

   // WeaponClass::Render's own gates: a transparent draw or no pose draws nothing.
   if (!cls || !color || color[3] == 0 || !pose) return;

   void*    model     = nullptr;
   uint32_t hardPoint = 0;
   {
      std::lock_guard<std::mutex> lock(s_mutex);
      auto it = s_classes.find(cls);
      if (it == s_classes.end()) return;
      model     = it->second.model;
      hardPoint = it->second.hardPoint;
   }

   float offhandWorld[16];
   draw_offhand(model, hardPoint, world, pose, color, flags, offhandWorld);
}

// Weapon::Render draws the main gun at hp_weapons, the muzzle flash, and writes
// mFirePointMatrix. After it, draw the offhand model at the class's OffhandHardPoint
// the same way (hardpoint matrix * world, then the model's own Render), remember where
// its fire point landed, and draw the flash there when gun 2 fired last.
void __fastcall dual_render(void* weapon, void* /*edx*/, const float* world, void* pose,
                            const uint8_t* color, uint32_t flags, uint32_t highRes)
{
   const bool hidden = (at<uint32_t>(weapon, kWeaponHideFlags) & 1) != 0;

   void*    model     = nullptr;
   uint32_t hardPoint = 0;
   bool     hasFirePoint = false;
   float    firePointOffset[3] = {};
   uint8_t  lastFired = 0;
   {
      std::lock_guard<std::mutex> lock(s_mutex);
      auto cls = s_classes.find(at<void*>(weapon, kWeaponRenderClass));
      if (cls != s_classes.end()) {
         model        = cls->second.model;
         hardPoint    = cls->second.hardPoint;
         hasFirePoint = cls->second.firePointFound;
         std::memcpy(firePointOffset, cls->second.firePointOffset, sizeof(firePointOffset));
      }
      auto w = s_weapons.find(weapon);
      if (w != s_weapons.end()) lastFired = w->second.lastFired;
   }

   const bool offhandFlash = lastFired == 1 && model && hardPoint && hasFirePoint;

   // Keep the main gun from drawing gun 2's flash.
   float& flashStart = at<float>(weapon, kWeaponFlashStart);
   const float savedFlashStart = flashStart;
   if (offhandFlash) flashStart = 0.0f;
   s_baseRender(weapon, world, pose, color, flags, highRes);
   if (offhandFlash) flashStart = savedFlashStart;

   if (hidden) return;

   float offhandWorld[16];
   if (!draw_offhand(model, hardPoint, world, pose, color, flags, offhandWorld)) return;

   // Same bake conditions as Weapon::Render: invisible draws leave the fire point alone,
   // and a reflection region's mirrored duplicate must not overwrite the real one.
   if (color[3] == 0 || !hasFirePoint || matrix_is_mirrored(offhandWorld)) return;

   float firePos[3];
   transform_coord(firePos, firePointOffset, offhandWorld);
   {
      std::lock_guard<std::mutex> lock(s_mutex);
      weapon_ext& ext = s_weapons[weapon];
      std::memcpy(ext.offhandFirePos, firePos, sizeof(firePos));
      ext.offhandValid = true;
   }

   if (!offhandFlash) return;
   void* aimer = at<void*>(weapon, kWeaponAimer);
   void* cls   = at<void*>(weapon, kWeaponClass);
   if (!aimer || !cls) return;
   const float flashLength = at<float>(cls, kClassFlashLength);
   const float remaining   = savedFlashStart - s_missionTime();
   if (remaining > 0.0f && flashLength > 0.0f)
      s_renderFlash(at<void*>(weapon, kWeaponRenderClass), firePos,
                    &at<float>(aimer, kAimerDirection), remaining / flashLength);
}

bool __fastcall hooked_fire(void* weapon, void* edx)
{
   if (!is_dual_weapon(weapon)) return original_fire(weapon, edx);

   void* cls = at<void*>(weapon, kWeaponClass);
   if (!cls) return original_fire(weapon, edx);

   const float now        = s_missionTime();
   const int   salvoCount = at<int>(weapon, kWeaponSalvoCount);
   const int   perSalvo   = at<int>(cls, kClassShotsPerSalvo);
   const int   perShotRaw = at<int>(cls, kClassShotsPerShot);
   const unsigned perShot = perShotRaw > 0 ? static_cast<unsigned>(perShotRaw) : 1u;

   uint8_t barrel = 0;
   bool    offhandValid = false;
   float   offhandPos[3] = {};
   {
      std::lock_guard<std::mutex> lock(s_mutex);
      alternate_mode mode = alternate_mode::shot;
      auto c = s_classes.find(cls);
      if (c != s_classes.end()) mode = c->second.mode;

      weapon_ext& ext = s_weapons[weapon];

      // UpdateFire calls Fire once per ShotsPerShot pellet, all in the same call and
      // with mSalvoCount unchanged; a new shot differs in time, in salvo count, or has
      // already had all its pellets.
      const bool newShot = !ext.anyShot || now != ext.lastFireTime ||
                           salvoCount != ext.lastSalvoCount || ext.pellets >= perShot;
      if (newShot) {
         // mSalvoCount is reset to ShotsPerSalvo when a salvo starts, then counts down.
         const bool newSalvo = salvoCount == perSalvo;
         if (ext.anyShot &&
             (mode == alternate_mode::salvo || (mode == alternate_mode::shot && newSalvo)))
            ext.barrel ^= 1;
         ext.pellets = 0;
         ext.anyShot = true;
      }
      ++ext.pellets;
      ext.lastFireTime   = now;
      ext.lastSalvoCount = salvoCount;
      ext.lastFired      = ext.barrel;

      barrel       = ext.barrel;
      offhandValid = ext.offhandValid;
      std::memcpy(offhandPos, ext.offhandFirePos, sizeof(offhandPos));
   }

   // Pick shoot or shoot2. Only ever FIRE <-> FIRE2, which every consumer treats alike.
   uint32_t& state = at<uint32_t>(weapon, kWeaponState);
   if (state == kStateFire || state == kStateFire2)
      state = barrel == 1 ? kStateFire2 : kStateFire;

   void* aimer = at<void*>(weapon, kWeaponAimer);
   float newDir[3];
   const bool moveOrigin = barrel == 1 && offhandValid && aimer &&
                           barrel_fire_origin_aim_from(weapon, offhandPos, newDir);
   if (!moveOrigin) return original_fire(weapon, edx);

   // The next shot of this turn may be gun 1 again, so put the main muzzle back after.
   float* firePos = &at<float>(aimer, kAimerFirePos);
   float* dir     = &at<float>(aimer, kAimerDirection);
   float savedPos[3], savedDir[3];
   std::memcpy(savedPos, firePos, sizeof(savedPos));
   std::memcpy(savedDir, dir, sizeof(savedDir));

   std::memcpy(firePos, offhandPos, sizeof(offhandPos));
   std::memcpy(dir, newDir, sizeof(newDir));
   const bool fired = original_fire(weapon, edx);
   std::memcpy(firePos, savedPos, sizeof(savedPos));
   std::memcpy(dir, savedDir, sizeof(savedDir));
   return fired;
}

void __cdecl hooked_first_person_init()
{
   // See the header comment. Every slot Init leaves alone afterwards is refilled with
   // this level's humanfp_tool_idle, or stays null, which ZephyrAnimInst::SetAnim skips.
   std::memset(s_firstPersonAnims, 0, kFirstPersonAnimSlots * sizeof(void*));
   original_first_person_init();
}

void build_vtables()
{
   std::memcpy(s_classVtable, s_cannonClassVtable, sizeof(s_classVtable));
   std::memcpy(s_weaponVtable, s_cannonWeaponVtable, sizeof(s_weaponVtable));

   s_baseDestroyClass  = reinterpret_cast<fn_destroy_t>(s_classVtable[kSlotDestroyClass]);
   s_baseDerive        = reinterpret_cast<fn_derive_t>(s_classVtable[kSlotDerive]);
   s_baseBuild         = reinterpret_cast<fn_build_t>(s_classVtable[kSlotBuild]);
   s_baseSetProperty   = reinterpret_cast<fn_set_property_t>(s_classVtable[kSlotSetProperty]);
   s_baseDestroyWeapon = reinterpret_cast<fn_destroy_t>(s_weaponVtable[kSlotDestroyWeapon]);
   s_baseRender        = reinterpret_cast<fn_render_t>(s_weaponVtable[kSlotRender]);

   s_classVtable[kSlotDestroyClass]   = reinterpret_cast<void*>(&dual_destroy_class);
   s_classVtable[kSlotDerive]         = reinterpret_cast<void*>(&dual_derive);
   s_classVtable[kSlotBuild]          = reinterpret_cast<void*>(&dual_build);
   s_classVtable[kSlotSetProperty]    = reinterpret_cast<void*>(&dual_set_property);
   s_weaponVtable[kSlotDestroyWeapon] = reinterpret_cast<void*>(&dual_destroy_weapon);
   s_weaponVtable[kSlotRender]        = reinterpret_cast<void*>(&dual_render);

   s_vtablesReady = true;
}

void __cdecl hooked_create_base_classes()
{
   original_create_base_classes();

   if (!s_vtablesReady) build_vtables();

   {
      std::lock_guard<std::mutex> lock(s_mutex);
      s_classes.clear();
      s_weapons.clear();
   }
   barrel_fire_origin_reset_targets();

   void* cls = s_operatorNew(kCannonClassSize);
   if (!cls) {
      get_gamelog()("[DualCannon] out of memory registering ClassLabel \"dualcannon\"\n");
      return;
   }

   s_classCtor(cls, kDualCannonHash);
   *static_cast<void***>(cls) = s_classVtable;

   // Give the counter slot back; see the header comment.
   at<uint32_t>(cls, kFactoryNetIndex) = kNoNetIndex;
   --*s_factoryCounter;

   get_gamelog()("[DualCannon] ClassLabel \"dualcannon\" registered, net index %u, sCounter %u\n",
                 kNoNetIndex, *s_factoryCounter);
}

} // namespace

void dual_cannon_install(uintptr_t exe_base)
{
   HOOK_REQUIRE_MODTOOLS();

   if (!g_addr->game_state_create_base_weapon_classes || !g_addr->engine_operator_new ||
       !g_addr->weapon_cannon_class_ctor || !g_addr->weapon_cannon_class_vftable ||
       !g_addr->weapon_cannon_vftable || !g_addr->weapon_class_factory_counter ||
       !g_addr->red_model_find || !g_addr->red_model_get_parent_bone_and_offset ||
       !g_addr->pbl_hash_table_find || !g_addr->weapon_cannon_fire ||
       !g_addr->weapon_class_render_flash || !g_addr->game_loop_get_mission_time ||
       !g_addr->first_person_init || !g_addr->fp_anim_array || !g_addr->weapon_class_render) {
      install_log("[DualCannon] NOT installed: addresses unknown for this build");
      return;
   }

   // A mismatch in either prologue means the address is wrong for this exe, and
   // detouring it would be a guess.
   // CreateBaseWeaponClasses: PUSH ECX / PUSH ESI / PUSH 0x3DC.
   static constexpr uint8_t kCreatePrologue[] = {0x51, 0x56, 0x68, 0xDC, 0x03, 0x00, 0x00};
   // WeaponCannon::Fire: PUSH EBP / MOV EBP,ESP / AND ESP,-16 / SUB ESP,0xC4.
   static constexpr uint8_t kFirePrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0,
                                               0x81, 0xEC, 0xC4, 0x00, 0x00, 0x00};

   // FirstPerson::Init: CALL rel32 (to thunk 0x411095) / TEST AL,AL / JE near.
   static constexpr uint8_t kFirstPersonInitPrologue[] = {0xE8, 0x00, 0x5B, 0xF6, 0xFF,
                                                          0x84, 0xC0, 0x0F, 0x84};

   void* createTarget = resolve(exe_base, g_addr->game_state_create_base_weapon_classes);
   void* fireTarget   = resolve(exe_base, g_addr->weapon_cannon_fire);
   // WeaponClass::Render: PUSH EBP / MOV EBP,ESP / AND ESP,-16 / SUB ESP,0x84.
   static constexpr uint8_t kClassRenderPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0,
                                                      0x81, 0xEC, 0x84, 0x00, 0x00, 0x00};

   void* classRenderTarget = resolve(exe_base, g_addr->weapon_class_render);
   if (std::memcmp(classRenderTarget, kClassRenderPrologue, sizeof(kClassRenderPrologue)) != 0) {
      install_log("[DualCannon] NOT installed: unexpected bytes at WeaponClass::Render %08X",
                  (unsigned)g_addr->weapon_class_render);
      return;
   }
   void* fpInitTarget = resolve(exe_base, g_addr->first_person_init);
   if (std::memcmp(fpInitTarget, kFirstPersonInitPrologue, sizeof(kFirstPersonInitPrologue)) != 0) {
      install_log("[DualCannon] NOT installed: unexpected bytes at FirstPerson::Init %08X",
                  (unsigned)g_addr->first_person_init);
      return;
   }
   if (std::memcmp(createTarget, kCreatePrologue, sizeof(kCreatePrologue)) != 0) {
      install_log("[DualCannon] NOT installed: unexpected bytes at CreateBaseWeaponClasses %08X",
                  (unsigned)g_addr->game_state_create_base_weapon_classes);
      return;
   }
   if (std::memcmp(fireTarget, kFirePrologue, sizeof(kFirePrologue)) != 0) {
      install_log("[DualCannon] NOT installed: unexpected bytes at WeaponCannon::Fire %08X",
                  (unsigned)g_addr->weapon_cannon_fire);
      return;
   }

   s_operatorNew   = reinterpret_cast<fn_operator_new_t>(resolve(exe_base, g_addr->engine_operator_new));
   s_classCtor     = reinterpret_cast<fn_class_ctor_t>(resolve(exe_base, g_addr->weapon_cannon_class_ctor));
   s_findModel     = reinterpret_cast<fn_find_model_t>(resolve(exe_base, g_addr->red_model_find));
   s_getHardPoint  = reinterpret_cast<fn_get_hardpoint_t>(
      resolve(exe_base, g_addr->red_model_get_parent_bone_and_offset));
   s_hashTableFind = reinterpret_cast<fn_hash_table_find_t>(resolve(exe_base, g_addr->pbl_hash_table_find));
   s_renderFlash   = reinterpret_cast<fn_render_flash_t>(resolve(exe_base, g_addr->weapon_class_render_flash));
   s_missionTime   = reinterpret_cast<fn_mission_time_t>(resolve(exe_base, g_addr->game_loop_get_mission_time));
   s_cannonClassVtable  = static_cast<void**>(resolve(exe_base, g_addr->weapon_cannon_class_vftable));
   s_cannonWeaponVtable = static_cast<void**>(resolve(exe_base, g_addr->weapon_cannon_vftable));
   s_factoryCounter =
      static_cast<uint32_t*>(resolve(exe_base, g_addr->weapon_class_factory_counter));

   original_create_base_classes = reinterpret_cast<fn_create_base_classes_t>(createTarget);
   original_fire                = reinterpret_cast<fn_fire_t>(fireTarget);
   original_first_person_init   = reinterpret_cast<fn_first_person_init_t>(fpInitTarget);
   original_class_render        = reinterpret_cast<fn_class_render_t>(classRenderTarget);
   s_firstPersonAnims = static_cast<void**>(resolve(exe_base, g_addr->fp_anim_array));

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_create_base_classes, hooked_create_base_classes);
   DetourAttach(&(PVOID&)original_fire, hooked_fire);
   DetourAttach(&(PVOID&)original_first_person_init, hooked_first_person_init);
   DetourAttach(&(PVOID&)original_class_render, hooked_class_render);
   if (DetourTransactionCommit() != NO_ERROR) {
      original_create_base_classes = nullptr;
      original_fire                = nullptr;
      original_first_person_init   = nullptr;
      original_class_render        = nullptr;
      install_log("[DualCannon] NOT installed: detours failed");
      return;
   }

   install_log("[DualCannon] installed: ClassLabel \"dualcannon\" registers on mission load");
}

void dual_cannon_uninstall()
{
   if (!original_create_base_classes) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_create_base_classes, hooked_create_base_classes);
   DetourDetach(&(PVOID&)original_fire, hooked_fire);
   DetourDetach(&(PVOID&)original_first_person_init, hooked_first_person_init);
   DetourDetach(&(PVOID&)original_class_render, hooked_class_render);
   DetourTransactionCommit();
   original_create_base_classes = nullptr;
   original_fire                = nullptr;
   original_first_person_init   = nullptr;
   original_class_render        = nullptr;
}
