// Standalone tests (not a DLL build) for who leads and who joins an AI squad:
// the plan in ai_squads_core.hpp, fed what ai_squads.cpp reads of every soldier.
#include "../PatcherDLL/src/ai/ai_squads_core.hpp"
#include <cassert>
#include <cstdio>
#include <limits>

using namespace ai_squads;

namespace {

constexpr float kRange = 20.0f;

Unit trooper(float x, float z, int team = 1)
{
   Unit u;
   u.team = team;
   u.x = x;
   u.z = z;
   u.rank = 1;
   u.joins = true;
   u.size = 3;
   u.since = 10.0;
   u.travelling = true;
   return u;
}

int plan_of(const Unit* units, int n, Recruit* out, float range = kRange, int maxOut = kMaxUnits)
{
   return plan(units, n, range, out, maxOut);
}

bool has(const Recruit* r, int n, int follower, int leader)
{
   for (int i = 0; i < n; ++i)
      if (r[i].follower == follower && r[i].leader == leader) return true;
   return false;
}

void two_troopers_make_a_squad()
{
   Unit u[2] = {trooper(0, 0), trooper(5, 0)};
   u[1].since = 5.0;   // alive longer: it leads
   Recruit r[8];
   const int n = plan_of(u, 2, r);
   assert(n == 1 && r[0].follower == 0 && r[0].leader == 1);
}

void a_lone_soldier_leads_nobody()
{
   Unit u[1] = {trooper(0, 0)};
   Recruit r[8];
   assert(plan_of(u, 1, r) == 0);
}

void teams_and_range()
{
   Unit u[3] = {trooper(0, 0, 1), trooper(5, 0, 2), trooper(25, 0, 1)};
   Recruit r[8];
   assert(plan_of(u, 3, r) == 0);           // the enemy and the far friend stay out
   assert(plan_of(u, 3, r, 30.0f) == 1);    // a wider range takes the friend
   assert(plan_of(u, 3, r, 0.0f) == 0);
   assert(plan_of(u, 3, r, std::numeric_limits<float>::quiet_NaN()) == 0);
   assert(plan_of(u, 3, r, -30.0f) == 0);   // squared, a negative range would read as a wide one
   Unit same[2] = {trooper(0, 0), trooper(0, 0)};
   assert(plan_of(same, 2, r, 0.0f) == 0);
}

void nearest_first_not_first_listed()
{
   Unit u[4] = {trooper(0, 0), trooper(9, 0), trooper(1, 0), trooper(5, 0)};
   u[0].since = 1.0;
   u[0].size = 2;
   Recruit r[8];
   const int n = plan_of(u, 4, r);
   assert(has(r, n, 2, 0) && has(r, n, 3, 0) && !has(r, n, 1, 0));
}

void the_squad_size_and_nearest_first()
{
   Unit u[6] = {trooper(0, 0), trooper(1, 0), trooper(2, 0), trooper(3, 0), trooper(4, 0), trooper(5, 0)};
   u[0].since = 1.0;   // the leader
   u[0].size = 2;
   for (int i = 1; i < 6; ++i) u[i].size = 2;
   Recruit r[8];
   const int n = plan_of(u, 6, r);
   assert(has(r, n, 1, 0) && has(r, n, 2, 0));   // the two nearest
   // The other three make a squad of their own: the first of them leads two.
   int led = 0;
   for (int i = 0; i < n; ++i) led += r[i].leader == 0;
   assert(led == 2 && n == 4);
}

void rank_decides_who_leads()
{
   Unit u[3] = {trooper(0, 0), trooper(3, 0), trooper(6, 0)};
   u[2].rank = 3;      // an officer, and the youngest
   u[2].since = 99.0;
   Recruit r[8];
   const int n = plan_of(u, 3, r);
   assert(n == 2 && has(r, n, 0, 2) && has(r, n, 1, 2));
   // A trooper never takes an officer in.
   Unit v[2] = {trooper(0, 0), trooper(3, 0)};
   v[1].rank = 3;
   v[1].size = 0;      // an officer who leads nobody
   assert(plan_of(v, 2, r) == 0);
}

void who_never_leads_or_joins()
{
   Unit u[3] = {trooper(0, 0), trooper(3, 0), trooper(6, 0)};
   u[0].rank = 0;      // a hero: never leads...
   u[0].joins = false; // ...and never joins
   u[0].since = 0.0;
   Recruit r[8];
   const int n = plan_of(u, 3, r);
   assert(n == 1 && r[0].follower != 0 && r[0].leader != 0);
   // Everyone at rank 0 (AI squads off and no class says otherwise): nothing.
   for (Unit& x : u) x.rank = 0;
   assert(plan_of(u, 3, r) == 0);
}

void followers_are_taken()
{
   Unit u[3] = {trooper(0, 0), trooper(3, 0), trooper(6, 0)};
   u[1].free = false;  // follows someone already: neither leads nor joins
   u[1].since = 0.0;
   Recruit r[8];
   const int n = plan_of(u, 3, r);
   assert(n == 1 && r[0].follower != 1 && r[0].leader != 1);
}

void combat()
{
   Unit u[3] = {trooper(0, 0), trooper(3, 0), trooper(6, 0)};
   u[0].since = 1.0;
   u[0].combat = true;   // a fighting soldier starts no squad...
   Recruit r[8];
   int n = plan_of(u, 3, r);
   assert(n == 1 && r[0].leader != 0 && r[0].follower != 0);   // ...and is not taken from its fight
   // A leader fighting with its squad still fills it, from those not fighting.
   u[0].followers = 1;
   u[0].size = 3;
   n = plan_of(u, 3, r);
   assert(n == 2 && has(r, n, 1, 0) && has(r, n, 2, 0));
}

void squads_fill_before_new_ones_start()
{
   Unit u[4] = {trooper(0, 0), trooper(3, 0), trooper(4, 0), trooper(5, 0)};
   u[0].followers = 2;   // a leader one short of 3
   u[0].since = 50.0;    // younger than the others, but it already leads
   for (int i = 1; i < 4; ++i) u[i].since = 1.0;
   Recruit r[8];
   const int n = plan_of(u, 4, r);
   assert(has(r, n, 1, 0));                 // the nearest fills its squad
   assert(n == 2 && has(r, n, 3, 2));       // the last two pair off
   // A full squad takes no more.
   u[0].followers = 3;
   const int m = plan_of(u, 4, r);
   for (int i = 0; i < m; ++i) assert(r[i].leader != 0);
}

void a_leader_is_not_recruited()
{
   Unit u[3] = {trooper(0, 0), trooper(3, 0), trooper(30, 0)};
   u[0].followers = 1;   // leads already, full at size 1
   u[0].size = 1;
   u[1].since = 1.0;
   Recruit r[8];
   assert(plan_of(u, 3, r) == 0);   // u[1] has nobody it may take
}

void output_bounds_and_determinism()
{
   Unit u[10];
   for (int i = 0; i < 10; ++i) u[i] = trooper((float)i, 0);
   Recruit a[16], b[16];
   const int n = plan_of(u, 10, a);
   assert(plan_of(u, 10, b) == n);
   for (int i = 0; i < n; ++i) assert(a[i].follower == b[i].follower && a[i].leader == b[i].leader);
   assert(plan_of(u, 10, a, kRange, 2) == 2);
   // No unit is recruited twice or both leads and follows.
   int role[10] = {};
   for (int i = 0; i < n; ++i) {
      assert(role[a[i].follower] == 0);
      role[a[i].follower] = 1;
   }
   for (int i = 0; i < n; ++i) assert(role[a[i].leader] != 1);
}

// Only a soldier on its way somewhere recruits, new squads and old.
void only_the_travelling_lead()
{
   Unit u[3] = {trooper(0, 0), trooper(3, 0), trooper(6, 0)};
   u[0].since = 1.0;
   u[0].travelling = false;   // a sniper at its spot: leads nobody...
   Recruit r[8];
   int n = plan_of(u, 3, r);
   assert(n == 2 && has(r, n, 0, 1) && has(r, n, 2, 1));   // ...but may still join one
   // Its squad from before keeps who it has, and takes nobody more.
   u[0].followers = 1;
   u[1].free = false;
   assert(plan_of(u, 3, r) == 0);
   // Nobody travelling: no squads.
   for (Unit& x : u) {
      x.travelling = false;
      x.followers = 0;
      x.free = true;
   }
   assert(plan_of(u, 3, r) == 0);
   assert(travelling(kTravelStart) && !travelling(kTravelStart - 0.5f));
   assert(!travelling(std::numeric_limits<float>::quiet_NaN()));
   assert(!travelling(std::numeric_limits<float>::infinity()));   // a destination read as garbage
}

void idle_leaders_let_go()
{
   Anchor a;
   a = next_anchor(a, 0.0f, 0.0f, false, 100.0);
   assert(a.valid && !idle(a, 100.0));
   // Shuffling about on the spot is standing.
   a = next_anchor(a, 1.5f, -1.0f, false, 110.0);
   assert(!idle(a, 110.0) && idle(a, 100.0 + kIdleSeconds));
   // Moving off starts the clock again.
   Anchor b = next_anchor(a, 5.0f, 0.0f, false, 118.0);
   assert(!idle(b, 118.0 + kIdleSeconds - 0.5) && idle(b, 118.0 + kIdleSeconds));
   // A fight holds the clock back while it lasts.
   Anchor c = next_anchor(a, 0.5f, 0.0f, true, 119.0);
   assert(!idle(c, 119.0 + kIdleSeconds - 0.5) && idle(c, 119.0 + kIdleSeconds));
   assert(!idle(Anchor{}, 1e9));
}

// A recruit whose order still waits stays a member for a while, then has to
// answer that it follows.
void members_while_their_order_waits()
{
   assert(keep_member(false, 100.0, 100.0));                    // just sent
   assert(keep_member(false, 100.0, 100.0 + kJoinGrace - 0.5));
   assert(!keep_member(false, 100.0, 100.0 + kJoinGrace));      // never took it up
   assert(keep_member(true, 100.0, 100.0 + 1000.0));            // following, however long ago
   assert(kJoinGrace >= 2.0 && kJoinGrace <= 15.0);
}

} // namespace

int main()
{
   two_troopers_make_a_squad();
   a_lone_soldier_leads_nobody();
   teams_and_range();
   nearest_first_not_first_listed();
   the_squad_size_and_nearest_first();
   rank_decides_who_leads();
   who_never_leads_or_joins();
   followers_are_taken();
   combat();
   squads_fill_before_new_ones_start();
   a_leader_is_not_recruited();
   output_bounds_and_determinism();
   members_while_their_order_waits();
   only_the_travelling_lead();
   idle_leaders_let_go();
   std::printf("AI squad tests passed (pairs, loners, teams and range, size and nearest, rank, heroes, "
               "followers, combat, filling first, leaders kept, bounds, joining members, travelling "
               "leaders, idle squads).\n");
   return 0;
}
