#include "pch.h"
#include "content_census.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

// See content_census.hpp for what this reports and why it reports each number
// twice where it can.

int  g_contentCensusInterval = 0;
bool g_contentCensusNames    = false;

namespace {

constexpr uint32_t kEffectClassSlots = 256; // key slots; values follow at +256

// HeapObj / Tag layout, taken from the surviving accessors rather than a PDB.
// See game_addrs.hpp for the instructions that prove each one.
constexpr uint32_t kHeapStride    = 0x24;
constexpr uint32_t kHeapFreeHead  = 0x04;
constexpr uint32_t kHeapSize      = 0x14;
constexpr uint32_t kHeapName      = 0x18;
constexpr uint32_t kTagNext       = 0x04;
constexpr uint32_t kTagSize       = 0x08;

// The walk runs on our own thread while the game may be allocating, so a torn
// list is possible. Bound every traversal and treat the bound as "unknown"
// rather than reporting a half-summed figure as fact.
constexpr uint32_t kMaxHeaps     = 64;
constexpr uint32_t kMaxFreeNodes = 65536;

// Previous sample, for deltas. A one-shot number tells you where you are; the
// delta tells you where you are going, which is what catches a leak.
uint32_t s_prevUsed[kMaxHeaps] = {};
bool     s_havePrev = false;

uintptr_t s_exeBase = 0;
HANDLE    s_thread  = nullptr;
HANDLE    s_stop    = nullptr;

void census_log(const char* fmt, ...)
{
   FILE* f = nullptr;
   if (fopen_s(&f, "BF2GameExt.log", "a") != 0 || !f) return;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fclose(f);
}

// A bar that makes "you are nearly out" obvious at a glance in a wall of log.
const char* pressure(uint32_t used, uint32_t cap)
{
   if (cap == 0) return "";
   const uint32_t pct = (used * 100) / cap;
   if (pct >= 100) return "  <-- FULL";
   if (pct >= 90)  return "  <-- 90%+";
   if (pct >= 75)  return "  <-- 75%+";
   return "";
}

// Count non-zero keys directly. This is the number that cannot be wrong: it does
// not depend on the engine maintaining a counter correctly, only on the table
// base being right, and a wrong base shows up immediately as a nonsense figure
// rather than as a plausible lie.
bool scan_effect_classes(uint32_t* outScan, uint32_t* outCounter)
{
   if (g_addr->fx_effect_classes_table == 0 || g_addr->fx_effect_classes_count == 0)
      return false;

   const uint32_t* const keys =
      reinterpret_cast<const uint32_t*>(resolve(s_exeBase, g_addr->fx_effect_classes_table));
   const uint32_t* const counter =
      reinterpret_cast<const uint32_t*>(resolve(s_exeBase, g_addr->fx_effect_classes_count));

   uint32_t used = 0;
   __try {
      for (uint32_t i = 0; i < kEffectClassSlots; ++i)
         if (keys[i] != 0) ++used;
      *outCounter = *counter;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
   *outScan = used;
   return true;
}

// ===========================================================================
// Class registries: entity, weapon, ordnance, explosion.
//
// Four Factory<> lists, all the same template, so one walker serves all four.
// See game_addrs.hpp for the head addresses and why only the head is stored.
//
// Three rules here were each paid for by a bug, in this project or the engine:
//
//   1. TERMINATE ON `node == head`, NEVER on `_pObject == 0`.  The destructor
//      NULLs _pObject while the node is STILL LINKED (modtools 0x004D073E; the
//      unlink is not until 0x004D0744), so using the engine's own terminator
//      truncates the walk during teardown and silently under-reports.  Skip
//      such a node and keep going.
//   2. NEVER CALL A VIRTUAL on a walked object.  The Factory<> base vftable has
//      only THREE slots, and an object carries it in two windows during which
//      it is fully linked and reachable.  IsRtti sits past the end of it -- on
//      modtools that slot reads 0, so an RTTI-based bucketer would call null
//      from the diagnostic thread.  __try catches an access violation; it does
//      not contain execution that has wandered into .rdata.
//   3. BOUND EVERY LOOP, including the hash probe.  The scar is
//      PblHashTableCode::_Find (modtools 0x007E1A40), whose probe has no
//      iteration counter and spins forever on a full table with an absent key.
//
// BUCKETING is by parent chain, not by type test.  Every ODF class carries
// mParent (+0x14) and mId (+0x18), and both are written BEFORE the node becomes
// reachable -- so they are the only object fields guaranteed valid for every
// node a walker can see.  Following mParent lands on a built-in root whose mId
// is one of the constants below, and those are just FNV hashes of plaintext
// names.  Crucially the chain is followed through an index the census built
// from its OWN first pass, not through live pointers: a scan that can only
// match, rather than arithmetic that can only be believed.
// ===========================================================================

constexpr uint32_t kMaxClasses = 4096;  // walk cap; ~10x the largest real map
constexpr uint32_t kIndexSlots = 8192;  // power of two, > 2 * kMaxClasses
constexpr uint32_t kMaxChain   = 16;    // diagnostic cap, not an engine fact

constexpr uint32_t kListNext   = 0x04;  // PblList node
constexpr uint32_t kListObject = 0x0C;
constexpr uint32_t kListCount  = 0x10;
constexpr uint32_t kNodeToObj  = 0x04;  // _pObject == node - 4, always
constexpr uint32_t kFacParent  = 0x14;  // Factory<> object fields
constexpr uint32_t kFacId      = 0x18;
constexpr uint32_t kNameMax    = 32;

enum Family { FAM_SOLDIER, FAM_VEHICLE, FAM_TURRET, FAM_BUILDING,
              FAM_CMDPOST, FAM_PICKUP, FAM_SCENERY, FAM_COUNT };

struct Root { uint32_t hash; const char* name; uint8_t family; };

// The 46 built-in entity roots, in creation order, from
// GameState::CreateBaseEntityClasses (modtools 0x0044CDA0, Steam 0x00539F60 --
// same names, same order, so this table is build-invariant).  All 46 are
// created through the ONE-ARGUMENT ctor, which zeroes +0x14, so every root has
// mParent == 0 and the chain always terminates.
const Root kEntityRoots[] = {
   {0xD93E11F8u, "prop",                         FAM_BUILDING},
   {0x38DF0375u, "building",                     FAM_BUILDING},
   {0x15F2C005u, "animatedprop",                 FAM_BUILDING},
   {0x6F31A7B7u, "destructablebuilding",         FAM_BUILDING},
   {0x8F83FB9Cu, "animatedbuilding",             FAM_BUILDING},
   {0x1618AB4Du, "armedanimatedbuilding",        FAM_BUILDING},
   {0x6AF75F5Cu, "armedbuilding",                FAM_BUILDING},
   {0x90AA3ACDu, "armedbuildingdynamic",         FAM_BUILDING},
   {0x21F5B729u, "door",                         FAM_BUILDING},
   {0x60809302u, "trap",                         FAM_BUILDING},
   {0x8908744Fu, "remoteterminal",               FAM_BUILDING},
   {0x9CCD1FB5u, "soldier",                      FAM_SOLDIER},
   {0xEDDB2A39u, "droid",                        FAM_SOLDIER},
   {0xACFA2C67u, "walker",                       FAM_VEHICLE},
   {0x31AF4F57u, "walkerdroid",                  FAM_VEHICLE},
   {0x040472D5u, "flyer",                        FAM_VEHICLE},
   {0xD7241CF7u, "carrier",                      FAM_VEHICLE},
   {0x70CEAB2Du, "hover",                        FAM_VEHICLE},
   {0xEB655C56u, "treaded",                      FAM_VEHICLE},
   {0x47142254u, "commandhover",                 FAM_VEHICLE},
   {0xD8CB53C8u, "commandwalker",                FAM_VEHICLE},
   {0xDE0A35A8u, "commandflyer",                 FAM_VEHICLE},
   {0x8046B5F4u, "portableturret",               FAM_TURRET},
   {0x1CBDCAF1u, "defensegridturret",            FAM_TURRET},
   {0x844CCDB4u, "commandpost",                  FAM_CMDPOST},
   {0xC6C70945u, "commandarmedbuilding",         FAM_CMDPOST},
   {0x1857FE4Cu, "commandarmedanimatedbuilding", FAM_CMDPOST},
   {0x23EBDCEAu, "vehiclespawn",                 FAM_PICKUP},
   {0x0BD75EA0u, "vehiclepad",                   FAM_PICKUP},
   {0xA3097771u, "powerupstation",               FAM_PICKUP},
   {0x778AAD1Cu, "powerupitem",                  FAM_PICKUP},
   {0x7E89EDB6u, "mine",                         FAM_PICKUP},
   {0xBDEAA8D7u, "flag",                         FAM_PICKUP},
   {0xBB6C27CAu, "SoundAmbienceStatic",          FAM_SCENERY},
   {0xEC9E9B0Cu, "SoundAmbienceStreaming",       FAM_SCENERY},
   {0xC8E4C134u, "dusteffect",                   FAM_SCENERY},
   {0xCD836D98u, "cloudcluster",                 FAM_SCENERY},
   {0x156B70A1u, "grasspatch",                   FAM_SCENERY},
   {0x6A6FB399u, "leafpatch",                    FAM_SCENERY},
   // Created from the bare constant in both builds -- there is no "godray"
   // string in any image.  The hash is read; the spelling is inferred from it
   // and from retail's GodrayClass symbol.
   {0xBA5B0537u, "godray",                       FAM_SCENERY},
   {0xE29D1E2Fu, "light",                        FAM_SCENERY},
   {0x179D1742u, "hologram",                     FAM_SCENERY},
   {0xCBFCA59Du, "rumbleeffect",                 FAM_SCENERY},
   {0xC658A4EDu, "effectprop",                   FAM_SCENERY},
   {0x768301F0u, "asteroid",                     FAM_SCENERY},
   {0x0CC8D20Bu, "cloth",                        FAM_SCENERY},
};

// GameState::CreateBaseWeaponClasses, modtools 0x0044C960.
const Root kWeaponRoots[] = {
   {0x6F332041u, "weapon", 0},              {0x3B4030E9u, "areaeffectweapon", 0},
   {0x524F8827u, "binoculars", 0},          {0x27A2DF1Cu, "cannon", 0},
   {0x7127EE91u, "catapult", 0},            {0xEA8C85E7u, "destruct", 0},
   {0xB57FA993u, "detonator", 0},           {0xB5079DC2u, "disguise", 0},
   {0x4961E1A8u, "dispenser", 0},           {0x6B4A3E34u, "grapplinghookweapon", 0},
   {0xD988D8FDu, "grenade", 0},             {0xE7EDCAEEu, "invisibility", 0},
   {0x9DAC718Cu, "laser", 0},               {0xE3468391u, "launcher", 0},
   {0x29520BB4u, "repair", 0},              {0x79CA637Bu, "melee", 0},
   {0xB82A9EBDu, "meleethrow", 0},          {0xC78D7953u, "remote", 0},
   {0x337519B0u, "shield", 0},              {0xC5222DB2u, "towcableweapon", 0},
};

// GameState::CreateBaseOrdnanceClasses, modtools 0x0044C630.  `grenade` and
// `laser` appear in the weapon table too -- different registries, do not merge.
const Root kOrdnanceRoots[] = {
   {0xCE96DD46u, "bolt", 0},                {0x5D826019u, "beacon", 0},
   {0xA75279FAu, "beam", 0},                {0xE894A379u, "bullet", 0},
   {0x947FEB55u, "emitterordnance", 0},     {0x5AD3F2D0u, "fatray", 0},
   {0x29D633A0u, "grapplinghook", 0},       {0xD988D8FDu, "grenade", 0},
   {0x037675B4u, "haywire", 0},             {0x9DAC718Cu, "laser", 0},
   {0x85E836A5u, "meleethrowordnance", 0},  {0xA4EF6883u, "missile", 0},
   {0x11E1FC01u, "shell", 0},               {0xE3FD289Au, "sticky", 0},
   {0xDBCA71D2u, "towcable", 0},
};

// One root, and its ctor never goes through Read, so its name is always empty.
const Root kExplosionRoots[] = { {0x02BB1FE0u, "explosion", 0} };

struct Registry {
   const char*  label;
   uintptr_t    head;
   uintptr_t    nameOff;   // 0 = this build has no name buffer for this kind
   uintptr_t    baseVft;   // 0 = not read for this build; only a label is lost
   const Root*  roots;
   uint32_t     rootCount;
   bool         families;  // print the seven-family breakdown
};

struct ClassRec {
   uint32_t obj, mId, mParent;
   uint32_t rootId;
   uint32_t vptr;          // the object's C++ type, used to attribute self-roots
   uint8_t  constructing;
};

struct WalkResult {
   bool     ok;
   uint32_t scanned;
   int32_t  engine;
   uint32_t fam[FAM_COUNT];
   uint32_t constructing, unlinking, orphan, cycle, unknownRoot;
   uint32_t unnamed, truncated, mismatch;
   uint32_t byType;        // attributed by vptr because the chain could not
   char     err[192];      // non-empty => walk aborted; err says exactly why
   // Distinct roots we could not name, so the report can say WHICH ones rather
   // than only how many. Eight is plenty: a handful of unknown roots is a
   // finding, hundreds would mean the mechanism itself is wrong.
   uint32_t unkId[8];
   uint32_t unkCount[8];
   uint32_t unkRec[8];
   char     unkName[8][kNameMax + 1];
   uint32_t unkDistinct;
};

// Census-thread only.  4096 * 20 B + 8192 * 4 B, in .bss rather than on a stack
// this code does not own.
ClassRec s_rec[kMaxClasses];
int32_t  s_idx[kIndexSlots];

// The engine's PblHash: FNV-1a 32, basis 0x811C9DC5, prime 0x01000193, each
// byte sign-extended then OR'd with 0x20 (modtools 0x007E1B70).  Used only to
// check a recovered name against the mId the engine already stored -- a match
// proves both, a mismatch is reported rather than resolved.
uint32_t pbl_hash(const char* str, uint32_t maxLen)
{
   uint32_t h = 0x811C9DC5u;
   for (uint32_t i = 0; i < maxLen && str[i]; ++i) {
      const uint32_t b = ((uint32_t)(int32_t)(int8_t)str[i]) | 0x20u;
      h = (h ^ b) * 0x01000193u;
   }
   return h;
}

const Root* find_root(const Registry& R, uint32_t id)
{
   for (uint32_t i = 0; i < R.rootCount; ++i)
      if (R.roots[i].hash == id) return &R.roots[i];
   return nullptr;
}

// Pass 1: collect.  Everything that can fault is inside the one __try, and any
// structural surprise stops the walk with a message rather than continuing to a
// plausible-looking total.
bool collect(const Registry& R, WalkResult* W, uint32_t* outCount)
{
   const uintptr_t head = (uintptr_t)resolve(s_exeBase, R.head);
   const uintptr_t lo   = g_addr->rdata_lo ? (uintptr_t)resolve(s_exeBase, g_addr->rdata_lo) : 0;
   const uintptr_t hi   = g_addr->rdata_hi ? (uintptr_t)resolve(s_exeBase, g_addr->rdata_hi) : 0;
   const uintptr_t vft  = R.baseVft ? (uintptr_t)resolve(s_exeBase, R.baseVft) : 0;

   uint32_t n = 0;
   __try {
      W->engine = *reinterpret_cast<const int32_t*>(head + kListCount);
      uintptr_t node = *reinterpret_cast<const uintptr_t*>(head + kListNext);

      while (n < kMaxClasses) {
         if (node == head) { *outCount = n; return true; }   // the terminator
         if (node == 0 || (node & 3) != 0 || node < 0x10000) {
            _snprintf_s(W->err, sizeof(W->err), _TRUNCATE,
                        "bad node ptr 0x%08X at step %u", (unsigned)node, n);
            *outCount = n;
            return false;
         }
         const uintptr_t obj = *reinterpret_cast<const uintptr_t*>(node + kListObject);
         if (obj == 0) {                       // mid-teardown, still linked
            ++W->unlinking;
            node = *reinterpret_cast<const uintptr_t*>(node + kListNext);
            continue;
         }
         if (obj != node - kNodeToObj) {
            _snprintf_s(W->err, sizeof(W->err), _TRUNCATE,
                        "obj 0x%08X != node-4 at step %u -- list corrupt",
                        (unsigned)obj, n);
            *outCount = n;
            return false;
         }
         const uintptr_t vptr = *reinterpret_cast<const uintptr_t*>(obj);
         if (lo != 0 && (vptr < lo || vptr >= hi)) {
            _snprintf_s(W->err, sizeof(W->err), _TRUNCATE,
                        "vptr 0x%08X out of .rdata at step %u", (unsigned)vptr, n);
            *outCount = n;
            return false;
         }

         s_rec[n].obj          = (uint32_t)obj;
         s_rec[n].mId          = *reinterpret_cast<const uint32_t*>(obj + kFacId);
         s_rec[n].mParent      = *reinterpret_cast<const uint32_t*>(obj + kFacParent);
         s_rec[n].rootId       = 0;
         s_rec[n].vptr         = (uint32_t)vptr;
         s_rec[n].constructing = (vft != 0 && vptr == vft) ? 1 : 0;
         ++n;

         node = *reinterpret_cast<const uintptr_t*>(node + kListNext);
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      _snprintf_s(W->err, sizeof(W->err), _TRUNCATE,
                  "unreadable this tick (faulted at step %u)", n);
      *outCount = n;
      return false;
   }

   _snprintf_s(W->err, sizeof(W->err), _TRUNCATE,
               "walk overran %u -- counts are a FLOOR, not a total", kMaxClasses);
   *outCount = n;
   return false;
}

// A vptr -> family map LEARNED from this same pass, not baked into a table.
//
// Not every CreateClass records a parent: some route to EntityClass::EntityClass
// (modtools 0x004D0B50), which zeroes +0x14 at 0x004D0B6B. A class made that way
// has mParent == 0, so it becomes its OWN root and no chain can attribute it --
// com_amb_soundstatic400m, com_rum_atmos and the cloth icons all behave this way.
//
// But such a class is the same C++ class as the built-in root of its type, so
// its vptr matches that root's. Reading the vptr of each of the 46 built-ins
// gives a type map derived from the game's own data, with nothing hardcoded and
// nothing to drift per build. A vptr shared by roots of DIFFERENT families is
// marked ambiguous and never used -- an unattributed class is a finding, a
// wrongly attributed one is a lie.
struct VtypeEntry { uint32_t vptr; uint8_t family; bool ambiguous; };
VtypeEntry s_vtype[64];
uint32_t   s_vtypeCount;

void learn_vtypes(const Registry& R, uint32_t n)
{
   s_vtypeCount = 0;
   for (uint32_t i = 0; i < n; ++i) {
      if (s_rec[i].mParent != 0 || s_rec[i].constructing) continue;
      const Root* const r = find_root(R, s_rec[i].mId);
      if (r == nullptr) continue;                     // a self-root, not a built-in
      bool merged = false;
      for (uint32_t k = 0; k < s_vtypeCount; ++k) {
         if (s_vtype[k].vptr != s_rec[i].vptr) continue;
         if (s_vtype[k].family != r->family) s_vtype[k].ambiguous = true;
         merged = true;
         break;
      }
      if (!merged && s_vtypeCount < 64) {
         s_vtype[s_vtypeCount].vptr      = s_rec[i].vptr;
         s_vtype[s_vtypeCount].family    = r->family;
         s_vtype[s_vtypeCount].ambiguous = false;
         ++s_vtypeCount;
      }
   }
}

const VtypeEntry* find_vtype(uint32_t vptr)
{
   for (uint32_t k = 0; k < s_vtypeCount; ++k)
      if (s_vtype[k].vptr == vptr) return s_vtype[k].ambiguous ? nullptr : &s_vtype[k];
   return nullptr;
}

// Pass 2: resolve parents and bucket, entirely inside our own arrays.  No live
// pointer is dereferenced here, so nothing in this pass can fault or hang.
void bucket(const Registry& R, WalkResult* W, uint32_t n)
{
   learn_vtypes(R, n);

   for (uint32_t i = 0; i < kIndexSlots; ++i) s_idx[i] = -1;
   for (uint32_t i = 0; i < n; ++i) {
      uint32_t slot = (s_rec[i].obj >> 2) & (kIndexSlots - 1);
      for (uint32_t probe = 0; probe < kIndexSlots; ++probe) {   // BOUNDED
         if (s_idx[slot] < 0) { s_idx[slot] = (int32_t)i; break; }
         slot = (slot + 1) & (kIndexSlots - 1);
      }
   }

   for (uint32_t i = 0; i < n; ++i) {
      if (s_rec[i].constructing) ++W->constructing;

      uint32_t cur = i, hops = 0;
      bool resolved = false, broken = false;
      while (hops++ < kMaxChain) {
         if (s_rec[cur].mParent == 0) { resolved = true; break; }
         int32_t found = -1;
         uint32_t slot = (s_rec[cur].mParent >> 2) & (kIndexSlots - 1);
         for (uint32_t probe = 0; probe < kIndexSlots; ++probe) {  // BOUNDED
            const int32_t k = s_idx[slot];
            if (k < 0) break;
            if (s_rec[k].obj == s_rec[cur].mParent) { found = k; break; }
            slot = (slot + 1) & (kIndexSlots - 1);
         }
         if (found < 0) { ++W->orphan; broken = true; break; }
         cur = (uint32_t)found;
      }
      if (broken) continue;
      if (!resolved) { ++W->cycle; continue; }

      s_rec[i].rootId = s_rec[cur].mId;
      const Root* root = find_root(R, s_rec[i].rootId);
      if (root == nullptr) {
         // The chain gave a root we cannot name. Before calling it unknown, ask
         // what C++ class the object actually is -- that answers the question
         // the parent chain could not.
         const VtypeEntry* const vt = find_vtype(s_rec[i].vptr);
         if (vt != nullptr) {
            ++W->byType;
            if (R.families) ++W->fam[vt->family];
            continue;
         }
         ++W->unknownRoot;
         bool seen = false;
         for (uint32_t k = 0; k < W->unkDistinct; ++k)
            if (W->unkId[k] == s_rec[i].rootId) { ++W->unkCount[k]; seen = true; break; }
         if (!seen && W->unkDistinct < 8) {
            W->unkId[W->unkDistinct]    = s_rec[i].rootId;
            W->unkCount[W->unkDistinct] = 1;
            W->unkRec[W->unkDistinct]   = cur;   // the root's own record
            ++W->unkDistinct;
         }
         continue;
      }
      if (R.families) ++W->fam[root->family];
   }
}

// Pass 3: names.  Read bounded, require printable, and check the result against
// mId -- EntityClass::Read hashes the very buffer it then copies, so a match is
// proof rather than a guess.
void check_names(const Registry& R, WalkResult* W, uint32_t n)
{
   if (R.nameOff == 0) return;
   for (uint32_t i = 0; i < n; ++i) {
      if (s_rec[i].constructing) continue;   // no name written yet
      char name[kNameMax + 1];
      __try {
         const char* const src =
            reinterpret_cast<const char*>((uintptr_t)s_rec[i].obj + R.nameOff);
         uint32_t j = 0;
         for (; j < kNameMax; ++j) { const char c = src[j]; if (c == 0) break; name[j] = c; }
         name[j] = 0;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         ++W->mismatch;
         continue;
      }
      if (name[0] == 0) {
         // A built-in root legitimately has no name -- the one-argument ctor
         // never goes through Read, so nothing ever writes the buffer. Only a
         // NON-root with an empty name is worth reporting.
         if (find_root(R, s_rec[i].mId) == nullptr) ++W->unnamed;
         continue;
      }
      bool printable = true;
      for (uint32_t j = 0; name[j] != 0; ++j)
         if (name[j] < 0x20 || name[j] > 0x7E) { printable = false; break; }
      if (!printable) { ++W->mismatch; continue; }
      // strlcpy caps at 31 chars + NUL, so a 31-char name is legitimately
      // truncated and will not hash back.  That is its own line, not corruption.
      uint32_t len = 0;
      while (name[len] != 0) ++len;
      if (pbl_hash(name, kNameMax) != s_rec[i].mId) {
         if (len >= kNameMax - 1) ++W->truncated; else ++W->mismatch;
      }
   }
}

// Read the name of each unknown root. Must run while s_rec still holds THIS
// registry's records -- the next registry walk overwrites it.
void name_unknown_roots(const Registry& R, WalkResult* W)
{
   for (uint32_t k = 0; k < W->unkDistinct; ++k) {
      W->unkName[k][0] = 0;
      if (R.nameOff == 0) continue;
      const uint32_t rec = W->unkRec[k];
      __try {
         const char* const src =
            reinterpret_cast<const char*>((uintptr_t)s_rec[rec].obj + R.nameOff);
         uint32_t j = 0;
         for (; j < kNameMax; ++j) {
            const char c = src[j];
            if (c == 0) break;
            W->unkName[k][j] = (c >= 0x20 && c <= 0x7E) ? c : '?';
         }
         W->unkName[k][j] = 0;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         W->unkName[k][0] = 0;
      }
   }
}

bool walk_registry(const Registry& R, WalkResult* W)
{
   *W = WalkResult{};
   if (R.head == 0) return false;
   uint32_t n = 0;
   const bool clean = collect(R, W, &n);
   W->scanned = n;
   bucket(R, W, n);
   name_unknown_roots(R, W);
   check_names(R, W, n);
   W->ok = clean;
   return true;
}

// Copy a game-owned string into our own bounded buffer. The source pointer
// comes from live engine memory, so it may be null, unterminated, or pointing
// at something that is not text at all -- none of which vfprintf survives.
// Callers invoke this INSIDE their __try so a fault is caught here, not later.
void copy_game_str(char* dst, size_t cap, const char* src)
{
   if (!src || cap == 0) { _snprintf_s(dst, cap, _TRUNCATE, "(unnamed)"); return; }
   size_t i = 0;
   for (; i + 1 < cap; ++i) {
      const char c = src[i];
      if (c == 0) break;
      dst[i] = (c >= 0x20 && c <= 0x7E) ? c : '?';
   }
   dst[i] = 0;
   if (i == 0) _snprintf_s(dst, cap, _TRUNCATE, "(unnamed)");
}

// Bytes, rendered so a human can compare them at a glance.
void fmt_bytes(char* out, size_t cap, uint32_t bytes)
{
   if (bytes >= 1024u * 1024u)
      _snprintf_s(out, cap, _TRUNCATE, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
   else if (bytes >= 1024u)
      _snprintf_s(out, cap, _TRUNCATE, "%.1f KB", (double)bytes / 1024.0);
   else
      _snprintf_s(out, cap, _TRUNCATE, "%u B", bytes);
}

// Walk one heap's free list. Returns false if the list looked torn, in which
// case the caller reports nothing rather than a partial sum.
bool walk_free_list(const uint8_t* heap, uint32_t* outFree, uint32_t* outLargest)
{
   uint32_t total = 0, largest = 0, n = 0;
   __try {
      const uint8_t* node = *reinterpret_cast<const uint8_t* const*>(heap + kHeapFreeHead);
      while (node) {
         const uint32_t sz = *reinterpret_cast<const uint32_t*>(node + kTagSize);
         total += sz;
         if (sz > largest) largest = sz;
         if (++n > kMaxFreeNodes) return false;
         node = *reinterpret_cast<const uint8_t* const*>(node + kTagNext);
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
   *outFree = total;
   *outLargest = largest;
   return true;
}

void report_heaps()
{
   if (g_addr->red_heap_table_ptr == 0 || g_addr->red_heap_count == 0) return;

   uint32_t count = 0;
   const uint8_t* table = nullptr;
   __try {
      count = *reinterpret_cast<const uint32_t*>(resolve(s_exeBase, g_addr->red_heap_count));
      table = *reinterpret_cast<const uint8_t* const*>(resolve(s_exeBase, g_addr->red_heap_table_ptr));
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      return;
   }
   if (!table || count == 0 || count > kMaxHeaps) return;

   census_log("[Census] --- memory ---");
   for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* const heap = table + (uintptr_t)i * kHeapStride;

      uint32_t size = 0;
      char name[40];
      __try {
         size = *reinterpret_cast<const uint32_t*>(heap + kHeapSize);
         // The name is a GAME-OWNED pointer. Copy it here, inside the __try --
         // handing it to vfprintf outside one is an unbounded read of live
         // engine memory with no SEH around it.
         copy_game_str(name, sizeof(name),
                       *reinterpret_cast<const char* const*>(heap + kHeapName));
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         continue;
      }
      if (size == 0) continue;

      uint32_t freeBytes = 0, largest = 0;
      if (!walk_free_list(heap, &freeBytes, &largest)) {
         census_log("[Census]   %-14s (free list unreadable this tick)", name);
         continue;
      }

      const uint32_t used = (freeBytes <= size) ? size - freeBytes : 0;
      char usedS[32], sizeS[32], largeS[32];
      fmt_bytes(usedS, sizeof(usedS), used);
      fmt_bytes(sizeS, sizeof(sizeS), size);
      fmt_bytes(largeS, sizeof(largeS), largest);

      // Largest-contiguous-free is the number stock `mem` never prints, and the
      // one that actually predicts an allocation failure: a heap can be 40%
      // free and still refuse a large block through fragmentation.
      char delta[48];
      delta[0] = 0;
      if (s_havePrev && i < kMaxHeaps) {
         const long d = (long)used - (long)s_prevUsed[i];
         if (d != 0) {
            char dS[32];
            fmt_bytes(dS, sizeof(dS), (uint32_t)(d < 0 ? -d : d));
            _snprintf_s(delta, sizeof(delta), _TRUNCATE, "  (%c%s)", d < 0 ? '-' : '+', dS);
         }
      }
      if (i < kMaxHeaps) s_prevUsed[i] = used;

      census_log("[Census]   %-14s %9s / %-9s  largest free %9s%s%s",
                 name, usedS, sizeS, largeS,
                 delta, pressure(used, size));
   }
   s_havePrev = true;
}

DWORD WINAPI census_thread(LPVOID)
{
   for (;;) {
      const DWORD ms = (DWORD)(g_contentCensusInterval > 0 ? g_contentCensusInterval : 30) * 1000u;
      if (WaitForSingleObject(s_stop, ms) == WAIT_OBJECT_0) return 0;
      content_census_report();
   }
}

// ---------------------------------------------------------------------------
// Reporting.
//
// A count that could not be established prints "?" and the reason. It never
// falls back to the engine's own counter: the whole point of scanning is that
// the counter is the thing being checked, so substituting it when the scan
// fails would quietly report the number we set out to verify.
// ---------------------------------------------------------------------------

// Last entity count we dumped names for, so the dump fires once per level load
// rather than every tick.
uint32_t s_lastDumped = 0xFFFFFFFFu;

void count_str(char* out, size_t cap, bool have, const WalkResult& W)
{
   if (!have || (W.err[0] != 0 && W.scanned == 0)) _snprintf_s(out, cap, _TRUNCATE, "?");
   else                                            _snprintf_s(out, cap, _TRUNCATE, "%u", W.scanned);
}

void dump_class_names(const Registry& R, uint32_t n)
{
   census_log("[Census]   --- %s classes, one line each ---", R.label);
   for (uint32_t i = 0; i < n; ++i) {
      const Root* const root = find_root(R, s_rec[i].rootId);
      char name[kNameMax + 1];
      name[0] = 0;
      if (R.nameOff != 0 && !s_rec[i].constructing) {
         __try {
            const char* const src =
               reinterpret_cast<const char*>((uintptr_t)s_rec[i].obj + R.nameOff);
            uint32_t j = 0;
            for (; j < kNameMax; ++j) {
               const char c = src[j];
               if (c == 0) break;
               name[j] = (c >= 0x20 && c <= 0x7E) ? c : '?';
            }
            name[j] = 0;
         } __except (EXCEPTION_EXECUTE_HANDLER) {
            name[0] = 0;
         }
      }
      census_log("[Census]     %-24s %-22s id=0x%08X",
                 name[0] != 0 ? name : "(no name on this build)",
                 root != nullptr ? root->name : "(unknown root)",
                 s_rec[i].mId);
   }
}

void report_registries()
{
   const Registry kRegs[] = {
      {"entity",    g_addr->entity_class_head,    g_addr->entity_class_name_off,
       g_addr->factory_vft_entity,    kEntityRoots,
       (uint32_t)(sizeof(kEntityRoots) / sizeof(kEntityRoots[0])),       true},
      {"weapon",    g_addr->weapon_class_head,    g_addr->weapon_class_name_off,
       g_addr->factory_vft_weapon,    kWeaponRoots,
       (uint32_t)(sizeof(kWeaponRoots) / sizeof(kWeaponRoots[0])),       false},
      {"ordnance",  g_addr->ordnance_class_head,  g_addr->ordnance_class_name_off,
       g_addr->factory_vft_ordnance,  kOrdnanceRoots,
       (uint32_t)(sizeof(kOrdnanceRoots) / sizeof(kOrdnanceRoots[0])),   false},
      {"explosion", g_addr->explosion_class_head, g_addr->explosion_class_name_off,
       g_addr->factory_vft_explosion, kExplosionRoots,
       (uint32_t)(sizeof(kExplosionRoots) / sizeof(kExplosionRoots[0])), false},
   };
   const uint32_t kRegCount = (uint32_t)(sizeof(kRegs) / sizeof(kRegs[0]));

   WalkResult W[4];
   bool       have[4];
   for (uint32_t i = 0; i < kRegCount; ++i)
      have[i] = walk_registry(kRegs[i], &W[i]);

   if (have[0]) {
      if (W[0].err[0] != 0 && W[0].scanned == 0) {
         census_log("[Census]   entity classes  ? (%s)", W[0].err);
      } else {
         census_log("[Census]   entity classes  %4u   sol %3u  veh %3u  tur %2u"
                    "  bld %3u  cp %2u  pup %3u  scn %3u",
                    W[0].scanned, W[0].fam[FAM_SOLDIER], W[0].fam[FAM_VEHICLE],
                    W[0].fam[FAM_TURRET], W[0].fam[FAM_BUILDING],
                    W[0].fam[FAM_CMDPOST], W[0].fam[FAM_PICKUP], W[0].fam[FAM_SCENERY]);
      }
   }

   if (have[1] || have[2] || have[3]) {
      char w[16], o[16], e[16];
      count_str(w, sizeof(w), have[1], W[1]);
      count_str(o, sizeof(o), have[2], W[2]);
      count_str(e, sizeof(e), have[3], W[3]);
      census_log("[Census]   weapon classes  %4s   ordnance %4s   explosion %4s", w, o, e);
   }

   // Anomalies, subordinate to the counts and printed only when non-zero, so a
   // healthy report stays two lines.
   for (uint32_t i = 0; i < kRegCount; ++i) {
      if (!have[i]) continue;
      const WalkResult& R = W[i];
      const char* const L = kRegs[i].label;

      if (R.err[0] != 0 && R.scanned != 0)
         census_log("[Census]     %s: %s", L, R.err);

      if (R.engine < 0) {
         census_log("[Census]     %s: engine counter is NEGATIVE (%d)"
                    " -- bad address, not a race", L, R.engine);
      } else if (R.err[0] == 0 && (uint32_t)R.engine != R.scanned) {
         const int d = (int)R.scanned - R.engine;
         // Both mutation paths update _iCount BEFORE the forward chain, so the
         // scan can legitimately lag the counter but can never lead it.
         if (d < 0)
            census_log("[Census]     %s: engine %d vs scan %u (%+d"
                       " -- mid-mutation, expected at a state change)", L, R.engine, R.scanned, d);
         else
            census_log("[Census]     %s: engine %d vs scan %u (%+d"
                       " -- IMPOSSIBLE, list or counter corrupt)", L, R.engine, R.scanned, d);
      }

      if (R.constructing || R.unlinking || R.orphan || R.cycle || R.unknownRoot)
         census_log("[Census]     %s: %u constructing, %u unlinking, %u orphan,"
                    " %u cycle, %u unknown-root",
                    L, R.constructing, R.unlinking, R.orphan, R.cycle, R.unknownRoot);

      // Say how the breakdown was reached. A class whose CreateClass never
      // recorded a parent can only be attributed by its C++ type, and the
      // reader deserves to know which figures came from which route.
      if (R.byType != 0)
         census_log("[Census]     %s: %u of those attributed by type"
                    " (their CreateClass records no parent)", L, R.byType);

      if (R.unnamed || R.truncated || R.mismatch)
         census_log("[Census]     %s: %u unnamed, %u names longer than 31 chars,"
                    " %u name/id MISMATCH", L, R.unnamed, R.truncated, R.mismatch);

      // Name the unknown roots rather than only counting them. Every root the
      // engine ships is in our table, so one that is not means a class was
      // created by a path we have not accounted for -- and the id, plus its
      // name where the build kept one, is what identifies it.
      for (uint32_t k = 0; k < R.unkDistinct; ++k)
         census_log("[Census]     %s: unknown root id=0x%08X %-24s %u class(es) below it",
                    L, R.unkId[k],
                    R.unkName[k][0] != 0 ? R.unkName[k] : "(no name)", R.unkCount[k]);
      if (R.unknownRoot != 0 && R.unkDistinct == 8)
         census_log("[Census]     %s: (more than 8 distinct unknown roots -- listing capped)", L);
   }

   // Optional per-class listing. Fires when the entity count has changed since
   // the last dump, i.e. once per level load rather than every tick.
   if (g_contentCensusNames && have[0] && W[0].scanned != 0 &&
       W[0].scanned != s_lastDumped) {
      // s_rec was overwritten by the three registry walks that followed the
      // entity one, so re-walk to repopulate it rather than caching 4096
      // records for a listing that fires once per level.
      WalkResult again;
      if (walk_registry(kRegs[0], &again) && again.scanned != 0) {
         dump_class_names(kRegs[0], again.scanned);
         s_lastDumped = again.scanned;
      }
   }
}

} // namespace

void content_census_report()
{
   if (s_exeBase == 0) return;

   report_heaps();

   census_log("[Census] --- content ---");

   uint32_t scan = 0, counter = 0;
   if (scan_effect_classes(&scan, &counter)) {
      census_log("[Census]   effect classes   %3u / %u%s",
                 scan, kEffectClassSlots, pressure(scan, kEffectClassSlots));
      // The engine's own counter is reported next to the scan rather than
      // instead of it. They should always agree; if they ever do not, the
      // discrepancy IS the finding, so say so instead of quietly preferring one.
      if (counter != scan) {
         census_log("[Census]   (engine counter says %u, scan says %u -- they disagree,"
                    " trust the scan)", counter, scan);
      }
      if (scan >= kEffectClassSlots) {
         census_log("[Census]   WARNING: the effect table is FULL. Loading one more distinct"
                    " effect will hang during level load, and any lookup of an effect name"
                    " that is not registered will hang the game now.");
      }
   }

   report_registries();

   // Path counts. Both are read as bare dwords and neither list is walked --
   // GamePathFactory in particular links a STACK LOCAL into its global list
   // once per `path` record, so dereferencing a node there can mean reading
   // another thread's live stack. The count alone is safe and is all we want.
   if (g_addr->branch_region_count != 0) {
      uint32_t regions = 0, factories = 0;
      bool haveFactories = false;
      __try {
         regions = *reinterpret_cast<const uint32_t*>(
            resolve(s_exeBase, g_addr->branch_region_count));
         if (g_addr->path_factory_count != 0) {
            factories = *reinterpret_cast<const uint32_t*>(
               resolve(s_exeBase, g_addr->path_factory_count));
            haveFactories = true;
         }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         regions = 0;
         haveFactories = false;
      }
      if (haveFactories)
         census_log("[Census]   path regions    %4u   path factories %u", regions, factories);
      else
         census_log("[Census]   path regions    %4u", regions);
      // 0 before the first load and after teardown, 3 while a state is live,
      // 4 transiently while path chunks are parsing. Anything else is news.
      if (haveFactories && factories != 0 && factories != 3 && factories != 4)
         census_log("[Census]     path factories %u -- outside expected {0,3,4}", factories);
   }

   census_log("[Census] --- end ---");
}

void content_census_install(uintptr_t exe_base)
{
   s_exeBase = exe_base;
   if (g_contentCensusInterval <= 0) return;

   s_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
   if (!s_stop) return;
   // Below normal: this must never compete with the simulation for a core.
   s_thread = CreateThread(nullptr, 0, census_thread, nullptr, 0, nullptr);
   if (s_thread) SetThreadPriority(s_thread, THREAD_PRIORITY_BELOW_NORMAL);
}

void content_census_uninstall()
{
   if (s_stop) SetEvent(s_stop);
   if (s_thread) {
      WaitForSingleObject(s_thread, 2000);
      CloseHandle(s_thread);
      s_thread = nullptr;
   }
   if (s_stop) { CloseHandle(s_stop); s_stop = nullptr; }
}
