#include "pch.h"
#include "ai_goals.hpp"
#include "comm_orders.hpp"
#include "ai_goals_core.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "game/Battlefront2/Source/Character.h"
#include "util/install_log.hpp"

#include <detours.h>

#include <cstring>

// See ai_goals.hpp for the behaviour and the reads.

int g_aiGoalLimit = ai_goals::kMaxGoals;

namespace {

using ai_goals::GoalView;

// ---- Modtools ---------------------------------------------------------------------
constexpr uint32_t kMgr_Goals     = 0x1C;   // AIGoalManager::mGoals, its last member
constexpr uint32_t kGoal_Members  = 0x14;   // AIGoal: mUnitList's count ...
constexpr uint32_t kGoal_Weight   = 0x18;   // ... mWeight ...
constexpr uint32_t kGoal_Enabled  = 0x1C;   // ... mEnabled ...
constexpr uint32_t kGoal_Team     = 0x1D;   // ... mTeam
constexpr uint32_t kVt_AddUnit    = 0x10;   // AIGoal's vtable: AddUnit(Character*) ...
constexpr uint32_t kVt_Assign     = 0x1C;   // ... and Assign(Character*): the goal's order to it (thiscall, RET 4)
constexpr uint32_t kChr_GoalList  = 0x20;   // Character::mGoalNode._pList: in a goal while set
constexpr uint32_t kChr_PlayerId  = 0x130;  // below 0: AI
constexpr uint32_t kChr_Team      = 0x134;
constexpr uint32_t kTeam_Members  = 0x38;   // Team::mMemberCount
constexpr int      kMaxTeams      = 8;      // as the game's Lua checks a team (ClearAIGoals)
constexpr int      kMaxCharacters = 1024;
constexpr size_t   kManagerSize   = kMgr_Goals + ai_goals::kGoalSlots * sizeof(void*);

using fn_new_t    = void*(__cdecl*)(size_t size);
using fn_ctor_t   = uint8_t*(__thiscall*)(void* mem);
using fn_create_t = void(__cdecl*)();
using fn_assign_t = void(__fastcall*)(uint8_t* mgr, void* edx, uint8_t* chr);   // thiscall(Character*), RET 4
using fn_goal_t   = void(__fastcall*)(uint8_t* mgr, void* edx, uint8_t* goal);  // thiscall(AIGoal*), RET 4
using fn_team_t   = void(__fastcall*)(uint8_t* mgr, void* edx, int team);       // thiscall(int), RET 4
using fn_remove_t = void(__stdcall*)(uint8_t* chr);
using fn_find_t   = uint8_t*(__thiscall*)(uint8_t* mgr, uint8_t* chr);
using fn_add_t    = void(__thiscall*)(uint8_t* goal, uint8_t* chr);

int         s_limit = ai_goals::kStockGoals;
bool        s_detoured = false;
fn_new_t    s_new = nullptr;        // the game's operator new
fn_ctor_t   s_ctor = nullptr;       // AIGoalManager::AIGoalManager
fn_create_t s_create = nullptr;     // detoured: AIGoalManager::Create ...
fn_assign_t s_assign = nullptr;     // ... AssignUnit ...
fn_goal_t   s_delete = nullptr;     // ... DeleteGoal ...
fn_team_t   s_clear = nullptr;      // ... ClearAllGoals
fn_remove_t s_remove = nullptr;     // RemoveUnit
fn_find_t   s_find = nullptr;       // FindGoal
uint8_t**   s_instance = nullptr;   // AIGoalManager::mInstance
uint8_t***  s_teams = nullptr;      // Team::sTeams
uint8_t*    s_own[kMaxCharacters] = {};   // a character's goal of its own, or null

// The counts written over the stock 20, kept to be put back.
struct Patch {
   uint8_t* at = nullptr;
   uint8_t  orig = 0;
};
Patch s_patches[12];
int   s_patchCount = 0;

uint8_t* manager() { return s_instance ? *s_instance : nullptr; }
uint8_t** goals_of(uint8_t* mgr) { return (uint8_t**)(mgr + kMgr_Goals); }

bool holds(uint8_t* mgr, const void* goal)
{
   uint8_t** const goals = goals_of(mgr);
   for (int i = 0; goal && i < s_limit; ++i)
      if (goals[i] == goal) return true;
   return false;
}

const uint8_t* characters(int& count)
{
   count = 0;
   if (!g_addr->char_array_base || !g_addr->max_chars) return nullptr;
   const uint8_t* const chars = *(const uint8_t* const*)resolve(g_addr->char_array_base);
   count = chars ? *(const int*)resolve(g_addr->max_chars) : 0;
   if (count < 0 || count > kMaxCharacters) count = 0;
   return chars;
}

uint8_t* character(int index)
{
   int count = 0;
   const uint8_t* const chars = characters(count);
   return chars && index >= 0 && index < count ? (uint8_t*)chars + (size_t)index * layout::Character::kSize : nullptr;
}

int character_index(const uint8_t* chr)
{
   int count = 0;
   const uint8_t* const chars = characters(count);
   if (!chars || chr < chars) return -1;
   const size_t offset = (size_t)(chr - chars);
   const int index = (int)(offset / layout::Character::kSize);
   return offset % layout::Character::kSize == 0 && index < count ? index : -1;
}

int team_size(int team)
{
   uint8_t** const teams = s_teams ? *s_teams : nullptr;
   const uint8_t* const t = teams && team >= 0 && team < kMaxTeams ? teams[team] : nullptr;
   return t ? *(const int*)(t + kTeam_Members) : 0;
}

void add_unit(uint8_t* goal, uint8_t* chr)
{
   ((fn_add_t)(*(void***)goal)[kVt_AddUnit / 4])(goal, chr);
}

// The goal's order to the unit, now: AIGoal::Update hands one only to a member
// that is idle, waiting or done (its controller's command CMD_NONE or CMD_WAIT,
// or its agent finished), so a unit moved out of a goal whose order never ends
// (Defend) would go on with that one.
void command(uint8_t* goal, uint8_t* chr)
{
   ((fn_add_t)(*(void***)goal)[kVt_Assign / 4])(goal, chr);
}

// A unit with no goal joins one: its own while that is an enabled goal of its
// side, else the game's rule over the limit, the goals kept for others passed
// over (ai_goals_core.hpp). The goal it joined, or null.
uint8_t* assign(uint8_t* mgr, uint8_t* chr)
{
   uint8_t** const goals = goals_of(mgr);
   const int team = *(const int*)(chr + kChr_Team);
   const int index = character_index(chr);
   uint8_t* const own = index >= 0 ? s_own[index] : nullptr;
   if (own && holds(mgr, own) && own[kGoal_Team] == team && own[kGoal_Enabled]) {
      add_unit(own, chr);
      return own;
   }
   // The goals scripts keep for units of their own.
   bool kept[ai_goals::kGoalSlots] = {};
   int count = 0;
   characters(count);
   for (int c = 0; c < count; ++c)
      if (const uint8_t* const g = s_own[c])
         for (int i = 0; i < s_limit; ++i)
            if (goals[i] == g) {
               kept[i] = true;
               break;
            }
   GoalView views[ai_goals::kGoalSlots];
   for (int i = 0; i < s_limit; ++i) {
      const uint8_t* const g = goals[i];
      GoalView& v = views[i];
      v.present = g != nullptr;
      if (!g) continue;
      v.team = g[kGoal_Team];
      v.enabled = g[kGoal_Enabled] != 0;
      v.weight = *(const float*)(g + kGoal_Weight);
      v.members = *(const int*)(g + kGoal_Members);
      v.reserved = kept[i];
   }
   const int k = ai_goals::pick_goal(views, s_limit, team, team_size(team));
   if (k < 0) return nullptr;
   add_unit(goals[k], chr);
   return goals[k];
}

// A goal deleted: its units are free of it (the game's next update puts them in
// others by weight).
void drop_gone(uint8_t* mgr)
{
   for (uint8_t*& own : s_own)
      if (own && (!mgr || !holds(mgr, own))) own = nullptr;
}

// ---- The stand-ins --------------------------------------------------------------------
// AIGoalManager::Create, cdecl(): the same object with room for kGoalSlots, from
// the game's own operator new (its destructor frees it with the game's delete)
// and constructor; every slot cleared. A new mission: no unit has a goal of its
// own.
void __cdecl create_mt()
{
   uint8_t* mgr = nullptr;
   __try {
      if (void* const mem = s_new(kManagerSize)) {
         mgr = s_ctor(mem);   // clears the first 20 slots
         std::memset(mgr + kMgr_Goals, 0, ai_goals::kGoalSlots * sizeof(void*));
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      mgr = nullptr;
   }
   *s_instance = mgr;
   std::memset(s_own, 0, sizeof s_own);
}

void __fastcall assign_mt(uint8_t* mgr, void*, uint8_t* chr)
{
   __try {
      assign(mgr, chr);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
}

// After either, the comm keys forget a goal gone (their orders' goals), before
// the pool can hand its memory to a new one.
void __fastcall delete_mt(uint8_t* mgr, void* edx, uint8_t* goal)
{
   s_delete(mgr, edx, goal);
   __try {
      drop_gone(mgr);
      comm_orders_goals_changed();
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
}

void __fastcall clear_mt(uint8_t* mgr, void* edx, int team)
{
   s_clear(mgr, edx, team);
   __try {
      drop_gone(mgr);
      comm_orders_goals_changed();
   } __except (EXCEPTION_EXECUTE_HANDLER) {
   }
}

// ---- Install --------------------------------------------------------------------------
bool code_is(uintptr_t base, uintptr_t va, const char* bytes, size_t len)
{
   return std::memcmp(resolve(base, va), bytes, len) == 0;
}

uintptr_t call_destination(const uint8_t* call)
{
   const uint8_t* dest = call + 5 + *(const int32_t*)(call + 1);
   if (dest[0] == 0xE9) dest = dest + 5 + *(const int32_t*)(dest + 1);
   return (uintptr_t)dest;
}

// A count of 20 the limit takes the place of: the instruction's bytes as the
// game's own (`bytes`, `len` of them, at `va`), the count's byte `at` into them.
struct Count {
   uintptr_t   va;
   const char* bytes;
   size_t      len;
   size_t      at;
   int         value;   // what it becomes
};

} // namespace

void ai_goals_install(uintptr_t base)
{
   const int wanted = g_aiGoalLimit;
   if (wanted <= ai_goals::kStockGoals) return;   // 0, or the game's own 20
   if (g_build != GameBuild::Modtools) {
      install_log("[AIGoals] NOT installed: modtools only for now; the game's 20 AI goals stand");
      return;
   }
   const auto* a = g_addr;
   if (!a->ai_goal_pool_setup || !a->ai_goal_manager_create || !a->ai_goal_manager_instance || !a->ai_goal_add ||
       !a->ai_goal_delete || !a->ai_goal_clear_all || !a->ai_goal_assign_unit || !a->ai_goal_remove_unit ||
       !a->ai_goal_print || !a->ai_goal_find || !a->ai_goal_update || !a->team_array_base ||
       !a->engine_operator_new) {
      install_log("[AIGoals] NOT installed: no address set for this build");
      return;
   }
   const int limit = ai_goals::clamp_limit(wanted);
   const Count counts[] = {
      // AIUtil::Init: MemoryPool::Setup(&AIGoal::sMemoryPool, 20, 0x3C).
      {a->ai_goal_pool_setup, "\x6A\x3C\x6A\x14\xB9\x38\xFA\xB8\x00", 9, 3, limit},
      // AddGoal's search for a free slot.
      {a->ai_goal_add + 0x5E, "\x33\xFF\x8D\x45\x1C\x83\x38\x00\x74\x3B\x47\x83\xC0\x04\x83\xFF\x14\x7C\xF2", 19, 16, limit},
      // DeleteGoal's.
      {a->ai_goal_delete + 0x08,
       "\x33\xF6\x8D\x47\x1C\x8D\x49\x00\x39\x08\x74\x36\x46\x83\xC0\x04\x83\xFE\x14\x7C\xF3", 21, 18, limit},
      // ClearAllGoals', counted down.
      {a->ai_goal_clear_all + 0x07, "\x8D\x71\x1C\xBF\x14\x00\x00\x00", 8, 4, limit},
      // FindGoal's.
      {a->ai_goal_find + 0x2C, "\x75\xF2\x46\x83\xC7\x04\x83\xFE\x14\x7C", 10, 8, limit},
      // Update's, counted down.
      {a->ai_goal_update + 0xC1, "\x8D\x73\x1C\xBF\x14\x00\x00\x00", 8, 4, limit},
      // The debug listing: its loop five at a time, its "none of this team",
      // and its list, counted down.
      {a->ai_goal_print + 0x9A, "\x83\xC2\x05\x83\xC0\x14\x83\xFA\x14\x7C\xAB", 11, 8, limit},
      {a->ai_goal_print + 0xB4, "\x83\xC2\x04\x83\xFA\x14\x0F\x84", 8, 5, ai_goals::listing_end(limit)},
      {a->ai_goal_print + 0x123, "\x83\xC6\x1C\xBB\x14\x00\x00\x00", 8, 4, limit},
   };
   // Create: PUSH 0x6C, CALL operator new, ADD ESP 4, TEST, JE, MOV ECX EAX,
   // CALL the constructor (through its thunk), MOV mInstance EAX, RET.
   const uint8_t* const create = (const uint8_t*)resolve(base, a->ai_goal_manager_create);
   uint8_t store[5] = {0xA3, 0, 0, 0, 0};
   const uint32_t instance = (uint32_t)(uintptr_t)resolve(base, a->ai_goal_manager_instance);
   std::memcpy(store + 1, &instance, 4);
   bool ok = create[0] == 0x6A && create[1] == 0x6C && create[2] == 0xE8 &&
             call_destination(create + 2) == (uintptr_t)resolve(base, a->engine_operator_new) &&
             std::memcmp(create + 7, "\x83\xC4\x04\x85\xC0\x74\x0D\x8B\xC8\xE8", 10) == 0 &&
             std::memcmp(create + 0x15, store, 5) == 0 && create[0x1A] == 0xC3;
   // The constructor clears 20 slots after the Thread base; the stand-in clears the rest.
   const uintptr_t ctor = ok ? call_destination(create + 0x10) : 0;
   ok = ok && std::memcmp((const void*)(ctor + 0x1A), "\x33\xC0\x8D\x7E\x1C\xB9\x14\x00\x00\x00\xF3\xAB", 12) == 0 &&
        // AssignUnit: the 20 weights on the stack; RemoveUnit: the character's
        // goal list (+0x20); FindGoal from mGoals (+0x1C); the teams as AssignUnit
        // reads them.
        code_is(base, a->ai_goal_assign_unit, "\x83\xEC\x64\x8B\x44\x24\x68", 7) &&
        code_is(base, a->ai_goal_assign_unit + 0x12, "\x8B\x88\x34\x01\x00\x00", 6) &&
        code_is(base, a->ai_goal_assign_unit + 0x85, "\x8B\x15", 2) &&
        *(const uint32_t*)resolve(base, a->ai_goal_assign_unit + 0x87) ==
           (uint32_t)(uintptr_t)resolve(base, a->team_array_base) &&
        code_is(base, a->ai_goal_assign_unit + 0x94, "\x8B\x50\x38", 3) &&
        code_is(base, a->ai_goal_remove_unit, "\x8B\x44\x24\x04\x8B\x48\x20\x33\xD2", 9) &&
        code_is(base, a->ai_goal_find, "\x53\x8B\x5C\x24\x08\x56\x57\x33\xF6\x8D\x79\x1C", 12) &&
        code_is(base, a->ai_goal_delete, "\x56\x57\x8B\xF9\x8B\x4C\x24\x0C", 8) &&
        code_is(base, a->ai_goal_clear_all, "\x53\x8B\x5C\x24\x08\x56\x57\x8D\x71\x1C", 10);
   for (const Count& c : counts) ok = ok && code_is(base, c.va, c.bytes, c.len);
   if (!ok) {
      install_log("[AIGoals] NOT installed: the AI goal manager's code differs; the game's 20 AI goals stand");
      return;
   }
   s_limit = limit;
   s_new = (fn_new_t)resolve(base, a->engine_operator_new);
   s_ctor = (fn_ctor_t)ctor;
   s_instance = (uint8_t**)resolve(base, a->ai_goal_manager_instance);
   s_teams = (uint8_t***)resolve(base, a->team_array_base);
   s_remove = (fn_remove_t)resolve(base, a->ai_goal_remove_unit);
   s_find = (fn_find_t)resolve(base, a->ai_goal_find);
   s_create = (fn_create_t)resolve(base, a->ai_goal_manager_create);
   s_assign = (fn_assign_t)resolve(base, a->ai_goal_assign_unit);
   s_delete = (fn_goal_t)resolve(base, a->ai_goal_delete);
   s_clear = (fn_team_t)resolve(base, a->ai_goal_clear_all);
   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   LONG r = DetourAttach(&(PVOID&)s_create, create_mt);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_assign, assign_mt);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_delete, delete_mt);
   if (r == NO_ERROR) r = DetourAttach(&(PVOID&)s_clear, clear_mt);
   if (r != NO_ERROR || DetourTransactionCommit() != NO_ERROR) {
      if (r != NO_ERROR) DetourTransactionAbort();
      install_log("[AIGoals] NOT installed: detouring the AI goal manager failed (%ld)", (long)r);
      return;
   }
   s_detoured = true;
   // The counts, during the installers' writable window.
   for (const Count& c : counts) {
      uint8_t* const at = (uint8_t*)resolve(base, c.va) + c.at;
      s_patches[s_patchCount].at = at;
      s_patches[s_patchCount].orig = *at;
      ++s_patchCount;
      *at = (uint8_t)c.value;
   }
   install_log("[AIGoals] installed: up to %d AI goals at once (the game's 20); units may have goals of their own "
               "(SetUnitGoal)",
               s_limit);
}

void ai_goals_uninstall()
{
   for (int i = 0; i < s_patchCount; ++i) protected_write(s_patches[i].at, &s_patches[i].orig, 1);
   s_patchCount = 0;
   if (s_detoured) {
      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourDetach(&(PVOID&)s_create, create_mt);
      DetourDetach(&(PVOID&)s_assign, assign_mt);
      DetourDetach(&(PVOID&)s_delete, delete_mt);
      DetourDetach(&(PVOID&)s_clear, clear_mt);
      DetourTransactionCommit();
      s_detoured = false;
   }
   s_limit = ai_goals::kStockGoals;
   std::memset(s_own, 0, sizeof s_own);
}

// ---- A mission script's units with goals of their own ---------------------------------
namespace ai_goals_lua {

bool available() { return s_detoured; }

bool goal_exists(const void* goal)
{
   if (!s_detoured || !goal) return false;
   __try {
      uint8_t* const mgr = manager();
      return mgr && holds(mgr, goal);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

// The unit goes in `goal` now (out of the goal it was in), takes its order now
// (command), and goes back in it whenever the game reassigns it; the goal is
// kept for the units scripts put in it. An AI unit, and a goal of its side.
bool set_unit_goal(int index, void* goal)
{
   if (!s_detoured || !goal || index < 0 || index >= kMaxCharacters) return false;
   bool ok = false;
   __try {
      uint8_t* const mgr = manager();
      uint8_t* const chr = character(index);
      uint8_t* const g = (uint8_t*)goal;
      if (mgr && chr && holds(mgr, g) && *(const int*)(chr + kChr_PlayerId) < 0 &&
          g[kGoal_Team] == *(const int*)(chr + kChr_Team)) {
         s_own[index] = g;
         s_remove(chr);
         if (uint8_t* const placed = assign(mgr, chr)) command(placed, chr);
         ok = true;
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      ok = false;
   }
   return ok;
}

// The unit back to its team's goals: out of its own, into one by weight.
bool clear_unit_goal(int index)
{
   if (!s_detoured || index < 0 || index >= kMaxCharacters) return false;
   bool ok = false;
   __try {
      const bool had = s_own[index] != nullptr;
      s_own[index] = nullptr;
      uint8_t* const mgr = manager();
      uint8_t* const chr = character(index);
      if (had && mgr && chr && *(const uint8_t* const*)(chr + kChr_GoalList)) {
         s_remove(chr);
         if (uint8_t* const placed = assign(mgr, chr)) command(placed, chr);
      }
      ok = had;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      ok = false;
   }
   return ok;
}

void* unit_goal(int index)
{
   if (!s_detoured || index < 0 || index >= kMaxCharacters) return nullptr;
   void* goal = nullptr;
   __try {
      uint8_t* const mgr = manager();
      uint8_t* const chr = character(index);
      goal = mgr && chr ? s_find(mgr, chr) : nullptr;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      goal = nullptr;
   }
   return goal;
}

} // namespace ai_goals_lua
