#include "pch.h"
#include "command_post_null_fix.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"
#include "core/pbl_hash.hpp"
#include "game/Battlefront2/Source/Character.h"
#include "game/Battlefront2/Source/CommandPost.h"
#include "game/Battlefront2/Source/Controllable.h"
#include "game/Battlefront2/Source/EntityFlyer.h"
#include "game/Battlefront2/Source/GameObject.h"
#include "util/install_log.hpp"

#include <detours.h>

// See command_post_null_fix.hpp for the crash this guards.

// CommandPost::SetTeam(CommandPost* this, int newTeam, int oldTeam) -- __thiscall,
// RET 8.  Taken as __fastcall so ECX arrives as the first parameter; EDX is
// unused by the original and is passed straight through.
typedef void(__fastcall* fn_SetTeam_t)(void* ecx, void* edx, int a, int b);

static fn_SetTeam_t g_origSetTeam = nullptr;

// The engine can retry the same bad script reference every capture tick, so a
// per-session counter keeps one crash's worth of content bug from filling the
// log with thousands of identical lines.
static uint32_t g_blocked = 0;
static constexpr uint32_t kMaxReports = 8;

static void __fastcall hooked_set_team(void* ecx, void* edx, int a, int b)
{
   if (!ecx) {
      ++g_blocked;
      if (g_blocked <= kMaxReports) {
         warn_gamelog(RED_SEVERITY_ERROR, SRC_FILE, __LINE__,
                      "[CommandPostNullFix] CommandPost::SetTeam called on a NULL command post "
                      "(team %d -> %d); ignoring what would have been a crash. A mission script "
                      "is almost certainly passing an entity that is not a command post -- check "
                      "the bf2log for \"is EntityProp, but need GameObject\" lines naming it.%s\n",
                      b, a, (g_blocked == kMaxReports) ? " (further reports suppressed)" : "");
      }
      return;
   }

   g_origSetTeam(ecx, edx, a, b);
}

// -----------------------------------------------------------------------------
// AI spawning at a CommandFlyer that has taken off.
//
// CommandFlyer::Update only offers its post to the AI while the flyer is
// landed, but Character::Update spawns a character at the post it already
// picked without asking again.  An AI that picked the gunship on the ground
// and finishes its spawn countdown after takeoff is built at the flyer's
// position in mid air.  Spawn then seats it with GetPassengerSlot(0), which
// only knows PassengerSlots seats, never turrets, and falls back to a seat an
// AI already holds.  Entering that seat throws its occupant out.  With no
// seat at all the new AI just falls.
//
// For AI only: Spawn now looks for a free seat the way CyclePosition does
// (pilotable, nobody in it), passenger slots first, then turrets.  If there is
// one, GetPassengerSlot hands it to the stock Spawn, which boards the AI with
// its own EnterControllable.  If not, Spawn handles the post the way it
// handles a post the team has lost: SelectPost(0) then Respawn().  Players are
// left alone.  GetPassengerSlot's only caller is Character::Spawn.
// -----------------------------------------------------------------------------

// Character::Spawn(const PblMatrix&) thiscall RET 4.
typedef void(__fastcall* fn_Spawn_t)(void* chr, void* edx, const void* matrix);
// Character::SelectPost(CommandPost*) thiscall RET 4.
typedef void(__fastcall* fn_SelectPost_t)(void* chr, void* edx, void* post);
// Character::Respawn() thiscall.
typedef void(__fastcall* fn_Respawn_t)(void* chr, void* edx);
// GameObject::IsRtti(hash), primary vtable slot 0, thiscall RET 4.
typedef bool(__fastcall* fn_IsRtti_t)(void* obj, void* edx, uint32_t hash);
// CommandFlyer::GetPassengerSlot(int unused) thiscall RET 4 -> Controllable*.
typedef void*(__fastcall* fn_GetSlot_t)(void* flyer, void* edx, int unused);

static fn_Spawn_t      g_origSpawn   = nullptr;
static fn_GetSlot_t    g_origGetSlot = nullptr;
static fn_SelectPost_t g_selectPost  = nullptr;
static fn_Respawn_t    g_respawn     = nullptr;

// Seat for the AI spawn in progress; GetPassengerSlot returns it.
static void* g_aiSeat = nullptr;

static constexpr uint32_t kCommandFlyerRtti = pbl_hash("CommandFlyer");

static bool seat_is_free(void* seatEntity)
{
   if (!seatEntity) return false;
   void* c = (char*)seatEntity + layout::Controllable::kSeatControllable;
   return layout::Controllable::IsPilotable(c) && !layout::Controllable::GetPilot(c);
}

// First free seat as a Controllable*: passenger slots, then turrets.
static void* find_free_seat(void* flyer)
{
   namespace F = layout::EntityFlyer;
   const ::EntityFlyerClass* cls = F::mClass(flyer);
   const int numSlots = cls ? layout::EntityFlyerClass::mNumPassengerSlots(cls) : 0;
   ::PassengerSlot* const* slots = F::mPassengerSlots(flyer);
   for (int i = 0; i < numSlots && i < 4; ++i)
      if (seat_is_free(slots[i])) return (char*)slots[i] + layout::Controllable::kSeatControllable;

   const int numTurrets = F::mNumTurrets(flyer);
   ::MountedTurret* const* turrets = F::mTurret(flyer);
   for (int i = 0; i < numTurrets && i < 8; ++i)
      if (seat_is_free(turrets[i])) return (char*)turrets[i] + layout::Controllable::kSeatControllable;
   return nullptr;
}

// The post's CommandFlyer when it is off the ground, else null.
static void* airborne_flyer(void* post)
{
   GameObject* obj = layout::CommandPost::mObject(post).Get();
   if (!obj) return nullptr;
   const fn_IsRtti_t is_rtti = (fn_IsRtti_t)(*(void***)obj)[0];
   if (!is_rtti(obj, nullptr, kCommandFlyerRtti)) return nullptr;
   if (layout::EntityFlyer::mState(obj) == layout::EntityFlyer::kLanded) return nullptr;
   return obj;
}

static void* __fastcall hooked_get_slot(void* flyer, void* edx, int unused)
{
   if (g_aiSeat) return g_aiSeat;
   return g_origGetSlot(flyer, edx, unused);
}

static void __fastcall hooked_spawn(void* chr, void* edx, const void* matrix)
{
   char* c = (char*)chr;
   void* post = *(void**)(c + layout::Character::kPost);
   if (*(int*)(c + layout::Character::kPlayerId) >= 0 || !post ||
       *(void**)(c + layout::Character::kUnit)) {
      g_origSpawn(chr, edx, matrix);
      return;
   }

   void* flyer = airborne_flyer(post);
   if (!flyer) {
      g_origSpawn(chr, edx, matrix);
      return;
   }

   void* seat = find_free_seat(flyer);
   if (seat) {
      g_aiSeat = seat;
      g_origSpawn(chr, edx, matrix);
      g_aiSeat = nullptr;
   } else {
      g_selectPost(chr, nullptr, nullptr);
      g_respawn(chr, nullptr);
   }
}

static void install_flyer_spawn_guard(uintptr_t exe_base)
{
   if (!g_addr->character_spawn || !g_addr->character_select_post ||
       !g_addr->character_respawn || !g_addr->command_flyer_get_passenger_slot) {
      install_log("[CommandPost] CommandFlyer spawn guard: addresses unknown for this build, not installed");
      return;
   }

   g_origSpawn   = reinterpret_cast<fn_Spawn_t>(resolve(exe_base, g_addr->character_spawn));
   g_origGetSlot = reinterpret_cast<fn_GetSlot_t>(resolve(exe_base, g_addr->command_flyer_get_passenger_slot));
   g_selectPost  = reinterpret_cast<fn_SelectPost_t>(resolve(exe_base, g_addr->character_select_post));
   g_respawn     = reinterpret_cast<fn_Respawn_t>(resolve(exe_base, g_addr->character_respawn));

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG r1 = DetourAttach(reinterpret_cast<PVOID*>(&g_origSpawn), hooked_spawn);
   const LONG r2 = DetourAttach(reinterpret_cast<PVOID*>(&g_origGetSlot), hooked_get_slot);
   const LONG rc = DetourTransactionCommit();

   if (r1 != NO_ERROR || r2 != NO_ERROR || rc != NO_ERROR) {
      g_origSpawn = nullptr;
      g_origGetSlot = nullptr;
      install_log("[CommandPost] CommandFlyer spawn guard: Detours failed (%ld/%ld/%ld), not installed", r1, r2, rc);
      return;
   }
   install_log("[CommandPost] CommandFlyer spawn guard installed");
}

static void uninstall_flyer_spawn_guard()
{
   if (!g_origSpawn) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(reinterpret_cast<PVOID*>(&g_origSpawn), hooked_spawn);
   DetourDetach(reinterpret_cast<PVOID*>(&g_origGetSlot), hooked_get_slot);
   DetourTransactionCommit();
   g_origSpawn = nullptr;
   g_origGetSlot = nullptr;
}

void command_post_null_fix_install(uintptr_t exe_base)
{
   install_flyer_spawn_guard(exe_base);

   if (g_addr->command_post_set_team == 0) return; // not derived for this build

   g_origSetTeam = reinterpret_cast<fn_SetTeam_t>(resolve(exe_base, g_addr->command_post_set_team));

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG rd = DetourAttach(reinterpret_cast<PVOID*>(&g_origSetTeam), hooked_set_team);
   const LONG rc = DetourTransactionCommit();

   if (rd != NO_ERROR || rc != NO_ERROR) g_origSetTeam = nullptr;
}

void command_post_null_fix_uninstall()
{
   uninstall_flyer_spawn_guard();

   if (!g_origSetTeam) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(reinterpret_cast<PVOID*>(&g_origSetTeam), hooked_set_team);
   DetourTransactionCommit();
   g_origSetTeam = nullptr;
}
