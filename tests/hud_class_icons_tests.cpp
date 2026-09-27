// Standalone tests (not a DLL build). Compile with /std:c++17 /W4 /WX, no NDEBUG.
#include "../PatcherDLL/src/render/hud_class_icons_core.hpp"
#include <cassert>
#include <cstdio>
#include <set>
#include <string>

using namespace hud_class_icons;

namespace {

struct Textures {
   std::set<uint32_t> names;
   int probes = 0;

   void add(const char* name) { names.insert(pbl_hash(name)); }
   bool operator()(uint32_t hash)
   {
      assert(hash != 0);
      ++probes;
      return names.count(hash) != 0;
   }
};

} // namespace

int main()
{
   // The engine stores HealthTexture as this hash (EntityClass::SetProperty).
   static_assert(pbl_hash("HealthTexture") == 0x0DB7468Eu);

   // A stored name hash extends to the full name's hash, case-folded like the engine.
   for (const char* base : { "rep_icon_rifleman", "Rep_Icon_Rifleman", "cis_inf_droideka", "a" })
      for (const char* suffix : { "_crouch", "_prone", "_ball", "" })
         assert(pbl_hash_append(pbl_hash(base), suffix) == pbl_hash((std::string(base) + suffix).c_str()));
   assert(pbl_hash_append(pbl_hash("REP_ICON"), "_CROUCH") == pbl_hash("rep_icon_crouch"));

   // SoldierState: only CROUCH (1) and PRONE (2) pick a variant.
   for (int state = -1; state < 32; ++state)
      assert(soldier_stance(state) == (state == 1 ? kCrouch : state == 2 ? kProne : kStand));

   // Droideka: fully balled only; the roll-up and unroll transitions stay standing.
   assert(droideka_stance(0x0C) == kBall);
   for (int state : { 0x00, 0x01, 0x04, 0x0B, 0x0D, 0x0E })
      assert(droideka_stance(state) == kStand);

   assert(!stance_suffix(kStand) && !stance_suffix(7));
   assert(std::string(stance_suffix(kBall)) == "_ball");

   // Fallback chain: prone -> crouch -> stand, crouch -> stand, then nothing.
   Textures loaded;
   loaded.add("rep_icon_rifleman");
   loaded.add("rep_icon_rifleman_crouch");
   const uint32_t base = pbl_hash("rep_icon_rifleman");
   assert(pick_texture(base, kCrouch, loaded) == pbl_hash("rep_icon_rifleman_crouch"));
   assert(pick_texture(base, kProne, loaded) == pbl_hash("rep_icon_rifleman_crouch")); // no _prone
   assert(pick_texture(base, kStand, loaded) == base);
   assert(pick_texture(pbl_hash("not_loaded"), kCrouch, loaded) == 0);
   const int before = loaded.probes;
   assert(pick_texture(0, kCrouch, loaded) == 0);         // no property: never probes
   assert(loaded.probes == before);

   Textures proneOnly;                                    // a prone icon never stands in for crouch
   proneOnly.add("rep_icon_sniper");
   proneOnly.add("rep_icon_sniper_prone");
   const uint32_t sniper = pbl_hash("rep_icon_sniper");
   assert(pick_texture(sniper, kProne, proneOnly) == pbl_hash("rep_icon_sniper_prone"));
   assert(pick_texture(sniper, kCrouch, proneOnly) == sniper);

   Textures baseOnly;                                     // no variants at all: always standing
   baseOnly.add("rep_icon_pilot");
   const uint32_t pilot = pbl_hash("rep_icon_pilot");
   for (uint32_t stance : { kStand, kCrouch, kProne, kBall })
      assert(pick_texture(pilot, stance, baseOnly) == pilot);
   baseOnly.probes = 0;
   assert(pick_texture(pilot, kProne, baseOnly) == pilot && baseOnly.probes == 3); // prone, crouch, base
   static_assert(fallback_stance(kProne) == kCrouch && fallback_stance(kCrouch) == kStand
                 && fallback_stance(kBall) == kStand);

   loaded.add("cis_icon_droideka_ball");
   loaded.add("cis_icon_droideka");
   assert(pick_texture(pbl_hash("cis_icon_droideka"), kBall, loaded) == pbl_hash("cis_icon_droideka_ball"));

   // The table is probed only when the class or stance changes.
   Pick pick;
   loaded.probes = 0;
   for (int frame = 0; frame < 1000; ++frame)
      assert(pick.get(base, kCrouch, loaded) == pbl_hash("rep_icon_rifleman_crouch"));
   assert(loaded.probes == 1);
   assert(pick.get(base, kStand, loaded) == base && loaded.probes == 2);
   assert(pick.get(base, kCrouch, loaded) && loaded.probes == 3);
   pick.invalidate();
   assert(pick.get(base, kCrouch, loaded) && loaded.probes == 4);

   // Sent once per mission, then only on change.
   Published sent;
   assert(sent.set(0) && !sent.set(0) && sent.set(5) && !sent.set(5));
   sent.invalidate();
   assert(sent.set(5));

   std::puts("Class icon tests passed (hash suffixes, stance maps, prone/crouch/stand fallback, probe caching, change detection).");
}
