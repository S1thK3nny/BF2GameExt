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
// aiming at"; Pickup last, hovers first. Then "Yep, get them done". And after
// trying it: "On foot, orders should only apply to current followers/squadmates
// recruited via F4. F5 is fine to work for general nearby friendlies not in the
// squad, but the text should signify it. F6/7/8 are also fine for general
// friendlies, but prioritise squadmates if possible. F9 also general friendlies,
// same for F10 to a degree. They should only help you out for a few seconds,
// maybe 10-15 max. F11, F12 - apply to squadmates only. However, an ODF param
// should be instated and when set, means you can issue commands to any
// friendlies without requiring squadmates."
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
   AidPlayer,      // Medic, Repair, Ammo: one who can give it comes to the player
   DefendPlayer,   // Backup: a few fight round the player for a moment
   DestroyTarget,  // Attack an enemy: the squad goes after it
   DefendTarget,   // Defend a friend: the squad guards it
   DefendSpot,     // Attack or Defend a place: the squad goes there and holds it
   PickUpPlayer,   // Pickup: the nearest hover with an AI driver comes to the player
};

// Who an order goes to: the player's squad is the AI following it (the squad
// key's recruits, F4 for the user).
enum class Who : uint8_t {
   Anyone,      // every friendly, the squad's too (Spotted: "general nearby friendlies")
   SquadFirst,  // a squadmate who can, else the nearest friendly who can (Medic, Repair, Ammo)
   Allies,      // friendlies outside the squad, which follows anyway (Pickup, Backup)
   Squad,       // the squad alone (Attack, Defend) ...
   SquadPlus,   // ... or, where the player may command any friendly (CommandAnyFriendly),
                // the whole squad and the nearest friendlies until there are enough
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

// How many answer, from how far, for how long, and who. A brief help (`brief`)
// is over kHelpFor after the first of them reached the player, or kTravelFor
// after it was asked if none has (help_over); `lasts` is the others' time. A
// squad order sends the whole squad (`units` 0); with any friendly to command,
// at least `units` (the squad first).
struct Order {
   int    units = 0;      // how many AI are sent (0: every one that qualifies)
   float  range = 0.0f;   // m: from the player (Spotted: from the target)
   double lasts = 0.0;    // s the goal holds before they go back (0: at once, one look)
   Who    who = Who::Anyone;
   bool   brief = false;
};

constexpr double kHelpFor   = 15.0;   // s with the player (the user: "maybe 10-15 max")
constexpr double kTravelFor = 30.0;   // s at most to reach it
constexpr float  kWithYou   = 20.0f;  // m: reached it

inline Order order_for(Ask ask, bool anyFriendly)
{
   const Who squad = anyFriendly ? Who::SquadPlus : Who::Squad;
   switch (ask) {
   case Ask::ShootTarget: return {0, 150.0f, 0.0, Who::Anyone, false};
   case Ask::AidPlayer: return {1, 150.0f, 0.0, Who::SquadFirst, true};
   case Ask::DefendPlayer: return {3, 150.0f, 0.0, Who::Allies, true};
   case Ask::PickUpPlayer: return {1, 300.0f, 0.0, Who::Allies, true};
   case Ask::DestroyTarget: return {anyFriendly ? 4 : 0, 300.0f, 90.0, squad, false};
   case Ask::DefendTarget:
   case Ask::DefendSpot: return {anyFriendly ? 3 : 0, 300.0f, 90.0, squad, false};
   default: return {};
   }
}

// A brief help: over kHelpFor after one reached the player (`reached`, a time,
// below 0 for not yet), else kTravelFor after it was asked.
inline bool help_over(double now, double asked, double reached)
{
   return reached >= 0.0 ? now - reached >= kHelpFor : now - asked >= kTravelFor;
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

// Whom an order goes to, of `n` candidates (`dist` from where it is about,
// `ok` able to, `squad` in the player's squad), within `range`, as `who` and
// `want` say (Order), into `out` (room for `n`), squadmates first where both
// may go. How many.
inline int pick(const float* dist, const bool* ok, const bool* squad, int n, Who who, int want, float range, int* out)
{
   constexpr int kMax = 256;
   if (n > kMax) n = kMax;
   bool mates[kMax], others[kMax];
   for (int i = 0; i < n; ++i) {
      mates[i] = ok[i] && squad[i];
      others[i] = ok[i] && !squad[i];
   }
   int a[kMax], b[kMax];
   switch (who) {
   case Who::Anyone: return nearest(dist, ok, n, want, range, out);
   case Who::Allies: return nearest(dist, others, n, want, range, out);
   case Who::Squad: return nearest(dist, mates, n, 0, range, out);
   case Who::SquadFirst:
   case Who::SquadPlus: {
      const int m = nearest(dist, mates, n, who == Who::SquadPlus ? 0 : want, range, a);
      int count = 0;
      for (int k = 0; k < m; ++k) out[count++] = a[k];
      const int room = want > 0 ? want - count : 0;
      if (room <= 0) return count;
      const int o = nearest(dist, others, n, room, range, b);
      for (int k = 0; k < o; ++k) out[count++] = b[k];
      return count;
   }
   }
   return 0;
}

} // namespace comm_orders
