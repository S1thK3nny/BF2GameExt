#include "pch.h"
#include "soldier_ragdoll.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"

#include <cmath>
#include <cstring>
#include <detours.h>

// =============================================================================
// Soldier ragdoll - Verlet bone simulation for dead soldiers.
//
// Hook point
// ----------
// SoldierAnimator::ApplyProceduralAnimationAndBuildWorldMatrices
// (modtools 0x00579f10, __thiscall(SoldierAnimator*, float dt), RET 4) is the
// single place the soldier's per-joint world matrices are produced.  Its tail
// is literally
//
//     00579f57: PUSH 0xcf6830              ; &gMatrixIdentity
//     00579f5c: LEA  ECX,[EBX + 0xd0]      ; &this->mZephyrSkeleton
//     00579f62: CALL 0x0082c390            ; ZephyrSkeleton<32>::BuildWorldMatrices
//
// so we let the original run and then overwrite what it produced.  This has to
// happen every frame, not once: the animator rebuilds the pose from the death
// clip on each call and would otherwise stomp the simulation straight back out
// (same shape as the droideka death-anim bug, where Update re-issues its input
// every frame).
//
// Note the root matrix is built against gMatrixIdentity, so m_pWorldMatrices are
// in MODEL space, not world space.  That is what makes this cheap: no terrain
// query is needed anywhere.
//
// The sim itself runs in WORLD space so the body has inertia of its own, which
// model space cannot express: there the corpse entity's motion is invisible, so
// a body could only ever be dragged rigidly along by it.  seed() moves the pose
// out to world space and takes its starting velocity from EntitySoldier::
// mVelocity, where DirectionKill has just deposited the death impulse; the
// entity is not consulted again after that, and write_back() brings the result
// back to model space.  Contact stays a single frozen plane, captured in world
// at seed, so no terrain query creeps back in - see seed() for why it is frozen
// rather than tracked, and what that costs on a slope.
//
// Data layout (build-invariant: these are PODs read straight out of the .zaabin
// skeleton, and ZephyrSkeleton<32> is 0x810 bytes on debug and release alike -
// cross-checked against the modtools EntityFlyer struct dump in
// docs/ghidra-fixup-plan.md, which lists mZephyrSkeleton as 0x810).
//
//   SoldierAnimator      +0x50  EntitySoldier* mOwner  (struct_base, NOT entity)
//                        +0xD0  ZephyrSkeleton<32> mZephyrSkeleton
//   ZephyrSkeleton<32>   +0x00  ZephyrSkeletonShared* m_pShared
//                        +0x04  ZephyrPoseStatic<32>* m_pPoseFinal
//                        +0x10  PblMatrix m_pWorldMatrices[32]
//   ZephyrSkeletonShared +0x00  ZephyrJointShared* m_pJoints
//                        +0x04  uint m_uNumJoints
//   ZephyrJointShared    112 bytes; +0x60 m_iParent, +0x61 m_iChild (signed,
//                        -1 = none).  Confirmed live: BuildWorldMatrices reads
//                        *(char*)(*m_pShared + 0x61) to start its child walk.
//
// Death test
// ----------
// EntitySoldier derives Damageable at struct_base+0x140 (data +0x144); its
// 188-byte block ends with mHealthFlags at +184, i.e. struct_base+0x1FC, packed
// mHealthType:3 / mIsAlive:1 / mVanishing:1 / mIsShielded:1.  So alive =
// bit 3.  Independently confirmed in the Phantom PDB build, where
// EntitySoldier::DirectionKill reads exactly `*(uint*)(x + 0x1fc) >> 3 & 1`.
//
// The matrices this writes are consumed by RedSkeleton::GetSkinMatrices for the
// skinned mesh, so nothing else needs patching to make it visible.
// =============================================================================

bool g_soldierRagdollEnabled = false;
bool g_soldierRagdollDebug   = false;

// One-shot guard for the seed dump, cleared per level by soldier_ragdoll_reset().
static bool g_loggedSeed = false;

// ---------------------------------------------------------------------------
// Offsets
// ---------------------------------------------------------------------------

static constexpr int kSAOwner          = 0x50;  // SoldierAnimator::mOwner (struct_base)
static constexpr int kSAZephyrSkeleton = 0xD0;  // SoldierAnimator::mZephyrSkeleton

static constexpr int kZSShared         = 0x00;  // ZephyrSkeleton::m_pShared
static constexpr int kZSWorldMatrices  = 0x10;  // ZephyrSkeleton::m_pWorldMatrices[32]

static constexpr int kZSSJoints        = 0x00;  // ZephyrSkeletonShared::m_pJoints
static constexpr int kZSSNumJoints     = 0x04;  // ZephyrSkeletonShared::m_uNumJoints

static constexpr int kJointStride      = 112;   // sizeof(ZephyrJointShared)
static constexpr int kJointParent      = 0x60;  // char m_iParent
static constexpr int kJointChild       = 0x61;  // char m_iChild

static constexpr int kHealthFlags      = 0x1FC; // EntitySoldier struct_base + mHealthFlags
static constexpr int kIsAliveBit       = 3;

// EntitySoldier::mMatrix - the corpse's world transform (PblMatrix: right, up,
// forward, trans).  Sits between the RedSceneObject block and mModel at +0x130.
static constexpr int kEntityMatrix     = 0xF0;

// EntitySoldier::mVelocity - the corpse's world-space velocity, and the field
// the death impulse lands in.  This is what gives the body its momentum; without
// it the ragdoll is born at rest and an explosion cannot throw it.
//
// Confirmed by DISASSEMBLY, not by the struct dump.  mt EntitySoldier::
// DirectionKill (0x0054c020) normalises (soldierPos - blastPos) and hands it to
// 0x005458b0, which writes three consecutive floats at this+0x4EC:
//   - normal path:        += dir * 5.0
//   - state 0x11 / 0x12:   = normalize(dir, biased up by 0.4 + rand*0.3) * 9.0
//     followed by SetState(9)
//
// The dump puts EntitySoldier_data at 0x424, which would make +0x4EC the middle
// of mPitchOffset / mPitchVelocity / mKickScale - three unrelated scalars, not an
// impulse target.  The real base is 0x434, and two fields agree on it
// independently: mVelocity at +184 lands on 0x4EC (the write above), and mState
// at +800 lands on 0x754, which is exactly the field 0x005458b0 tests against
// SoldierState 0x11/0x12.  This is the same 0x10 skew the dump has elsewhere -
// see the warning in [[soldier-ragdoll-feature]] about sub-struct bases.
//
// The earlier attempt at this used struct+0x334, taking Controllable_data's base
// (0x27C) with EntitySoldier_data's field index (+184) - two different
// sub-structs. That is why corpses launched at clamp speed from a standing kill.
static constexpr int kEntityVelocity   = 0x4EC;

// Sanity ceiling on the seeded speed.  A real death impulse is 5-9 m/s, and a
// sprinting soldier adds a few more; anything past this means we are reading a
// field that is not mVelocity, and the body should collapse in place rather than
// be flung off the map.  Logged when it trips so a bad offset stays visible
// instead of being silently absorbed.
static constexpr float kMaxSeedSpeed   = 30.0f;

// PblMatrix is D3DX row-major with row-vector convention: rows at +0x00/+0x10/
// +0x20 and the translation in row 3 at +0x30 (matches Chunk::mMatrix, whose
// position is read at field_0x30).
static constexpr int kMatrixFloats     = 16;
static constexpr int kMatTransX        = 12;    // float index of row3.x

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------

static constexpr int   kMaxJoints    = 32;     // ZephyrSkeleton<32>
static constexpr int   kMaxRagdolls  = 24;     // simultaneous simulated corpses

static constexpr float kGravity      = -9.81f; // model units are metres
static constexpr float kFixedStep    = 1.0f / 60.0f;
static constexpr int   kMaxSubsteps  = 4;      // clamps a long frame / hitch
static constexpr int   kIterations   = 10;     // constraint relaxation passes
static constexpr float kDamping      = 0.985f; // per-step velocity retention
static constexpr float kGroundFric   = 0.55f;  // tangential velocity killed on contact
static constexpr float kSettleTime   = 10.0f;  // seconds before the sim is parked

// Constraint network.  Parent-child links alone are a 1D string: they fix bone
// LENGTHS and nothing else, so the body folds up and heaps into a pile, and
// joints are free to converge in x/z because nothing holds them apart.  Instead
// every pair of joints within kConstraintDepth hops of each other gets a
// distance constraint measured off the seed pose, which turns the skeleton into
// a truss.  A link to a joint 2 hops away is exactly an angle spring on the
// joint between them, so this supplies joint limits as well as rigidity - which
// matters most across the shoulder girdle, where j07/j12/j18 are coincident and
// the torso would otherwise have no width at all.
//
// Stiffness falls off with hop distance: bones are rigid, the rest are springs
// pulling back towards the pose the soldier died in.
static constexpr int   kConstraintDepth = 3;
static constexpr int   kMaxConstraints  = 256;
static constexpr int   kMaxNeighbors    = 12;
static constexpr float kStiffByDepth[kConstraintDepth + 1] = {
   0.0f,   // unused
   1.0f,   // bone: does not stretch
   0.55f,  // angle spring across one joint
   0.30f,  // broad shape retention
};

// Joint thickness.  Contacts stop at groundY + radius rather than exactly on the
// plane, so the corpse has volume instead of every joint converging on one
// height (which is the other half of the flattening).
static constexpr float kJointRadius  = 0.09f;

// Joint classification (see seed()).  kCoincidentEps is "sits on its parent";
// kOutlierFactor is how many median bone lengths a LEAF may sit from its parent
// before it is treated as an aim/hardpoint node rather than a bone.  On a stock
// human skeleton the real bones run 0.13-0.48 against a median of ~0.24, and the
// three aim nodes sit at 1.79-1.84, so the margin either side is wide.
static constexpr float kCoincidentEps = 0.01f;
static constexpr float kOutlierFactor = 2.5f;

// Below this many simulated joints there is no body left to simulate and we
// leave the animated pose alone.
static constexpr int   kMinSimJoints  = 4;

// ---------------------------------------------------------------------------
// Small vector helpers (kept local - no D3DX dependency)
// ---------------------------------------------------------------------------

struct Vec3 {
   float x, y, z;
};

static inline Vec3 v_sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline float v_dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline Vec3 v_cross(const Vec3& a, const Vec3& b)
{
   return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static inline float v_len(const Vec3& a) { return std::sqrt(v_dot(a, a)); }

// PblMatrix is row-major with row vectors, so a point transforms as p * M with
// the translation living in row 3.
static inline Vec3 xform_point(const float* M, const Vec3& p)
{
   return {p.x * M[0] + p.y * M[4] + p.z * M[8]  + M[12],
           p.x * M[1] + p.y * M[5] + p.z * M[9]  + M[13],
           p.x * M[2] + p.y * M[6] + p.z * M[10] + M[14]};
}

// Inverse of the above for a rigid (optionally scaled) transform: subtract the
// translation, then project onto each basis row and divide by its squared
// length, which undoes any scale baked into the rows as well as the rotation.
static inline Vec3 xform_point_inv(const float* M, const Vec3& p)
{
   const float d[3] = {p.x - M[12], p.y - M[13], p.z - M[14]};
   Vec3 out{};
   float* o = &out.x;
   for (int r = 0; r < 3; r++) {
      const float* row = M + r * 4;
      const float sq = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
      o[r] = (sq > 1e-12f)
                ? (d[0] * row[0] + d[1] * row[1] + d[2] * row[2]) / sq
                : 0.0f;
   }
   return out;
}

// A soldier's world transform is upright yaw, so a sane matrix has a near-
// vertical up row and a unit-ish scale.  Anything else means we are reading a
// torn-down or not-yet-initialised entity and should stay in model space.
static inline bool matrix_is_sane(const float* M)
{
   const float sq = M[4] * M[4] + M[5] * M[5] + M[6] * M[6];
   return sq > 0.25f && sq < 4.0f;
}

// ---------------------------------------------------------------------------
// Per-corpse state
// ---------------------------------------------------------------------------

struct Constraint {
   signed char a, b;
   float       len;
   float       stiff;
};

struct RagdollState {
   void*       animator;              // key; nullptr = free slot
   uint32_t    touchTick;             // for LRU eviction
   int         numJoints;
   int         simCount;              // joints that survived classification
   bool        seeded;
   float       accum;                 // fixed-step accumulator
   float       age;                   // seconds since death
   float       groundY;               // model-space contact plane, measured at seed
   bool        sim[kMaxJoints];       // false = scaffolding / non-physical node
   signed char parent[kMaxJoints];    // raw m_iParent, as authored
   signed char cparent[kMaxJoints];   // constraint parent after pass-through, -1 = free root
   signed char dirChild[kMaxJoints];  // joint this one aims at for its swing, -1 = none
   signed char follow[kMaxJoints];    // dropped node copies this joint's matrix, -1 = none
   Vec3        pos[kMaxJoints];
   Vec3        prev[kMaxJoints];
   float       restLen[kMaxJoints];   // distance to cparent, measured off the live pose
   Constraint  cons[kMaxConstraints];
   int         consCount;
};

static RagdollState g_ragdolls[kMaxRagdolls] = {};
static uint32_t     g_tick                   = 0;

static RagdollState* find_state(void* animator)
{
   for (int i = 0; i < kMaxRagdolls; i++)
      if (g_ragdolls[i].animator == animator) return &g_ragdolls[i];
   return nullptr;
}

static void release_state(void* animator)
{
   RagdollState* s = find_state(animator);
   if (s) s->animator = nullptr;
}

// Takes a free slot, else evicts the least recently touched one.  Corpses
// despawn on their own, so eviction only bites when more than kMaxRagdolls are
// visible at once - the oldest is the one nearest to vanishing anyway.
static RagdollState* acquire_state(void* animator)
{
   RagdollState* victim = nullptr;
   for (int i = 0; i < kMaxRagdolls; i++) {
      if (g_ragdolls[i].animator == nullptr) { victim = &g_ragdolls[i]; break; }
      if (!victim || g_ragdolls[i].touchTick < victim->touchTick) victim = &g_ragdolls[i];
   }
   if (!victim) return nullptr;

   std::memset(victim, 0, sizeof(*victim));
   victim->animator = animator;
   return victim;
}

// ---------------------------------------------------------------------------
// Seed - snapshot the animated pose as the ragdoll's starting configuration
// ---------------------------------------------------------------------------
// Rest lengths come from the LIVE pose rather than ZephyrJointShared's bind
// transform: measuring what the skeleton actually is right now costs one
// subtract per bone and is automatically correct for per-joint scale and for
// whatever non-standard skeleton a mod ships.

static void seed(RagdollState* s, const float* world, const uint8_t* joints, int n,
                 const float* entityMat, const Vec3& vel)
{
   s->numJoints = n;

   for (int i = 0; i < n; i++) {
      const float* m = world + i * kMatrixFloats;
      s->pos[i]  = {m[kMatTransX], m[kMatTransX + 1], m[kMatTransX + 2]};
      s->prev[i] = s->pos[i]; // start at rest; gravity does the rest
      s->parent[i] = (signed char)joints[i * kJointStride + kJointParent];
   }

   // ---- Classify joints -----------------------------------------------------
   // Not every entry in a Zephyr skeleton is a bone.  Simulating the ones that
   // are not is what wrecks the pose, and no amount of solver tuning fixes it.
   //
   //  a) COINCIDENT nodes sit exactly on their parent - BuildWorldMatrices
   //     copies the parent's matrix wholesale for these (the kuiDummyRootHash
   //     branch).  They are branch/pass-through markers, not bones.
   //  b) The ROOT sits at the model origin.  The link from it up to the pelvis
   //     is the rig's root offset, not anatomy: constraining it pins the pelvis
   //     to a fixed distance from a point on the ground, so the corpse can never
   //     lie down.  Anything hanging directly off the root becomes a free root.
   //  c) OUTLIER LEAVES are aim / hardpoint nodes positioned by the procedural
   //     pass rather than the hierarchy - on a human they show up as leaves
   //     ~1.8m from their parent against a 0.48m longest real bone.  Judged
   //     against the median bone so this holds for non-human skeletons too, and
   //     restricted to leaves so it can never sever the body.

   bool coincident[kMaxJoints] = {};
   for (int i = 0; i < n; i++) {
      const int p = s->parent[i];
      coincident[i] = (p >= 0 && p < n) &&
                      v_len(v_sub(s->pos[i], s->pos[p])) < kCoincidentEps;
   }

   // Median of the non-degenerate parent distances, for the outlier test.
   float lens[kMaxJoints];
   int   lenCount = 0;
   for (int i = 0; i < n; i++) {
      const int p = s->parent[i];
      if (p < 0 || p >= n) continue;
      const float d = v_len(v_sub(s->pos[i], s->pos[p]));
      if (d >= kCoincidentEps) lens[lenCount++] = d;
   }
   for (int a = 1; a < lenCount; a++) { // insertion sort, <= 32 elements
      const float v = lens[a];
      int b = a - 1;
      while (b >= 0 && lens[b] > v) { lens[b + 1] = lens[b]; b--; }
      lens[b + 1] = v;
   }
   const float medianLen  = lenCount ? lens[lenCount / 2] : 0.0f;
   const float outlierLen = medianLen * kOutlierFactor;

   // Pass through coincident scaffolding to the first real ancestor.  Kept in
   // its own array because a joint's effective parent is very often a HIGHER
   // index (the pelvis is j25 while its children are j04/j05/j11/j21/j22), so
   // nothing here may read a per-joint result that the same loop is still
   // producing - see the two-pass split below.
   signed char eff[kMaxJoints];
   for (int i = 0; i < n; i++) {
      int p = s->parent[i];
      int guard = 0;
      while (p >= 0 && p < n && coincident[p] && guard++ < kMaxJoints)
         p = s->parent[p];
      eff[i] = (signed char)((p >= 0 && p < n) ? p : -1);
   }

   // Pass 1 - classify every joint.  Depends only on geometry and the raw
   // hierarchy, never on another joint's classification.
   for (int i = 0; i < n; i++) {
      const bool isRoot = (s->parent[i] < 0);
      const bool isLeaf = ((signed char)joints[i * kJointStride + kJointChild]) < 0;
      const int  p      = eff[i];
      const float d     = (p >= 0) ? v_len(v_sub(s->pos[i], s->pos[p])) : 0.0f;

      s->sim[i] = !isRoot && !coincident[i] &&
                  !(isLeaf && outlierLen > 0.0f && d > outlierLen);
   }

   // Pass 2 - link constraints, now that every sim[] is known.  Constrain only
   // to another simulated joint; anything hanging off the root (or off dropped
   // scaffolding) becomes a free root of the body.
   for (int i = 0; i < n; i++) {
      const int  p      = eff[i];
      const bool linked = s->sim[i] && p >= 0 && s->sim[p];
      s->cparent[i] = (signed char)(linked ? p : -1);
      s->restLen[i] = linked ? v_len(v_sub(s->pos[i], s->pos[p])) : 0.0f;
   }

   // Direction child - the joint each one aims at to derive its swing.  Cannot
   // just be m_iChild: the pelvis's m_iChild is coincident scaffolding, which is
   // dropped, so the pelvis would have no bone direction and would keep the
   // death clip's orientation forever while its position simulated.  Since it is
   // also the free root it has no ancestor to inherit from either, and the root
   // of the visible torso not rotating reads as the hips detaching.  Fall back to
   // any simulated joint that constrains to this one (for the pelvis, the spine).
   for (int i = 0; i < n; i++) {
      s->dirChild[i] = -1;
      if (!s->sim[i]) continue;

      const int c = (signed char)joints[i * kJointStride + kJointChild];
      if (c >= 0 && c < n && s->sim[c]) { s->dirChild[i] = (signed char)c; continue; }

      for (int j = 0; j < n; j++)
         if (s->sim[j] && s->cparent[j] == i) { s->dirChild[i] = (signed char)j; break; }
   }

   // Coincident scaffolding has to be carried along with the joint it collapsed
   // into.  The engine copies the parent's matrix into these nodes itself (the
   // kuiDummyRootHash branch of BuildWorldMatrices), and we run after that, so
   // leaving them alone strands any mesh skinned to them at the animated pose
   // while the joint they sit on moves away.
   for (int i = 0; i < n; i++) {
      const int e = eff[i];
      s->follow[i] = (signed char)((!s->sim[i] && coincident[i] && e >= 0 && s->sim[e])
                                      ? e : -1);
   }

   // Pass 3 - build the truss.  Undirected adjacency from the constraint links,
   // then a bounded BFS out of every joint; each pair found within
   // kConstraintDepth hops becomes one distance constraint at its seed length.
   signed char nb[kMaxJoints][kMaxNeighbors];
   int         nbCount[kMaxJoints] = {};

   for (int i = 0; i < n; i++) {
      const int p = s->cparent[i];
      if (p < 0 || p >= n) continue;
      if (nbCount[i] < kMaxNeighbors) nb[i][nbCount[i]++] = (signed char)p;
      if (nbCount[p] < kMaxNeighbors) nb[p][nbCount[p]++] = (signed char)i;
   }

   s->consCount = 0;
   for (int src = 0; src < n; src++) {
      if (!s->sim[src]) continue;

      signed char dist[kMaxJoints];
      std::memset(dist, -1, sizeof(dist));
      signed char queue[kMaxJoints];
      int head = 0, tail = 0;

      dist[src] = 0;
      queue[tail++] = (signed char)src;

      while (head < tail) {
         const int cur = queue[head++];
         if (dist[cur] >= kConstraintDepth) continue;

         for (int e = 0; e < nbCount[cur]; e++) {
            const int nxt = nb[cur][e];
            if (dist[nxt] >= 0) continue;
            dist[nxt] = (signed char)(dist[cur] + 1);
            queue[tail++] = (signed char)nxt;

            // Emit each pair once, and skip degenerate coincident pairs (the
            // three shoulder branch joints sit on top of each other).
            if (nxt <= src) continue;
            const float len = v_len(v_sub(s->pos[src], s->pos[nxt]));
            if (len < kCoincidentEps) continue;
            if (s->consCount >= kMaxConstraints) break;

            Constraint& c = s->cons[s->consCount++];
            c.a     = (signed char)src;
            c.b     = (signed char)nxt;
            c.len   = len;
            c.stiff = kStiffByDepth[dist[nxt]];
         }
      }
   }

   // Contact plane.  A soldier is standing on the ground at the instant it dies,
   // so its lowest joint IS ground level - measuring it removes the assumption
   // that the model origin sits exactly at the feet.  Clamped to <= 0 so a
   // soldier killed in mid-air still falls to its own origin plane, which the
   // engine keeps on the terrain as the corpse entity drops.  Only simulated
   // joints count: the dropped aim nodes sit wherever the procedural pass put
   // them (one of them hangs 1.8m below the head) and would drag the floor down
   // with them.
   float lowest = 0.0f;
   for (int i = 0; i < n; i++)
      if (s->sim[i] && s->pos[i].y < lowest) lowest = s->pos[i].y;
   s->groundY = lowest;

   int simCount = 0;
   for (int i = 0; i < n; i++) if (s->sim[i]) simCount++;
   s->simCount = simCount;

   // Everything above ran in MODEL space, which is the space the world matrices
   // are built in and the space the classification and rest lengths belong to.
   // The simulation itself runs in WORLD space so the body has inertia against
   // the corpse entity's own motion - in model space that motion is invisible,
   // so a corpse sliding downhill was dragged along rigidly instead of tumbling.
   // Rest lengths and the truss survive the move unchanged because the entity
   // transform is rigid.
   // The contact plane is frozen HERE, in world space, rather than tracked off
   // the entity every frame.  The entity keeps sliding under its own death
   // impulse and the body now travels under its own, but the two are integrated
   // separately and will not agree; tracking the entity would drag the floor
   // around under a body that is not following it.  Freezing makes them
   // independent, so entity drift cannot reach us.
   // The cost is that the plane is the height where the soldier DIED, not where
   // the body lands.  Exact on flat ground; a body thrown off a ledge stops in
   // mid-air at the old height, and one blown up a slope sinks into it.  Fixing
   // that properly means a real terrain height at the landing point, which is
   // the one thing this design has so far avoided needing.
   s->groundY += entityMat[13];

   // Seed the body's momentum from the corpse entity's own velocity, which is
   // where EntitySoldier::DirectionKill has just deposited the death impulse.
   // Verlet carries velocity as the gap between pos and prev, and step() scales
   // that gap by kDamping before applying it, so the gap has to be pre-divided
   // to make the first step come out at exactly v.  Without this the body is
   // born at rest and no explosion can throw it, however hard it hit.
   float speed = v_len(vel);
   Vec3  v     = vel;
   if (speed > kMaxSeedSpeed) {
      const float k = kMaxSeedSpeed / speed;
      v.x *= k; v.y *= k; v.z *= k;
   }
   const float gap = kFixedStep / kDamping;

   for (int i = 0; i < n; i++) {
      s->pos[i]  = xform_point(entityMat, s->pos[i]);
      s->prev[i] = {s->pos[i].x - v.x * gap,
                    s->pos[i].y - v.y * gap,
                    s->pos[i].z - v.z * gap};
   }

   s->seeded = true;

   // Logged per corpse, not once per level: only some deaths are explosions, so
   // a single sample cannot tell a correct mVelocity from a wrong one.  Across a
   // few kills this should read ~0 for a standing shot, a few m/s along the
   // travel direction for a running one, and 5-9 m/s biased upward and away from
   // the blast for an explosion.  Anything constant, huge, or unrelated to how
   // the soldier died means kEntityVelocity is pointing at the wrong field.
   if (g_soldierRagdollDebug) {
      auto fn_log = get_gamelog();
      fn_log("[Ragdoll] seed vel=(%.3f, %.3f, %.3f) speed=%.3f%s\n",
             vel.x, vel.y, vel.z, speed,
             speed > kMaxSeedSpeed ? "  CLAMPED - suspect offset" : "");
   }

   // One-shot dump of the seed configuration.  The model-space frame is the one
   // thing here that cannot be settled by reading the disassembly - whether the
   // skeleton root sits at the feet, the pelvis or somewhere else decides where
   // the contact plane belongs, and this answers it directly.  Logged for the
   // first corpse per level only; [Features] SoldierRagdollDebug=1.
   if (g_soldierRagdollDebug && !g_loggedSeed) {
      g_loggedSeed = true;
      auto fn_log = get_gamelog();
      fn_log("[Ragdoll] seed: numJoints=%d simulated=%d constraints=%d "
             "medianBone=%.4f outlierOver=%.4f groundY=%.4f contactY=%.4f\n",
             n, s->simCount, s->consCount, medianLen, outlierLen,
             s->groundY, s->groundY + kJointRadius);
      for (int i = 0; i < n; i++) {
         fn_log("[Ragdoll]   j%02d %s parent=%-3d cparent=%-3d child=%-3d "
                "pos=(%.3f, %.3f, %.3f) restLen=%.4f\n",
                i, s->sim[i] ? "sim " : "DROP", (int)s->parent[i],
                (int)s->cparent[i],
                (int)(signed char)joints[i * kJointStride + kJointChild],
                s->pos[i].x, s->pos[i].y, s->pos[i].z, s->restLen[i]);
      }
   }
}

// ---------------------------------------------------------------------------
// Solver
// ---------------------------------------------------------------------------

static void step(RagdollState* s, float dt, float groundY)
{
   const int n = s->numJoints;

   // Verlet integration.  The root is simulated too - the whole body collapses
   // towards the ground plane, which in model space is where the feet were.
   const float gdt2 = kGravity * dt * dt;
   for (int i = 0; i < n; i++) {
      if (!s->sim[i]) continue;
      const Vec3 cur = s->pos[i];
      s->pos[i].x += (cur.x - s->prev[i].x) * kDamping;
      s->pos[i].y += (cur.y - s->prev[i].y) * kDamping + gdt2;
      s->pos[i].z += (cur.z - s->prev[i].z) * kDamping;
      s->prev[i] = cur;
   }

   // Gauss-Seidel relaxation, same structure as EntityCloth::SatisfyConstraints.
   // Both endpoints move half the error, so the chain converges without a pinned
   // root.  Bone constraints run at full stiffness (a bone does not stretch);
   // bend constraints run soft so the body resists folding flat without going
   // rigid.
   for (int it = 0; it < kIterations; it++) {
      for (int ci = 0; ci < s->consCount; ci++) {
         const Constraint& c = s->cons[ci];

         Vec3 d = v_sub(s->pos[c.a], s->pos[c.b]);
         const float len = v_len(d);
         if (len < 1e-6f) continue;

         const float k = ((len - c.len) / len) * 0.5f * c.stiff;
         d.x *= k; d.y *= k; d.z *= k;
         s->pos[c.a].x -= d.x; s->pos[c.a].y -= d.y; s->pos[c.a].z -= d.z;
         s->pos[c.b].x += d.x; s->pos[c.b].y += d.y; s->pos[c.b].z += d.z;
      }
   }

   // Contacts are resolved ONCE, after the relaxation, not inside it.  Clamping
   // every iteration lets the floor dominate the solve: each pass shoves joints
   // back onto the plane and the constraints never recover the bone lengths, so
   // the whole skeleton converges onto a single height.  Each joint stops a
   // radius above the plane so the body keeps some thickness.
   const float contactY = groundY + kJointRadius;
   for (int i = 0; i < n; i++) {
      if (!s->sim[i] || s->pos[i].y >= contactY) continue;
      s->pos[i].y = contactY;
      // Contact friction: bleed off the tangential velocity by dragging the
      // previous position towards the current one.
      s->prev[i].x += (s->pos[i].x - s->prev[i].x) * kGroundFric;
      s->prev[i].z += (s->pos[i].z - s->prev[i].z) * kGroundFric;
      if (s->prev[i].y < contactY) s->prev[i].y = contactY;
   }
}

// ---------------------------------------------------------------------------
// Write-back - turn particle positions into joint world matrices
// ---------------------------------------------------------------------------
// Translation is just the particle.  For rotation we keep the animated pose's
// orientation and apply the minimal ("swing") rotation that takes the bone's
// original direction onto its simulated one.  That preserves the clip's roll
// about the bone axis, which a basis rebuilt from the direction alone would
// throw away - the classic cause of limbs spinning about themselves.
//
// A leaf joint has no direction of its own, so it inherits its parent's
// orientation unchanged.

static void axis_angle_to_mat3(const Vec3& axis, float s, float c, float out[9])
{
   const float t = 1.0f - c;
   const float x = axis.x, y = axis.y, z = axis.z;
   // D3DXMatrixRotationAxis form (row-vector convention: v' = v * R).
   out[0] = c + t * x * x;     out[1] = t * x * y + s * z; out[2] = t * x * z - s * y;
   out[3] = t * x * y - s * z; out[4] = c + t * y * y;     out[5] = t * y * z + s * x;
   out[6] = t * x * z + s * y; out[7] = t * y * z - s * x; out[8] = c + t * z * z;
}

static void write_back(RagdollState* s, float* world, const uint8_t* joints,
                       const float* entityMat)
{
   const int n = s->numJoints;

   // The solver works in world space but the matrices we are writing are model
   // space, so bring the simulated positions back first.  Everything below then
   // stays entirely model-space, which is what keeps the swing math correct: it
   // compares an animated direction against a simulated one, and those two have
   // to be in the same space.
   Vec3 simT[kMaxJoints];
   for (int i = 0; i < n; i++)
      simT[i] = s->sim[i] ? xform_point_inv(entityMat, s->pos[i]) : Vec3{};

   // Snapshot the animated translations too: the write loop overwrites row 3 in
   // place, and a joint's swing needs its child's ORIGINAL position.
   Vec3 origT[kMaxJoints];
   for (int i = 0; i < n; i++) {
      const float* m = world + i * kMatrixFloats;
      origT[i] = {m[kMatTransX], m[kMatTransX + 1], m[kMatTransX + 2]};
   }

   // Pass 1 - swing per joint, from its bone direction.
   float rot[kMaxJoints][9];
   bool  hasRot[kMaxJoints] = {};

   for (int i = 0; i < n; i++) {
      if (!s->sim[i]) continue;
      const int c = s->dirChild[i];
      if (c < 0 || c >= n) continue;

      Vec3 a = v_sub(origT[c], origT[i]);
      Vec3 b = v_sub(simT[c], simT[i]);
      const float la = v_len(a), lb = v_len(b);
      if (la < 1e-6f || lb < 1e-6f) continue;

      a.x /= la; a.y /= la; a.z /= la;
      b.x /= lb; b.y /= lb; b.z /= lb;

      Vec3 axis = v_cross(a, b);
      const float sn = v_len(axis);
      const float cs = v_dot(a, b);

      // sn ~ 0 means the bone is unmoved (cs > 0) or exactly reversed (cs < 0).
      // A reversal has no well-defined axis; leave it alone rather than pick an
      // arbitrary one and snap the limb.
      if (sn <= 1e-5f) continue;

      axis.x /= sn; axis.y /= sn; axis.z /= sn;
      axis_angle_to_mat3(axis, sn, cs, rot[i]);
      hasRot[i] = true;
   }

   // Pass 2 - a joint whose only child was dropped has no bone direction of its
   // own: the hands (holding an aim node) and, more visibly, the head.  Left
   // alone they would translate with the body while keeping the death clip's
   // orientation - a head staying upright over a corpse lying down.  Inherit the
   // nearest simulated ancestor's swing instead.
   for (int i = 0; i < n; i++) {
      if (!s->sim[i] || hasRot[i]) continue;
      int a = s->cparent[i], guard = 0;
      while (a >= 0 && a < n && !hasRot[a] && guard++ < kMaxJoints)
         a = s->cparent[a];
      if (a >= 0 && a < n && hasRot[a]) {
         std::memcpy(rot[i], rot[a], sizeof(rot[i]));
         hasRot[i] = true;
      }
   }

   // Pass 3 - apply.
   for (int i = 0; i < n; i++) {
      // Scaffolding and aim nodes keep whatever the animator produced: they are
      // not simulated, so there is no simulated position to move them to.
      if (!s->sim[i]) continue;

      float* m = world + i * kMatrixFloats;

      if (hasRot[i]) {
         const float* r = rot[i];
         // 3x3 rotation part only: M_new = M_orig * R.  Row 3 is set below, so
         // the translation is never run through R.
         for (int row = 0; row < 3; row++) {
            const float e0 = m[row * 4 + 0], e1 = m[row * 4 + 1], e2 = m[row * 4 + 2];
            m[row * 4 + 0] = e0 * r[0] + e1 * r[3] + e2 * r[6];
            m[row * 4 + 1] = e0 * r[1] + e1 * r[4] + e2 * r[7];
            m[row * 4 + 2] = e0 * r[2] + e1 * r[5] + e2 * r[8];
         }
      }

      m[kMatTransX + 0] = simT[i].x;
      m[kMatTransX + 1] = simT[i].y;
      m[kMatTransX + 2] = simT[i].z;
   }

   // Pass 4 - drag the collapsed scaffolding along, exactly as the engine's own
   // dummy-root branch does.
   for (int i = 0; i < n; i++) {
      const int f = s->follow[i];
      if (f < 0 || f >= n) continue;
      std::memcpy(world + i * kMatrixFloats, world + f * kMatrixFloats,
                  kMatrixFloats * sizeof(float));
   }
}

// ---------------------------------------------------------------------------
// Hook: SoldierAnimator::ApplyProceduralAnimationAndBuildWorldMatrices
// ---------------------------------------------------------------------------
// __thiscall(SoldierAnimator*, float dt) - the float rides on the stack, so the
// __fastcall spelling with a dummy EDX matches it exactly.

using fn_ApplyProcedural_t = void(__fastcall*)(void* ecx, void* edx, float dt);

static fn_ApplyProcedural_t original_ApplyProcedural = nullptr;

static void __fastcall hooked_ApplyProcedural(void* ecx, void* /*edx*/, float dt)
{
   original_ApplyProcedural(ecx, nullptr, dt);

   if (!g_soldierRagdollEnabled || !ecx) return;

   __try {
      uint8_t* animator = (uint8_t*)ecx;

      uint8_t* owner = *(uint8_t**)(animator + kSAOwner);
      if (!owner) return;

      const uint8_t healthFlags = *(uint8_t*)(owner + kHealthFlags);
      if ((healthFlags >> kIsAliveBit) & 1) {
         // Alive: drop any state so a respawn into the same pooled animator
         // does not resume the previous corpse's simulation.
         release_state(ecx);
         return;
      }

      // The corpse's own world transform.  Used to move the sim in and out of
      // world space; the contact plane is taken from it once, at seed, and then
      // frozen (see seed()) rather than tracked per frame.
      const float* entityMat = (const float*)(owner + kEntityMatrix);
      if (!matrix_is_sane(entityMat)) return;

      uint8_t* skel   = animator + kSAZephyrSkeleton;
      uint8_t* shared = *(uint8_t**)(skel + kZSShared);
      if (!shared) return;

      uint8_t* joints = *(uint8_t**)(shared + kZSSJoints);
      int      n      = (int)*(uint32_t*)(shared + kZSSNumJoints);
      if (!joints || n <= 1) return;
      if (n > kMaxJoints) n = kMaxJoints;

      float* world = (float*)(skel + kZSWorldMatrices);

      RagdollState* s = find_state(ecx);
      if (!s) {
         s = acquire_state(ecx);
         if (!s) return;
      }
      s->touchTick = ++g_tick;

      if (!s->seeded) {
         // Read on the first dead frame, which is the frame after DirectionKill
         // wrote the impulse, so this still carries the blast.  Read at seed and
         // never again: from here the body is on its own, and the entity's later
         // motion must not feed back into a sim it is no longer driving.
         const float* ev = (const float*)(owner + kEntityVelocity);
         seed(s, world, joints, n, entityMat, Vec3{ev[0], ev[1], ev[2]});
      }
      if (s->numJoints != n) return;        // skeleton swapped under us
      if (s->simCount < kMinSimJoints) return; // nothing bone-like to simulate

      const float groundY = s->groundY; // world space, frozen at seed

      // Park the sim once the body has settled, but keep writing the final pose
      // so the death clip cannot creep back in underneath it.
      if (s->age < kSettleTime) {
         s->age += dt;
         s->accum += dt;
         int steps = 0;
         while (s->accum >= kFixedStep && steps < kMaxSubsteps) {
            step(s, kFixedStep, groundY);
            s->accum -= kFixedStep;
            steps++;
         }
         if (s->accum > kFixedStep * kMaxSubsteps) s->accum = 0.0f; // hitch guard
      }

      write_back(s, world, joints, entityMat);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      // A torn-down animator mid-frame must never take the game with it.
      release_state(ecx);
   }
}

// ---------------------------------------------------------------------------
// Install / Uninstall / Reset
// ---------------------------------------------------------------------------

void soldier_ragdoll_install(uintptr_t exe_base)
{
   if (!g_soldierRagdollEnabled) return;

   // Modtools only for now - the Steam/GOG address is not ported yet, and the
   // generated table leaves it 0 there.
   if (g_addr->soldier_apply_procedural == 0) return;

   original_ApplyProcedural =
      (fn_ApplyProcedural_t)resolve(exe_base, g_addr->soldier_apply_procedural);

   soldier_ragdoll_reset();

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_ApplyProcedural, hooked_ApplyProcedural);
   DetourTransactionCommit();
}

void soldier_ragdoll_uninstall()
{
   if (!original_ApplyProcedural) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(&(PVOID&)original_ApplyProcedural, hooked_ApplyProcedural);
   DetourTransactionCommit();
}

void soldier_ragdoll_reset()
{
   std::memset(g_ragdolls, 0, sizeof(g_ragdolls));
   g_tick = 0;
   g_loggedSeed = false;
}
