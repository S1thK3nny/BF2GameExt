// Standalone tests (not a DLL build) for the comm keys in single player: which
// keys went down, what each asks of the AI for what the player aims at, and who
// answers - the logic in comm_orders_core.hpp, fed what comm_orders.cpp reads.
#include "../PatcherDLL/src/ai/comm_orders_core.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace comm_orders;

namespace {

const float kNan = std::numeric_limits<float>::quiet_NaN();
const float kInf = std::numeric_limits<float>::infinity();

// The keys: inputs 20 to 27, each counted once as it goes down.
void keys()
{
   uint32_t last = 0;
   assert(pressed(0, last) == 0 && last == 0);
   assert(pressed(1u << 20, last) == (1u << kSpotted) && last == 1u);   // F5 down
   assert(pressed(1u << 20, last) == 0);                               // held: once
   assert(pressed((1u << 20) | (1u << 27), last) == (1u << kDefend));    // F12 joins it
   assert(pressed(1u << 27, last) == 0 && last == (1u << kDefend));     // F5 up
   assert(pressed((1u << 20) | (1u << 27), last) == (1u << kSpotted));   // F5 again
   assert(pressed(0, last) == 0 && last == 0);
   // Only the eight comm inputs count: 19 (Comm_Chat before) and 28 do not.
   assert(pressed((1u << 19) | (1u << 28), last) == 0 && last == 0);
   assert(pressed(0xFFFFFFFFu, last) == 0xFFu && last == 0xFFu);
   assert(kFirstInput == 20 && kCodes == 8 && kAttack == 6 && kDefend == 7 && kBackup == 5);
}

// A place they can be sent: walkable, within reach.
void spots()
{
   assert(reachable_spot(1.0f, 10.0f) && reachable_spot(kWalkableUp, kSpotRange) && reachable_spot(0.9f, 0.0f));
   assert(!reachable_spot(0.69f, 10.0f));        // a wall or a cliff face
   assert(!reachable_spot(1.0f, kSpotRange + 1.0f) && !reachable_spot(1.0f, -1.0f));
   assert(!reachable_spot(kNan, 10.0f) && !reachable_spot(1.0f, kNan) && !reachable_spot(1.0f, kInf));
   assert(reachable_spot(1.0f, 399.0f) && !reachable_spot(1.0f, 401.0f) && kSpotRange == 400.0f && kWalkableUp == 0.7f);
}

// What each key asks for what the crosshair is on (the user's design).
void asks()
{
   // Spotted: an enemy only.
   assert(ask_for(kSpotted, Aim::Enemy, false) == Ask::ShootTarget);
   assert(ask_for(kSpotted, Aim::Friend, true) == Ask::Nothing && ask_for(kSpotted, Aim::Spot, true) == Ask::Nothing);
   assert(ask_for(kSpotted, Aim::None, false) == Ask::Nothing);
   // The aid keys and Backup: whatever is aimed at.
   for (const Aim a : {Aim::None, Aim::Enemy, Aim::Friend, Aim::Spot}) {
      assert(ask_for(kMedic, a, false) == Ask::AidPlayer && ask_for(kRepair, a, true) == Ask::AidPlayer);
      assert(ask_for(kAmmo, a, false) == Ask::AidPlayer);
      assert(ask_for(kBackup, a, false) == Ask::DefendPlayer);
      assert(ask_for(kPickup, a, true) == Ask::PickUpPlayer);
   }
   // Attack: the enemy aimed at, else a place they can reach.
   assert(ask_for(kAttack, Aim::Enemy, false) == Ask::DestroyTarget);
   assert(ask_for(kAttack, Aim::Spot, true) == Ask::DefendSpot && ask_for(kAttack, Aim::Spot, false) == Ask::Nothing);
   assert(ask_for(kAttack, Aim::Friend, true) == Ask::Nothing && ask_for(kAttack, Aim::None, true) == Ask::Nothing);
   // Defend: "aim at a friendly under the cross hair, OR if nothing and they can
   // reach it then the position you are aiming at".
   assert(ask_for(kDefend, Aim::Friend, false) == Ask::DefendTarget);
   assert(ask_for(kDefend, Aim::Spot, true) == Ask::DefendSpot && ask_for(kDefend, Aim::Spot, false) == Ask::Nothing);
   assert(ask_for(kDefend, Aim::Enemy, true) == Ask::Nothing && ask_for(kDefend, Aim::None, true) == Ask::Nothing);
   assert(ask_for(-1, Aim::Enemy, true) == Ask::Nothing && ask_for(kCodes, Aim::Enemy, true) == Ask::Nothing);
}

// How many answer, from how far, for how long.
void orders()
{
   assert(order_for(Ask::Nothing).units == 0 && order_for(Ask::Nothing).range == 0.0f);
   const Order shoot = order_for(Ask::ShootTarget), aid = order_for(Ask::AidPlayer);
   const Order backup = order_for(Ask::DefendPlayer), attack = order_for(Ask::DestroyTarget);
   assert(shoot.units == 0 && shoot.range == 150.0f && shoot.lasts == 0.0);   // all who see it, at once
   assert(aid.units == 1 && aid.range == 300.0f && aid.lasts == 45.0);
   assert(backup.units == 3 && backup.range == 200.0f && backup.lasts == 60.0);
   assert(attack.units == 4 && attack.range == 250.0f && attack.lasts == 90.0);
   assert(order_for(Ask::DefendTarget).units == 3 && order_for(Ask::DefendSpot).lasts == 90.0);
   const Order pickup = order_for(Ask::PickUpPlayer);
   assert(pickup.units == 1 && pickup.range == 400.0f && pickup.lasts == 60.0);
}

// Who answers: the nearest that may, within range, nearest first.
void who()
{
   const float dist[] = {50.0f, 10.0f, 300.0f, 20.0f, kNan, 10.0f, 199.0f};
   const bool ok[] = {true, true, true, true, true, false, true};
   int out[7];
   assert(nearest(dist, ok, 7, 3, 200.0f, out) == 3 && out[0] == 1 && out[1] == 3 && out[2] == 0);
   assert(nearest(dist, ok, 7, 0, 200.0f, out) == 4 && out[3] == 6);   // all: the 300 m one and the NaN left out
   assert(nearest(dist, ok, 7, 10, 25.0f, out) == 2 && out[0] == 1 && out[1] == 3);
   assert(nearest(dist, ok, 7, 1, 5.0f, out) == 0);
   // A tie: the earlier first.
   const float tie[] = {30.0f, 10.0f, 10.0f};
   const bool all[] = {true, true, true};
   assert(nearest(tie, all, 3, 2, 100.0f, out) == 2 && out[0] == 1 && out[1] == 2);
   assert(nearest(tie, all, 0, 2, 100.0f, out) == 0);
   assert(nearest(tie, all, 3, 0, 30.0f, out) == 3 && out[2] == 0);   // the range itself is in reach
}

} // namespace

int main()
{
   keys();
   spots();
   asks();
   orders();
   who();
   std::printf("Comm key tests passed (keys, places, what each key asks, how many answer, who answers).\n");
   return 0;
}
