#pragma once

#include <cmath>
#include <cstdint>

// =============================================================================
// The comm keys in single player, on their own: which keys went down, what each
// asks of the AI given what the player aims at, and who answers. No engine
// reads, so tests/comm_orders_tests.cpp can exercise all of it; the reads,
// rays, goals and markers are in comm_orders.cpp.
//
// The user (2026-10-14): "Any chance of re-activating the MP commands in SP and
// having the AI do what they say?", and on the design: Spotted, Medic, Repair,
// Ammo as agreed; "F10 ... should not strictly follow you like how F4 makes
// them"; "F12 could be a defend my target, so you aim at a friendly under the
// cross hair, OR if nothing and they can reach it then the position you are
// aiming at"; Pickup last, hovers first. Then "Yep, get them done".
// =============================================================================

namespace comm_orders {

// The comm keys' codes, as their team chat carries them (NetGame::
// AddChatMessageToQue's Code) and TeamAssistManager::AddRequest takes them;
// the key for each is ProcessedInputs Comm_Chat_0 (input 20) on.
enum Code : int {
   kSpotted = 0,   // F5 by default
   kMedic   = 1,   // F6
   kRepair  = 2,   // F7
   kAmmo    = 3,   // F8
   kPickup  = 4,   // F9
   kBackup  = 5,   // F10
   kAttack  = 6,   // F11
   kDefend  = 7,   // F12
   kCodes   = 8,
};
constexpr int kFirstInput = 20;   // ePROCESSEDINPUT_Comm_Chat_0

// The codes whose keys went down since the last look (`last`, the held bits
// then, kept here): bit `code` set for each. A key held counts once.
inline uint32_t pressed(uint32_t inputBits, uint32_t& last)
{
   const uint32_t held = (inputBits >> kFirstInput) & ((1u << kCodes) - 1u);
   const uint32_t down = held & ~last;
   last = held;
   return down;
}

// What the crosshair is on.
enum class Aim : uint8_t {
   None,     // nothing within reach
   Enemy,    // a unit or object of another side
   Friend,   // one of the player's side
   Spot,     // the ground, a building or a prop: a place
};

// A place the AI can be sent to: within kSpotRange of the player and on ground
// a soldier stands on (no steeper than kWalkableUp: a wall or a cliff face is
// not a place). The user: "if nothing and they can reach it".
constexpr float kSpotRange  = 400.0f;   // m
constexpr float kWalkableUp = 0.7f;     // the hit's normal, up: about 45 degrees

inline bool reachable_spot(float normalUp, float dist)
{
   return std::isfinite(normalUp) && std::isfinite(dist) && normalUp >= kWalkableUp && dist >= 0.0f &&
          dist <= kSpotRange;
}

// What a key asks for.
enum class Ask : uint8_t {
   Nothing,        // nothing to do (no target for it): the stock marker and shout only
   ShootTarget,    // Spotted: those who see the enemy aim at it where they stand
   AidPlayer,      // Medic, Repair, Ammo: the nearest who can give it comes to the player
   DefendPlayer,   // Backup: a few fight round the player for a while
   DestroyTarget,  // Attack an enemy: a few go after it
   DefendTarget,   // Defend a friend: a few guard it
   DefendSpot,     // Attack or Defend a place: a few go there and hold it
   PickUpPlayer,   // Pickup: the nearest hover with an AI driver comes to the player
};

inline Ask ask_for(int code, Aim aim, bool spotReachable)
{
   switch (code) {
   case kSpotted: return aim == Aim::Enemy ? Ask::ShootTarget : Ask::Nothing;
   case kMedic:
   case kRepair:
   case kAmmo: return Ask::AidPlayer;
   case kBackup: return Ask::DefendPlayer;
   case kPickup: return Ask::PickUpPlayer;   // hovers first (the user)
   case kAttack:
      if (aim == Aim::Enemy) return Ask::DestroyTarget;
      return aim == Aim::Spot && spotReachable ? Ask::DefendSpot : Ask::Nothing;
   case kDefend:
      if (aim == Aim::Friend) return Ask::DefendTarget;
      return aim == Aim::Spot && spotReachable ? Ask::DefendSpot : Ask::Nothing;
   default: return Ask::Nothing;
   }
}

// How many answer, from how far, and for how long.
struct Order {
   int    units = 0;     // how many AI are sent (0: every one that qualifies)
   float  range = 0.0f;  // m: from the player (Spotted: from the target)
   double lasts = 0.0;   // s the goal holds before they go back to their own (0: at once, one look)
};

inline Order order_for(Ask ask)
{
   switch (ask) {
   case Ask::ShootTarget: return {0, 150.0f, 0.0};
   case Ask::AidPlayer: return {1, 300.0f, 45.0};
   case Ask::DefendPlayer: return {3, 200.0f, 60.0};
   case Ask::DestroyTarget: return {4, 250.0f, 90.0};
   case Ask::DefendTarget: return {3, 250.0f, 90.0};
   case Ask::DefendSpot: return {3, 250.0f, 90.0};
   case Ask::PickUpPlayer: return {1, 400.0f, 60.0};
   default: return {};
   }
}

// The nearest of `n` candidates (`dist`, m; `ok`, whether each may answer)
// within `range`, nearest first (the earlier on a tie), into `out`, which has
// room for `n`. How many of them answer: `want` at most (0: all).
inline int nearest(const float* dist, const bool* ok, int n, int want, float range, int* out)
{
   int count = 0;
   for (int i = 0; i < n; ++i)
      if (ok[i] && std::isfinite(dist[i]) && dist[i] <= range) out[count++] = i;
   for (int a = 1; a < count; ++a) {
      const int v = out[a];
      int b = a;
      for (; b > 0 && dist[out[b - 1]] > dist[v]; --b) out[b] = out[b - 1];
      out[b] = v;
   }
   return want > 0 && count > want ? want : count;
}

} // namespace comm_orders
