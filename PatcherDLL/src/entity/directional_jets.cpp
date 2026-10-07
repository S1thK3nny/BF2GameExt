#include "pch.h"
#include "directional_jets.hpp"
#include "directional_jets_core.hpp"
#include "soldier_anim_tables.hpp"
#include "core/entity_layout.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "entity/odf_gameext_props.hpp"
#include "game/Battlefront2/Source/EntitySoldierClass.h"
#include "game/Battlefront2/Source/SoldierAnimator.h"
#include "game/Battlefront2/Source/Zephyr.h"
#include "util/install_log.hpp"

#include <cstdlib>
#include <cstring>
#include <unordered_set>

// See directional_jets.hpp for the mechanism.

namespace {

using namespace directional_jets;
namespace sa     = layout::SoldierAnimator;
namespace sc     = layout::EntitySoldierClass;
namespace zpd    = layout::ZephyrPoseDyn;
namespace za     = layout::ZephyrAnim;
namespace zs     = layout::ZephyrSkeleton;
namespace tables = soldier_anim_tables;

constexpr uint32_t kUseDirectionalJets = pbl_hash("UseDirectionalJets");
static_assert(kUseDirectionalJets == 0xECB123E4u);

// A soldier's Controllable part, where g_soldier's offsets start.
constexpr uint32_t kSoldierControllable = 0x240;

// The lower half of the body, as soldier_anim_tables numbers the halves.
constexpr int kLower = 1;

// The rate SetupPose's own players are set up with.
constexpr float kAnimFps = 30.0f;

// ZephyrPoseDyn<32>::SetAnimation: thiscall(ZephyrAnim*, float fps), RET 8.
using SetAnimationFn = void(__fastcall*)(void* dyn, void* edx, void* anim, float fps);
// ZephyrPoseStatic<32>::Blend(ZephyrPoseDyn<32>*, PblBitVector<32>*, float):
// thiscall, RET 0xC.
using BlendFn = void(__fastcall*)(void* pose, void* edx, void* dyn, const uint32_t* mask, float t);

SetAnimationFn s_setAnimation = nullptr;
BlendFn        s_blend        = nullptr;
const float*   s_frameTime    = nullptr;   // GameLoop::sClientDeltaTime
uint8_t*       s_callSite     = nullptr;   // SetupPose's call, while it is retargeted
int32_t        s_callRel      = 0;         // the call's own displacement
bool           s_faulted      = false;     // a blend faulted; logged once

// ---------------------------------------------------------------------------
// The ODF property, per class
// ---------------------------------------------------------------------------

std::unordered_set<const void*> s_classes;   // classes with UseDirectionalJets on

bool on_property(void* cls, uint32_t hash, const char* value)
{
   if (hash != kUseDirectionalJets) return false;
   if (cls && value) {
      if (std::atoi(value) != 0) s_classes.insert(cls);
      else                       s_classes.erase(cls);
   }
   return true;   // no stock SetProperty knows it
}

void on_derive(const void* parent, void* child)
{
   if (s_classes.empty()) return;
   if (parent && s_classes.count(parent)) s_classes.insert(child);
   else                                   s_classes.erase(child);
}

// The soldier's class when it has UseDirectionalJets on, else null.
const uint8_t* jet_class(const uint8_t* animator)
{
   if (s_classes.empty()) return nullptr;
   const uint8_t* soldier = sa::mOwner(animator);
   if (!soldier) return nullptr;
   const auto* cls = *reinterpret_cast<const uint8_t* const*>(soldier + kSoldierControllable + g_soldier->classPtr);
   return cls && s_classes.count(cls) ? cls : nullptr;
}

// ---------------------------------------------------------------------------
// The four ways' animations, per animation map
// ---------------------------------------------------------------------------

struct JetSet {
   bool                    built = false;
   const SoldierAnimation* from = nullptr;   // the map's JET lower entry when built
   bool                    have[kDirs] = {};
   void*                   anim[kDirs] = {};
   char                    name[kDirs][tables::kNameMax] = {};
   uint16_t                joints[kDirs] = {};   // of one found but too big to play
};
JetSet s_sets[tables::kMaxMapsAny];

// Whether a player of ours can play the animation: one frame at least, and no
// more joints than ZephyrAnimInst<32>'s tables hold.
bool playable(const void* anim)
{
   const uint16_t joints = za::m_u16NumJoints(anim);
   return za::m_u16NumFrames(anim) >= 1 && joints >= 1 && joints <= za::kMaxJoints;
}

void report(int map, const JetSet& set)
{
   int bank = 0, weapon = 0;
   if (!tables::map_key(map, bank, weapon)) return;
   char bankName[tables::kNameBuffer], weaponName[tables::kNameBuffer];
   tables::bank_name(bank, bankName);
   tables::weapon_name(weapon, weaponName);
   auto log = get_gamelog();
   bool any = false;
   for (int d = 0; d < kDirs; ++d) any |= set.have[d] || set.joints[d] != 0;
   if (!any) {
      log("[DirectionalJets] %s_%s has no jetpack_hover_forward, _backward, _left or _right: its jets play "
          "jetpack_hover\n", bankName, weaponName);
      return;
   }
   for (int d = 0; d < kDirs; ++d) {
      if (set.have[d])
         log("[DirectionalJets] %s_%s %s: %s\n", bankName, weaponName, anim_name(d), set.name[d]);
      else if (set.joints[d])
         log("[DirectionalJets] %s_%s %s: %s has %u joints, more than %u: not played\n", bankName, weaponName,
             anim_name(d), set.name[d], (unsigned)set.joints[d], (unsigned)za::kMaxJoints);
      else
         log("[DirectionalJets] %s_%s %s: none, that way keeps jetpack_hover\n", bankName, weaponName, anim_name(d));
   }
}

// A map's set, built the first time a jet needs it and again when the map's
// JET lower entry changes. Null when it has none of the four.
const JetSet* jet_set(int map, const SoldierAnimation* hover)
{
   if (map < 0 || map >= tables::max_maps()) return nullptr;
   JetSet& set = s_sets[map];
   if (!set.built || set.from != hover) {
      set = JetSet{};
      set.built = true;
      set.from = hover;
      for (int d = 0; d < kDirs; ++d) {
         tables::Match match = tables::Match::Plain;
         void* anim = tables::find_named(map, anim_name(d), kLower, set.name[d], match);
         if (!anim) continue;
         if (playable(anim)) {
            set.anim[d] = anim;
            set.have[d] = true;
         } else {
            set.joints[d] = za::m_u16NumJoints(anim);
         }
      }
      report(map, set);
   }
   for (int d = 0; d < kDirs; ++d)
      if (set.have[d]) return &set;
   return nullptr;
}

// ---------------------------------------------------------------------------
// The lean each jetting soldier's legs show, per animator
// ---------------------------------------------------------------------------

struct Jetting {
   const uint8_t* animator;
   const uint8_t* owner;
   Lean           shown;
   uint32_t       stamp;   // when last drawn, to reuse the oldest when full
};
constexpr int kMaxJetting = 64;
Jetting  s_jetting[kMaxJetting];
int      s_jettingCount = 0;   // in use, packed at the front
uint32_t s_stamp = 0;

Jetting* find_jetting(const uint8_t* animator)
{
   for (int i = 0; i < s_jettingCount; ++i)
      if (s_jetting[i].animator == animator) return &s_jetting[i];
   return nullptr;
}

void forget_jetting(const uint8_t* animator)
{
   for (int i = 0; i < s_jettingCount; ++i) {
      if (s_jetting[i].animator == animator) {
         s_jetting[i] = s_jetting[--s_jettingCount];
         return;
      }
   }
}

// A new entry; when all are in use, the one drawn longest ago, a soldier no
// longer drawn.
Jetting* add_jetting(const uint8_t* animator, const uint8_t* owner)
{
   Jetting* j = nullptr;
   if (s_jettingCount < kMaxJetting) {
      j = &s_jetting[s_jettingCount++];
   } else {
      j = &s_jetting[0];
      for (int i = 1; i < kMaxJetting; ++i)
         if (s_stamp - s_jetting[i].stamp > s_stamp - j->stamp) j = &s_jetting[i];
   }
   j->animator = animator;
   j->owner = owner;
   j->shown = Lean{};
   return j;
}

float dot3(const float* a, const float* b)
{
   return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// One way's animation laid on the animator's local pose, `t` of the way, at
// the hover's time, through the lower-body mask: a ZephyrPoseDyn<32> set up as
// SetupPose sets up its overlay player, but never looping (Zephyr.h).
void blend_way(uint8_t* animator, void* anim, float phase, float t)
{
   alignas(16) uint8_t dyn[zpd::kSize];
   std::memset(dyn, 0, sizeof dyn);
   zpd::m_pSkel(dyn) = &sa::mZephyrSkeleton(animator);
   zpd::m_bInterpolate(dyn) = 1;
   s_setAnimation(dyn, nullptr, anim, kAnimFps);
   zpd::m_fCurT(dyn) = phase;
   s_blend(&sa::mZephyrPoseStatic(animator), nullptr, dyn, &sa::mLowerBodyAnimMask(animator), t);
}

void blend_legs(uint8_t* animator)
{
   if (sa::mAction(animator) != sa::kActionJet) {
      if (s_jettingCount != 0) forget_jetting(animator);
      return;
   }
   const uint8_t* cls = jet_class(animator);
   const int map = sa::mWeaponAnimationMap(animator);
   const SoldierAnimation* hover = cls ? tables::action_animation(map, sa::kActionJet, kLower) : nullptr;
   // The hover on the legs: not another animation there, such as a melee swing.
   const JetSet* set = hover && sa::m_pAnimLower(animator) == hover ? jet_set(map, hover) : nullptr;
   if (!set || !zs::m_pShared(&sa::mZephyrSkeleton(animator))) {
      if (s_jettingCount != 0) forget_jetting(animator);
      return;
   }

   const uint8_t* owner = sa::mOwner(animator);
   Jetting* j = find_jetting(animator);
   if (!j) {
      j = add_jetting(animator, owner);
   } else if (j->owner != owner) {
      j->owner = owner;
      j->shown = Lean{};
   }
   j->stamp = ++s_stamp;

   const float dt = *s_frameTime;
   if (dt > 0.0f) {
      const float* move = sa::mMovement(animator);
      const float f = dot3(move, sa::mLegMatrix_forward(animator)) / dt;
      const float s = dot3(move, sa::mLegMatrix_right(animator)) / dt;
      const TopSpeeds top = top_speeds(sc::mMaxSpeed(cls), sc::mMaxStrafeSpeed(cls), sc::mThrustFactorJet(cls),
                                       sc::mStrafeFactorJet(cls));
      j->shown = follow(j->shown, lean_of(f, s, top), dt);
   }

   Step steps[kDirs];
   const int n = blend_steps(weights_of(j->shown), set->have, steps);
   if (n == 0) return;
   float phase = zpd::m_fCurT(&sa::mZephyrPoseDynLower(animator));
   if (!(phase >= 0.0f)) phase = 0.0f;
   if (phase > 1.0f) phase = 1.0f;
   for (int i = 0; i < n; ++i) blend_way(animator, set->anim[steps[i].dir], phase, steps[i].t);
}

void note_fault()
{
   get_gamelog()("[DirectionalJets] a blend faulted; jetting soldiers it reaches show jetpack_hover alone\n");
}

// What the stand-in calls. A fault leaves the soldier with the hover alone,
// and is logged once a level.
void __cdecl blend_legs_guarded(uint8_t* animator)
{
   __try {
      blend_legs(animator);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      if (!s_faulted) {
         s_faulted = true;
         note_fault();
      }
   }
}

// SetupPose's call of ApplyProceduralAnimationAndBuildWorldMatrices lands
// here. Modtools passes the frame time on the stack, Steam and GOG in XMM1;
// every register and the stack come back as they were, then the call goes on.
void* s_applyProcedural = nullptr;

__declspec(naked) void apply_procedural_stand_in()
{
   __asm {
      pushad
      sub    esp, 0x80
      movups [esp + 0x00], xmm0
      movups [esp + 0x10], xmm1
      movups [esp + 0x20], xmm2
      movups [esp + 0x30], xmm3
      movups [esp + 0x40], xmm4
      movups [esp + 0x50], xmm5
      movups [esp + 0x60], xmm6
      movups [esp + 0x70], xmm7
      push   ecx                          // the animator
      call   blend_legs_guarded
      add    esp, 4
      movups xmm0, [esp + 0x00]
      movups xmm1, [esp + 0x10]
      movups xmm2, [esp + 0x20]
      movups xmm3, [esp + 0x30]
      movups xmm4, [esp + 0x40]
      movups xmm5, [esp + 0x50]
      movups xmm6, [esp + 0x60]
      movups xmm7, [esp + 0x70]
      add    esp, 0x80
      popad
      jmp    [s_applyProcedural]
   }
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

bool guard(uintptr_t base, uintptr_t va, const char* what, const char* bytes, const char* mask)
{
   const auto* code = static_cast<const unsigned char*>(resolve(base, va));
   for (size_t i = 0; mask[i]; ++i) {
      if (mask[i] == 'x' && code[i] != static_cast<unsigned char>(bytes[i])) {
         install_log("[DirectionalJets] NOT installed: unexpected code at %s 0x%08X", what, (unsigned)va);
         return false;
      }
   }
   return true;
}

// Whether the CALL at `site` goes to `fn`, directly or through one JMP thunk.
bool calls(uintptr_t base, uintptr_t site, uintptr_t fn)
{
   const auto* call = static_cast<const uint8_t*>(resolve(base, site));
   if (call[0] != 0xE8) return false;
   int32_t rel;
   std::memcpy(&rel, call + 1, sizeof rel);
   const uint8_t* target = call + 5 + rel;
   const auto* want = static_cast<const uint8_t*>(resolve(base, fn));
   if (target == want) return true;
   if (target[0] != 0xE9) return false;
   std::memcpy(&rel, target + 1, sizeof rel);
   return target + 5 + rel == want;
}

} // namespace

void directional_jets_install(uintptr_t base)
{
   const bool modtools = g_build == GameBuild::Modtools;
   if (!modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;
   const auto* a = g_addr;
   if (!a->soldier_animator_apply_procedural || !a->soldier_animator_apply_procedural_call ||
       !a->zephyr_pose_dyn_set_anim || !a->zephyr_pose_static_blend_masked || !a->game_client_delta_time) {
      install_log("[DirectionalJets] NOT installed: no address set for this build");
      return;
   }
   if (!tables::init(base)) {
      install_log("[DirectionalJets] NOT installed: the soldier animation tables are unavailable");
      return;
   }
   // Retail's guards skip the stack cookie's address, which moves with the exe.
   if (!guard(base, a->soldier_animator_apply_procedural,
              "SoldierAnimator::ApplyProceduralAnimationAndBuildWorldMatrices",
              modtools ? "\x55\x8B\xEC\x83\xE4\xF0\x81\xEC\xD4\x01\x00\x00\x53\x8B\xD9"
                       : "\x55\x8B\xEC\x83\xE4\xF0\x81\xEC\xD8\x00\x00\x00\x56\x57\x8B\xF9\xF3\x0F\x11\x4C\x24\x38",
              modtools ? "xxxxxxxxxxxxxxx" : "xxxxxxxxxxxxxxxxxxxxxx") ||
       !guard(base, a->zephyr_pose_dyn_set_anim, "ZephyrPoseDyn<32>::SetAnimation",
              modtools ? "\x8B\x44\x24\x04\x83\xEC\x1C\x53\x33\xDB\x3B\xC3\x56\x8B\xF1"
                       : "\x55\x8B\xEC\x83\xEC\x20\xA1\x00\x00\x00\x00\x33\xC5\x89\x45\xFC\x8B\x45\x08\x56\x8B\xF1"
                         "\xFF\xB6\x88\x09\x00\x00",
              modtools ? "xxxxxxxxxxxxxxx" : "xxxxxxx????xxxxxxxxxxxxxxxxx") ||
       !guard(base, a->zephyr_pose_static_blend_masked, "ZephyrPoseStatic<32>::Blend",
              modtools ? "\x83\xEC\x3C\x55\x57\x8B\xF9\x8B\x07"
                       : "\x55\x8B\xEC\x83\xEC\x48\xA1\x00\x00\x00\x00\x33\xC5\x89\x45\xFC\x8B\x01\xF3\x0F\x10\x45\x10"
                         "\x8B\x55\x08",
              modtools ? "xxxxxxxxx" : "xxxxxxx????xxxxxxxxxxxxxxx"))
      return;
   if (!calls(base, a->soldier_animator_apply_procedural_call, a->soldier_animator_apply_procedural)) {
      install_log("[DirectionalJets] NOT installed: SetupPose's call 0x%08X is not as expected",
                  (unsigned)a->soldier_animator_apply_procedural_call);
      return;
   }

   s_frameTime       = static_cast<const float*>(resolve(base, a->game_client_delta_time));
   s_setAnimation    = reinterpret_cast<SetAnimationFn>(resolve(base, a->zephyr_pose_dyn_set_anim));
   s_blend           = reinterpret_cast<BlendFn>(resolve(base, a->zephyr_pose_static_blend_masked));
   s_applyProcedural = resolve(base, a->soldier_animator_apply_procedural);

   if (!odf_add_property_handler(on_property) || !odf_add_derive_handler(on_derive)) {
      install_log("[DirectionalJets] NOT installed: no ODF listener slot left");
      return;
   }

   s_callSite = static_cast<uint8_t*>(resolve(base, a->soldier_animator_apply_procedural_call));
   std::memcpy(&s_callRel, s_callSite + 1, sizeof s_callRel);
   const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&apply_procedural_stand_in) -
                                            reinterpret_cast<uintptr_t>(s_callSite + 5));
   std::memcpy(s_callSite + 1, &rel, sizeof rel);
   install_log("[DirectionalJets] installed (SetupPose's call 0x%08X, %s animation tables): UseDirectionalJets "
               "soldiers' legs lean with jetpack_hover_forward, _backward, _left and _right",
               (unsigned)a->soldier_animator_apply_procedural_call,
               tables::expanded() ? "ComboAnimIncrease's" : "the stock");
}

void directional_jets_uninstall()
{
   if (!s_callSite) return;
   // Past the install the code is protected again.
   DWORD old = 0;
   if (VirtualProtect(s_callSite + 1, sizeof s_callRel, PAGE_EXECUTE_READWRITE, &old)) {
      std::memcpy(s_callSite + 1, &s_callRel, sizeof s_callRel);
      VirtualProtect(s_callSite + 1, sizeof s_callRel, old, &old);
      FlushInstructionCache(GetCurrentProcess(), s_callSite, 5);
   }
   s_callSite = nullptr;
   directional_jets_reset();
   s_classes.clear();
}

void directional_jets_reset()
{
   for (JetSet& set : s_sets) set.built = false;
   s_jettingCount = 0;
   s_faulted = false;
}
