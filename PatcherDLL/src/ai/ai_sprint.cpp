#include "pch.h"
#include "ai_sprint.hpp"
#include "ai_sprint_core.hpp"
#include "core/entity_layout.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <detours.h>

#include <cmath>

// See ai_sprint.hpp for the behaviour and the per-build reads.

bool  g_aiSprint         = true;
float g_aiSprintDistance = 30.0f;

namespace {

// ---- Layout, the same on modtools, Steam and GOG unless split --------------
constexpr uint32_t kCtrl_Controller     = 0xC8;   // Controllable::mCtrl
constexpr int      kCtrl_Object         = -0x240; // the soldier's GameObject
constexpr uint32_t kGO_MatrixTrans      = 0x120;  // mMatrix (+0xF0) .trans
constexpr uint32_t kGO_HandleId         = 0x204;
constexpr uint32_t kUC_State            = 0x7C;   // CommandFsm::mCurState
constexpr uint32_t kUC_LowLevel         = 0x2C4;  // AILowLevel, embedded
constexpr uint32_t kLL_Navigator        = 0x18;
constexpr uint32_t kLL_Height           = 0x6C;
constexpr uint32_t kLL_Target           = 0xA0;   // PblHandle<GameObject const>
constexpr uint32_t kNav_PathFollower    = 0x10;
constexpr uint32_t kGraph_GlobalDest    = 0x24;   // NavigatorGraphFollower::mGlobalDest
constexpr uint32_t kPF_FinalPoint       = 0x30;   // PathFollower::mFinalPoint
constexpr uint32_t kVt_IsRtti           = 0x00;   // Controller primary vptr
constexpr uint32_t kVt_NavIsDone        = 0x14;   // Navigator vptr
constexpr uint32_t kVt_NavIsRtti        = 0x20;
constexpr uint32_t kEB_Class            = 0x0;    // EnergyBarClass*
constexpr uint32_t kEB_Energy           = 0x4;
constexpr uint32_t kEB_Flags            = 0x8;    // bit 0: overheated
constexpr uint32_t kEBC_Max             = 0x0;
// The soldier's EnergyBar, from the Controllable: past the debug/release split.
constexpr uint32_t kEnergyBarModtools   = 0x7E0;
constexpr uint32_t kEnergyBarRelease    = 0x7D0;

constexpr int kStateCombat = 2;

constexpr uint32_t kRttiUnitController = pbl_hash("UnitController");
constexpr uint32_t kRttiGraphFollower  = pbl_hash("NavigatorGraphFollower");
constexpr uint32_t kRttiPathFollower   = pbl_hash("NavigatorPathFollower");

using fn_update_indirect_t = bool(__fastcall*)(void* ctrl, void* edx, float dt);
using fn_is_rtti_t         = bool(__thiscall*)(void* self, uint32_t hash);
using fn_is_done_t         = bool(__thiscall*)(void* self);

fn_update_indirect_t s_updateIndirect = nullptr;
uint32_t s_energyBar = 0;
bool     s_installed = false;

bool is_rtti(void* obj, uint32_t slot, uint32_t hash)
{
   void** vt = *(void***)obj;
   return ((fn_is_rtti_t)vt[slot / 4])(obj, hash);
}

// Where the AI is heading, when it travels along a path of its own: to an
// objective through the graph, or along a path it was given. Null for a unit
// following a leader (the engine sprints those itself), sliding or standing.
const float* travel_destination(uint8_t* nav)
{
   void** vt = *(void***)nav;
   if (((fn_is_done_t)vt[kVt_NavIsDone / 4])(nav)) return nullptr;
   if (is_rtti(nav, kVt_NavIsRtti, kRttiGraphFollower))
      return (const float*)(nav + kGraph_GlobalDest);
   if (is_rtti(nav, kVt_NavIsRtti, kRttiPathFollower)) {
      const uint8_t* follower = *(const uint8_t* const*)(nav + kNav_PathFollower);
      return follower ? (const float*)(follower + kPF_FinalPoint) : nullptr;
   }
   return nullptr;
}

// The AI's height, when this soldier should sprint this update; null otherwise.
int* sprint_height(uint8_t* ctrl)
{
   uint8_t* uc = *(uint8_t**)(ctrl + kCtrl_Controller);
   if (!uc || !is_rtti(uc, kVt_IsRtti, kRttiUnitController)) return nullptr;
   uint8_t* lowLevel = uc + kUC_LowLevel;

   ai_sprint::Inputs in;
   int* height = (int*)(lowLevel + kLL_Height);
   in.height = *height;
   in.soldierState = *(const int*)(ctrl + g_soldier->mState);
   in.inCombat = *(const int*)(uc + kUC_State) == kStateCombat;
   const uint8_t* target = *(const uint8_t* const*)(lowLevel + kLL_Target);
   in.hasTarget = target &&
                  *(const uint32_t*)(target + kGO_HandleId) == *(const uint32_t*)(lowLevel + kLL_Target + 4);
   // The cheap tests first: most AI most of the time are fighting or not standing.
   if (in.height != ai_sprint::kHeightStand || in.inCombat || in.hasTarget) return nullptr;
   if (in.soldierState != ai_sprint::kStateStand && in.soldierState != ai_sprint::kStateSprint) return nullptr;

   uint8_t* nav = *(uint8_t**)(lowLevel + kLL_Navigator);
   const float* dest = nav ? travel_destination(nav) : nullptr;
   if (!dest) return nullptr;
   in.travelling = true;
   const float* pos = (const float*)(ctrl + kCtrl_Object + kGO_MatrixTrans);
   const float dx = dest[0] - pos[0];
   const float dz = dest[2] - pos[2];
   in.distance = std::sqrt(dx * dx + dz * dz);

   const uint8_t* bar = ctrl + s_energyBar;
   const uint8_t* barClass = *(const uint8_t* const*)(bar + kEB_Class);
   if (!barClass) return nullptr;
   in.energy = *(const float*)(bar + kEB_Energy);
   in.energyMax = *(const float*)(barClass + kEBC_Max);
   in.overheated = (bar[kEB_Flags] & 1) != 0;

   return ai_sprint::should_sprint(in, g_aiSprintDistance) ? height : nullptr;
}

bool __fastcall hooked_update_indirect(void* ctrl, void* edx, float dt)
{
   int* height = nullptr;
   __try {
      height = sprint_height(static_cast<uint8_t*>(ctrl));
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      height = nullptr;   // a torn-down controller must not reach the game's update
   }
   if (height) *height = ai_sprint::kHeightSprint;
   const bool result = s_updateIndirect(ctrl, edx, dt);
   // Back to what the AI asked for, unless it asked for something else meanwhile.
   if (height && *height == ai_sprint::kHeightSprint) *height = ai_sprint::kHeightStand;
   return result;
}

bool guard(uintptr_t base, uintptr_t va, const char* bytes, const char* mask)
{
   const auto* code = static_cast<const unsigned char*>(resolve(base, va));
   for (size_t i = 0; mask[i]; ++i) {
      if (mask[i] == 'x' && code[i] != static_cast<unsigned char>(bytes[i])) {
         install_log("[AISprint] NOT installed: prologue mismatch at EntitySoldier::UpdateIndirect 0x%08X",
                     (unsigned)va);
         return false;
      }
   }
   return true;
}

} // namespace

void ai_sprint_install(uintptr_t base)
{
   if (!g_aiSprint) return;
   const bool modtools = g_build == GameBuild::Modtools;
   if (!modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG) return;
   if (!g_addr->entity_soldier_update_indirect) {
      install_log("[AISprint] NOT installed: no address set for this build");
      return;
   }
   // The prologue, up to `this` moving into its register. No absolute addresses.
   if (!guard(base, g_addr->entity_soldier_update_indirect,
              modtools ? "\x83\xEC\x48\x53\x55\x56\x57\x8B\xF1"
                       : "\x55\x8B\xEC\x83\xE4\xF8\x83\xEC\x44\x53\x56\x57\x8B\xF9",
              modtools ? "xxxxxxxxx" : "xxxxxxxxxxxxxx"))
      return;
   if (!(g_aiSprintDistance > 0.0f) || !std::isfinite(g_aiSprintDistance)) g_aiSprintDistance = 30.0f;
   s_energyBar = modtools ? kEnergyBarModtools : kEnergyBarRelease;

   s_updateIndirect = reinterpret_cast<fn_update_indirect_t>(resolve(base, g_addr->entity_soldier_update_indirect));
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG r = DetourAttach(&(PVOID&)s_updateIndirect, hooked_update_indirect);
   if (r != NO_ERROR || DetourTransactionCommit() != NO_ERROR) {
      if (r != NO_ERROR) DetourTransactionAbort();
      s_updateIndirect = nullptr;
      install_log("[AISprint] NOT installed: detouring EntitySoldier::UpdateIndirect failed (%ld)", (long)r);
      return;
   }
   s_installed = true;
   install_log("[AISprint] installed (EntitySoldier::UpdateIndirect 0x%08X): AI out of combat sprint once %.0f m "
               "from where they are going", (unsigned)g_addr->entity_soldier_update_indirect,
               (double)g_aiSprintDistance);
}

void ai_sprint_uninstall()
{
   if (!s_installed) return;
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)s_updateIndirect, hooked_update_indirect);
   DetourTransactionCommit();
   s_installed = false;
}
