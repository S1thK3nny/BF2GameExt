#include "pch.h"
#include "directional_rolls.hpp"
#include "directional_rolls_core.hpp"
#include "soldier_anim_tables.hpp"
#include "core/entity_layout.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "entity/odf_gameext_props.hpp"
#include "game/Battlefront2/Source/SoldierAnimator.h"
#include "util/install_log.hpp"

#include <detours.h>

#include <cstdlib>
#include <unordered_set>

// See directional_rolls.hpp for the mechanism.

namespace {

using namespace directional_rolls;
namespace sa     = layout::SoldierAnimator;
namespace tables = soldier_anim_tables;

constexpr uint32_t kUseDirectionalRolls = pbl_hash("UseDirectionalRolls");
static_assert(kUseDirectionalRolls == 0x3F77C468u);

// A soldier's Controllable part, where g_soldier's offsets start.
constexpr uint32_t kSoldierControllable = 0x240;

using SetupPoseFn = void(__fastcall*)(uint8_t* self, void* edx, void* pose);

SetupPoseFn  s_setupPose = nullptr;
const float* s_frameTime = nullptr;   // GameLoop::sClientDeltaTime

// ---------------------------------------------------------------------------
// The ODF property, per class
// ---------------------------------------------------------------------------

std::unordered_set<const void*> s_classes;   // classes with UseDirectionalRolls on

bool on_property(void* cls, uint32_t hash, const char* value)
{
   if (hash != kUseDirectionalRolls) return false;
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

bool class_rolls_directionally(const uint8_t* animator)
{
   if (s_classes.empty()) return false;
   const uint8_t* soldier = sa::mOwner(animator);
   if (!soldier) return false;
   const void* cls = *reinterpret_cast<void* const*>(soldier + kSoldierControllable + g_soldier->classPtr);
   return cls && s_classes.count(cls) != 0;
}

// ---------------------------------------------------------------------------
// The side dives, per animation map
// ---------------------------------------------------------------------------

// One side's dive for one map: copies of the map's DIVE entries carrying the
// side dive, and what they were found by.
struct SideDive {
   bool                    built = false;
   Side                    side = Side::Forward;
   const SoldierAnimation* from[2] = {};   // the DIVE entries the copies came from
   bool                    found[2] = {};
   SoldierAnimation        anim[2] = {};
   char                    name[2][tables::kNameMax] = {};
};
SideDive s_dives[tables::kMaxMapsAny][2];   // [map][left, right]

// The scope a found half gets, as AnimationFinder::AssignAnimation gives it:
// the half's own (1 upper, 2 lower), "_full"'s, or the action's own for the
// plain name, which is diveforward's.
uint32_t scope_of(tables::Match match, int half, uint32_t from)
{
   if (match == tables::Match::Half) return half == 0 ? 1u : 2u;
   if (match == tables::Match::Full) return 3u;
   return from >> 30;
}

void report(int map, Side side, const SideDive& d)
{
   int bank = 0, weapon = 0;
   if (!tables::map_key(map, bank, weapon)) return;
   char bankName[tables::kNameBuffer], weaponName[tables::kNameBuffer];
   tables::bank_name(bank, bankName);
   tables::weapon_name(weapon, weaponName);
   auto log = get_gamelog();
   if (!d.found[0] && !d.found[1])
      log("[DirectionalRolls] %s_%s has no %s: those rolls play diveforward\n", bankName, weaponName,
          dive_name(side));
   else
      log("[DirectionalRolls] %s_%s %s: upper body %s, lower body %s\n", bankName, weaponName,
          dive_name(side), d.found[0] ? d.name[0] : "diveforward", d.found[1] ? d.name[1] : "diveforward");
}

// A map's side dive, built the first time a roll needs it and again when the
// map's DIVE entries change. Null when the map has none.
SideDive* side_dive(int map, Side side)
{
   if (map < 0 || map >= tables::max_maps()) return nullptr;
   const SoldierAnimation* const stock[2] = { tables::action_animation(map, sa::kActionDive, 0),
                                              tables::action_animation(map, sa::kActionDive, 1) };
   SideDive& d = s_dives[map][side == Side::Left ? 0 : 1];
   if (!d.built || d.from[0] != stock[0] || d.from[1] != stock[1]) {
      d = SideDive{};
      d.built = true;
      d.side = side;
      for (int half = 0; half < 2; ++half) {
         const SoldierAnimation* from = stock[half];
         d.from[half] = from;
         if (!from) continue;
         tables::Match match = tables::Match::Plain;
         void* anim = tables::find_named(map, dive_name(side), half, d.name[half], match);
         if (!anim) continue;
         d.anim[half] = *from;
         d.anim[half].m_pZephyrAnim = anim;
         d.anim[half].m_uiData = (from->m_uiData & 0x3FFFFFFFu) | (scope_of(match, half, from->m_uiData) << 30);
         d.anim[half].mName = d.name[half];
         d.found[half] = true;
      }
      if (d.from[0] || d.from[1]) report(map, side, d);
   }
   return d.found[0] || d.found[1] ? &d : nullptr;
}

// ---------------------------------------------------------------------------
// The way each roll goes, per animator
// ---------------------------------------------------------------------------

struct Roll {
   const uint8_t* animator;
   float          time;   // mActionTime when last seen
   Side           side;
};
constexpr int kMaxRolls = 64;
Roll s_rolls[kMaxRolls];
int  s_rollCount = 0;   // in use, packed at the front

Roll* find_roll(const uint8_t* animator)
{
   for (int i = 0; i < s_rollCount; ++i)
      if (s_rolls[i].animator == animator) return &s_rolls[i];
   return nullptr;
}

void forget_roll(const uint8_t* animator)
{
   for (int i = 0; i < s_rollCount; ++i) {
      if (s_rolls[i].animator == animator) {
         s_rolls[i] = s_rolls[--s_rollCount];
         return;
      }
   }
}

// The side dive a rolling soldier plays this frame, or null for the stock
// dive. Decided once per roll: on the first frame, and again only when the
// roll's time starts over.
SideDive* roll_dive(const uint8_t* animator)
{
   const float time = sa::mActionTime(animator);
   Roll* roll = find_roll(animator);
   if (!roll || time < roll->time) {
      Side side = Side::Forward;
      if (class_rolls_directionally(animator)) {
         const float dt = *s_frameTime;
         if (!(dt > 0.0f)) return nullptr;   // paused: decide on a running frame
         side = choose_side(sa::mMovement(animator), sa::mLegMatrix_right(animator),
                            sa::mLegMatrix_forward(animator), dt);
      }
      if (!roll) roll = &s_rolls[s_rollCount < kMaxRolls ? s_rollCount++ : kMaxRolls - 1];
      roll->animator = animator;
      roll->side = side;
   }
   roll->time = time;
   return roll->side == Side::Forward ? nullptr : side_dive(sa::mWeaponAnimationMap(animator), roll->side);
}

void __fastcall hooked_SetupPose(uint8_t* self, void* edx, void* pose)
{
   SideDive* dive = nullptr;
   if (sa::mAction(self) == sa::kActionDive && sa::mSoldierAction(self) == sa::kStateRoll)
      dive = roll_dive(self);
   else if (s_rollCount != 0)
      forget_roll(self);
   if (!dive) {
      s_setupPose(self, edx, pose);
      return;
   }

   // The side dive in the map's DIVE entries, for this soldier only.
   const int map = sa::mWeaponAnimationMap(self);
   SoldierAnimation** const slot[2] = { tables::action_slot(map, sa::kActionDive, 0),
                                        tables::action_slot(map, sa::kActionDive, 1) };
   SoldierAnimation* const stock[2] = { slot[0] ? *slot[0] : nullptr, slot[1] ? *slot[1] : nullptr };
   for (int half = 0; half < 2; ++half)
      if (dive->found[half] && slot[half]) *slot[half] = &dive->anim[half];

   // SetupPose aims the diving root at the move; turned a quarter turn back,
   // the move aims a side dive's root so the dive follows the soldier's real
   // move, through turns, as a forward dive does.
   float* move = sa::mMovement(self);
   const float moved[3] = { move[0], move[1], move[2] };
   side_dive_aim(moved, sa::mLegMatrix_right(self), sa::mLegMatrix_forward(self), dive->side, move);

   s_setupPose(self, edx, pose);

   for (int i = 0; i < 3; ++i) move[i] = moved[i];
   for (int half = 0; half < 2; ++half)
      if (slot[half]) *slot[half] = stock[half];
}

bool guard(uintptr_t base, uintptr_t va, const char* what, const char* bytes, const char* mask)
{
   const auto* code = static_cast<const unsigned char*>(resolve(base, va));
   for (size_t i = 0; mask[i]; ++i) {
      if (mask[i] == 'x' && code[i] != static_cast<unsigned char>(bytes[i])) {
         install_log("[DirectionalRolls] NOT installed: unexpected code at %s 0x%08X", what, (unsigned)va);
         return false;
      }
   }
   return true;
}

} // namespace

void directional_rolls_install(uintptr_t base)
{
   const bool modtools = g_build == GameBuild::Modtools;
   if (!modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;
   const auto* a = g_addr;
   if (!a->soldier_animator_setup_pose || !a->game_client_delta_time) {
      install_log("[DirectionalRolls] NOT installed: no address set for this build");
      return;
   }
   if (!tables::init(base)) {
      install_log("[DirectionalRolls] NOT installed: the soldier animation tables are unavailable");
      return;
   }
   if (!guard(base, a->soldier_animator_setup_pose, "SoldierAnimator::SetupPose",
              modtools ? "\x81\xEC\xD8\x0A\x00\x00\x56\x57\x8B\xF1"
                       : "\x55\x8B\xEC\x83\xE4\xF8\x64\xA1\x00\x00\x00\x00\x6A\xFF",
              modtools ? "xxxxxxxxxx" : "xxxxxxxxxxxxxx"))
      return;

   s_frameTime = static_cast<const float*>(resolve(base, a->game_client_delta_time));

   if (!odf_add_property_handler(on_property) || !odf_add_derive_handler(on_derive)) {
      install_log("[DirectionalRolls] NOT installed: no ODF listener slot left");
      return;
   }

   s_setupPose = reinterpret_cast<SetupPoseFn>(resolve(base, a->soldier_animator_setup_pose));
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG r = DetourAttach(&(PVOID&)s_setupPose, hooked_SetupPose);
   if (r != NO_ERROR || DetourTransactionCommit() != NO_ERROR) {
      if (r != NO_ERROR) DetourTransactionAbort();
      s_setupPose = nullptr;
      install_log("[DirectionalRolls] NOT installed: detouring SoldierAnimator::SetupPose failed (%ld)", (long)r);
      return;
   }
   install_log("[DirectionalRolls] installed (SetupPose 0x%08X, %s animation tables): UseDirectionalRolls "
               "soldiers roll to the side with diveleft and diveright", (unsigned)a->soldier_animator_setup_pose,
               tables::expanded() ? "ComboAnimIncrease's" : "the stock");
}

void directional_rolls_uninstall()
{
   if (!s_setupPose) return;
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)s_setupPose, hooked_SetupPose);
   DetourTransactionCommit();
   s_setupPose = nullptr;
   directional_rolls_reset();
   s_classes.clear();
}

void directional_rolls_reset()
{
   for (auto& map : s_dives)
      for (SideDive& d : map) d.built = false;
   s_rollCount = 0;
}
