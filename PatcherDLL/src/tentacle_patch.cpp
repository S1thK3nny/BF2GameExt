#include "pch.h"

#include "tentacle_patch.hpp"
#include "cfile.hpp"

#include <detours.h>
#include <math.h>
#include <string.h>

// ============================================================================
// 1. Constants
// ============================================================================

static constexpr int MAX_TENTACLES      = 9;
static constexpr int MAX_BONES_PER_TENT = 5;
static constexpr int TENT_STRIDE        = 6;  // tPos/oldPos stride per tentacle
static constexpr int BONE_PTR_STRIDE    = 5;  // bonePtrs stride per tentacle
static constexpr int TOTAL_BONES        = MAX_TENTACLES * MAX_BONES_PER_TENT; // 45

// Physics constants (from .rdata at modtools addresses)
static constexpr float VELOCITY_CLAMP  = 10.0f;
static constexpr float ACCEL_CLAMP     = 20.0f;
static constexpr float DAMPING_FACTOR  = 0.5f;
static constexpr float VELOCITY_DAMPING = -5.0f;
static constexpr float GRAVITY_SCALE   = 1.0f;
static constexpr float GRAVITY_Y       = -9.8f;
static constexpr float MAX_DT          = 0.039f;
static constexpr float ANGLE_CLAMP     = 0.5235988f; // 30 degrees
static constexpr float TAIL_REST_LEN   = 0.25f;      // rest length for last bone

// Collision constants (from EnforceCollisions decompilation)
static constexpr float SPHERE_RADIUS      = 1.234f;
static constexpr float SPHERE_UP_OFFSET   = 0.98f;
static constexpr float BOX_HALF_RIGHT     = 0.35f;
static constexpr float BOX_HALF_UP        = 0.18f;
static constexpr float BOX_HALF_FWD       = 0.5f;
static constexpr float CYLINDER_HALF_HEIGHT = 0.18f;
static constexpr float CYLINDER_RADIUS    = 1.234f;

// Hash table size parameter (128 keys + 128 values = 256 uint32s, tableSize=0x100)
static constexpr int HT_TABLE_SIZE = 0x100;

// ============================================================================
// 2. CRC-32/BZIP2 — used for bone name hashes
// Poly 0x04C11DB7, MSB-first, init 0xFFFFFFFF, final XOR 0xFFFFFFFF
// ============================================================================

static uint32_t g_crc32_table[256];

static void init_crc32_table()
{
   for (int i = 0; i < 256; i++) {
      uint32_t crc = (uint32_t)i << 24;
      for (int j = 0; j < 8; j++) {
         if (crc & 0x80000000)
            crc = (crc << 1) ^ 0x04C11DB7;
         else
            crc <<= 1;
      }
      g_crc32_table[i] = crc;
   }
}

static uint32_t bone_hash(const char* str)
{
   uint32_t hash = 0xFFFFFFFF;
   while (*str) {
      hash = g_crc32_table[((hash >> 24) ^ (uint8_t)*str) & 0xFF] ^ (hash << 8);
      str++;
   }
   return hash ^ 0xFFFFFFFF;
}

// ============================================================================
// Expanded bone hash table — 45 entries (9 tentacles * 5 bones)
// ============================================================================

static uint32_t g_bone_hashes[TOTAL_BONES];
static uint32_t g_hash_bone_ribcage;

static bool init_bone_hashes(uintptr_t original_table_addr, cfile& log)
{
   init_crc32_table();

   char name_buf[32];
   for (int i = 0; i < TOTAL_BONES; i++) {
      sprintf_s(name_buf, "bone_string_%d", i + 1);
      g_bone_hashes[i] = bone_hash(name_buf);
   }

   g_hash_bone_ribcage = bone_hash("bone_ribcage");

   // Verify first hash matches the game's table
   uint32_t game_first_hash = *(uint32_t*)original_table_addr;
   if (g_bone_hashes[0] != game_first_hash) {
      log.printf("Tentacle: hash mismatch: computed %08x, game has %08x (at %08x)\n",
                 g_bone_hashes[0], game_first_hash, original_table_addr);
      return false;
   }

   return true;
}

// ============================================================================
// 3. Types
// ============================================================================

struct PblVec3 {
   float x, y, z;
};

struct PblMat4 {
   PblVec3 right;  float rw;
   PblVec3 up;     float uw;
   PblVec3 fwd;    float fw;
   PblVec3 trans;   float tw;
};

struct TentSim {
   uint8_t _pad[0x240];
   PblVec3 oldVelocity;          // 0x240
   float   mInternalTimer;       // 0x24C
   float   mTimeSinceLastUpdate;  // 0x250
   float   mTimerOffset;          // 0x254
   int     mNumTentacles;         // 0x258
   int     mBonesPerTentacle;     // 0x25C
   int     mCollType;             // 0x260
   int     mFirstUpdate;          // 0x264
   PblVec3 tPos[54];              // 0x268 (9 tentacles * 6 stride)
   PblVec3 oldPos[54];            // 0x4F0
};
static_assert(sizeof(TentSim) == 0x778, "TentSim size mismatch");

// RedPose layout: *(int*)(pose+0) = numEntries, (uint32_t*)(pose+4) = table data
struct RedPose {
   int     numEntries;
   uint32_t tableData[256]; // 128 keys + 128 values
};

// ============================================================================
// 4. Math helpers
// ============================================================================

static float vec_length(const PblVec3& v)
{
   return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

static PblVec3 vec_sub(const PblVec3& a, const PblVec3& b)
{
   return { a.x - b.x, a.y - b.y, a.z - b.z };
}

static PblVec3 vec_add(const PblVec3& a, const PblVec3& b)
{
   return { a.x + b.x, a.y + b.y, a.z + b.z };
}

static PblVec3 vec_scale(const PblVec3& v, float s)
{
   return { v.x * s, v.y * s, v.z * s };
}

static float vec_dot(const PblVec3& a, const PblVec3& b)
{
   return a.x * b.x + a.y * b.y + a.z * b.z;
}

static PblVec3 vec_cross(const PblVec3& a, const PblVec3& b)
{
   return {
      a.y * b.z - a.z * b.y,
      a.z * b.x - a.x * b.z,
      a.x * b.y - a.y * b.x
   };
}

static PblVec3 vec_normalize(const PblVec3& v)
{
   float len = vec_length(v);
   if (len < 1e-12f) return v;
   float inv = 1.0f / len;
   return { v.x * inv, v.y * inv, v.z * inv };
}

// Clamp a vector's magnitude to maxLen; return clamped vector
static PblVec3 vec_clamp_length(const PblVec3& v, float maxLen)
{
   float sqLen = v.x * v.x + v.y * v.y + v.z * v.z;
   if (sqLen > maxLen * maxLen) {
      float s = maxLen / sqrtf(sqLen);
      return { v.x * s, v.y * s, v.z * s };
   }
   return v;
}

// Row-major matrix: mat_rotate_vec(M, v) = {dot(M.right,v), dot(M.up,v), dot(M.fwd,v)}
static PblVec3 mat_rotate_vec(const PblMat4& m, const PblVec3& v)
{
   return {
      m.right.x * v.x + m.right.y * v.y + m.right.z * v.z,
      m.up.x * v.x    + m.up.y * v.y    + m.up.z * v.z,
      m.fwd.x * v.x   + m.fwd.y * v.y   + m.fwd.z * v.z
   };
}

// Inverse of an orthonormal affine matrix: transpose 3x3 rotation, negate transformed translation
static PblMat4 mat_inverse_fast(const PblMat4& m)
{
   PblMat4 inv;
   // Transpose rotation
   inv.right = { m.right.x, m.up.x, m.fwd.x }; inv.rw = 0.0f;
   inv.up    = { m.right.y, m.up.y, m.fwd.y }; inv.uw = 0.0f;
   inv.fwd   = { m.right.z, m.up.z, m.fwd.z }; inv.fw = 0.0f;
   // invTrans = -(transposedRot * origTrans)
   inv.trans.x = -(inv.right.x * m.trans.x + inv.right.y * m.trans.y + inv.right.z * m.trans.z);
   inv.trans.y = -(inv.up.x * m.trans.x    + inv.up.y * m.trans.y    + inv.up.z * m.trans.z);
   inv.trans.z = -(inv.fwd.x * m.trans.x   + inv.fwd.y * m.trans.y   + inv.fwd.z * m.trans.z);
   inv.tw = 1.0f;
   return inv;
}

// Full 4x4 matrix multiply (row-major, affine)
static PblMat4 mat_multiply(const PblMat4& a, const PblMat4& b)
{
   PblMat4 r;
   r.right.x = a.right.x * b.right.x + a.right.y * b.up.x + a.right.z * b.fwd.x;
   r.right.y = a.right.x * b.right.y + a.right.y * b.up.y + a.right.z * b.fwd.y;
   r.right.z = a.right.x * b.right.z + a.right.y * b.up.z + a.right.z * b.fwd.z;
   r.rw = 0.0f;

   r.up.x = a.up.x * b.right.x + a.up.y * b.up.x + a.up.z * b.fwd.x;
   r.up.y = a.up.x * b.right.y + a.up.y * b.up.y + a.up.z * b.fwd.y;
   r.up.z = a.up.x * b.right.z + a.up.y * b.up.z + a.up.z * b.fwd.z;
   r.uw = 0.0f;

   r.fwd.x = a.fwd.x * b.right.x + a.fwd.y * b.up.x + a.fwd.z * b.fwd.x;
   r.fwd.y = a.fwd.x * b.right.y + a.fwd.y * b.up.y + a.fwd.z * b.fwd.y;
   r.fwd.z = a.fwd.x * b.right.z + a.fwd.y * b.up.z + a.fwd.z * b.fwd.z;
   r.fw = 0.0f;

   r.trans.x = a.trans.x * b.right.x + a.trans.y * b.up.x + a.trans.z * b.fwd.x + b.trans.x;
   r.trans.y = a.trans.x * b.right.y + a.trans.y * b.up.y + a.trans.z * b.fwd.y + b.trans.y;
   r.trans.z = a.trans.x * b.right.z + a.trans.y * b.up.z + a.trans.z * b.fwd.z + b.trans.z;
   r.tw = 1.0f;
   return r;
}

// ============================================================================
// 5. Hash table reimplementation (pure algorithms, no game calls)
// Layout: half = tableSize >> 1 = 128
//         keys at table[0..half-1], values at table[half..tableSize-1]
//         probe = table + ((half-1) & hash), linear probe backward with wrap
// ============================================================================

static void* ht_find(uint32_t* table, int tableSize, uint32_t key)
{
   if (key == 0) return nullptr;
   int half = tableSize >> 1;
   uint32_t* probe = table + ((half - 1) & key);
   uint32_t cur = *probe;
   while (true) {
      if (cur == key) return (void*)probe[half];
      if (cur == 0) return nullptr;
      if (probe <= table) probe += half;
      probe--;
      cur = *probe;
   }
}

static bool ht_store(uint32_t* table, int tableSize, uint32_t key, void* value)
{
   int half = tableSize >> 1;
   uint32_t* probe = table + ((half - 1) & key);
   uint32_t cur = *probe;
   while (true) {
      if (cur == 0) {
         *probe = key;
         probe[half] = (uint32_t)(uintptr_t)value;
         return true;
      }
      if (*probe == key) return false; // already exists
      if (probe <= table) probe += half;
      probe--;
      cur = *probe;
   }
}

static bool ht_remove(uint32_t* table, int tableSize, uint32_t key)
{
   int half = tableSize >> 1;
   uint32_t* probe = table + ((half - 1) & key);
   uint32_t cur = *probe;
   while (true) {
      if (cur == key) {
         *probe = 0;
         // Rehash following entries in the cluster
         if (probe <= table) probe += half;
         probe--;
         cur = *probe;
         while (cur != 0) {
            *probe = 0;
            ht_store(table, tableSize, cur, (void*)probe[half]);
            if (probe <= table) probe += half;
            probe--;
            cur = *probe;
         }
         return true;
      }
      if (*probe == 0) return false;
      if (probe <= table) probe += half;
      probe--;
      cur = *probe;
   }
}

// ============================================================================
// 6. Simulation functions
// ============================================================================

// --- UpdatePositions ---
static void UpdatePositions(TentSim* self, float dt, PblVec3* velocity,
                            RedPose* pose, PblMat4* parentMatrix, PblMat4** bonePtrs)
{
   float invDt = 1.0f / dt;

   // Clamp velocity magnitude
   PblVec3 vel = vec_clamp_length(*velocity, VELOCITY_CLAMP);

   // Compute acceleration = (vel - oldVelocity) / dt, clamp to ACCEL_CLAMP
   PblVec3 accel = vec_scale(vec_sub(vel, self->oldVelocity), invDt);
   accel = vec_clamp_length(accel, ACCEL_CLAMP);

   // Store velocity
   self->oldVelocity = vel;

   // Compute parent inverse
   PblMat4 parentInv = mat_inverse_fast(*parentMatrix);

   // Transform forces to local space
   PblVec3 gravity = { 0.0f, GRAVITY_Y, 0.0f };
   PblVec3 scaledAccel = vec_scale(accel, DAMPING_FACTOR);
   PblVec3 worldForce = vec_sub(gravity, scaledAccel);
   PblVec3 localForce = mat_rotate_vec(parentInv, worldForce);
   PblVec3 localVel = mat_rotate_vec(parentInv, vel);

   float invGravityScale = 1.0f / GRAVITY_SCALE;
   int numT = self->mNumTentacles;
   int bpt = self->mBonesPerTentacle;

   // Loop 1: Compute per-bone acceleration into parentInv storage (reuse as scratch)
   // We store acceleration in a local array
   PblVec3 boneAccel[54]; // max bones
   for (int t = 0; t < numT; t++) {
      for (int b = 1; b < bpt + 1; b++) {
         int idx = t * TENT_STRIDE + b;
         PblVec3 boneVel = vec_scale(vec_sub(self->tPos[idx], self->oldPos[idx]), invDt);
         PblVec3 combinedVel = vec_add(localVel, boneVel);
         boneAccel[idx] = vec_add(localForce, vec_scale(combinedVel, VELOCITY_DAMPING * invGravityScale));
      }
   }

   // Loop 2: Verlet integration
   float dt2 = dt * dt;
   for (int t = 0; t < numT; t++) {
      for (int b = 1; b < bpt + 1; b++) {
         int idx = t * TENT_STRIDE + b;
         PblVec3 cur = self->tPos[idx];
         PblVec3 old = self->oldPos[idx];
         // newPos = cur + (cur - old) + accel * dt^2
         PblVec3 newPos = vec_add(vec_add(cur, vec_sub(cur, old)), vec_scale(boneAccel[idx], dt2));
         self->oldPos[idx] = cur;
         self->tPos[idx] = newPos;
      }
   }

   // Loop 3: Root bone fixup — tPos[t][0] = bonePtrs[t][0]->trans
   for (int t = 0; t < numT; t++) {
      int idx = t * TENT_STRIDE;
      PblMat4* rootBone = bonePtrs[t * BONE_PTR_STRIDE];
      self->tPos[idx] = rootBone->trans;
   }

   // Loop 4: Distance constraints — enforce rest lengths between consecutive bones
   for (int t = 0; t < numT; t++) {
      for (int b = 0; b < bpt; b++) {
         int curIdx = t * TENT_STRIDE + b + 1;
         int prevIdx = curIdx - 1;

         PblVec3 delta = vec_sub(self->tPos[curIdx], self->tPos[prevIdx]);
         float dist = vec_length(delta);

         float restLen;
         if (b == bpt - 1) {
            // Last bone: use fixed rest length
            restLen = TAIL_REST_LEN;
         }
         else {
            // Compute rest length from reference bone matrices
            int boneIdx = t * BONE_PTR_STRIDE + b;
            PblMat4* boneA = bonePtrs[boneIdx];
            PblMat4* boneB = bonePtrs[boneIdx + 1];
            PblVec3 d = vec_sub(boneB->trans, boneA->trans);
            restLen = vec_length(d);
         }

         float correction = (dist - restLen) / dist;
         self->tPos[curIdx] = vec_sub(self->tPos[curIdx], vec_scale(delta, correction));
      }
   }
}

// --- EnforceSphereCollision ---
static void EnforceSphereCollision(TentSim* self, PblMat4* collMatrix, float radius)
{
   int numT = self->mNumTentacles;
   int bpt = self->mBonesPerTentacle;
   float r2 = radius * radius;

   for (int t = 0; t < numT; t++) {
      for (int b = 1; b < bpt + 1; b++) {
         int idx = t * TENT_STRIDE + b;
         PblVec3 delta = vec_sub(self->tPos[idx], collMatrix->trans);
         float d2 = vec_dot(delta, delta);
         if (d2 < r2) {
            float scale = radius / sqrtf(d2);
            self->tPos[idx] = vec_add(collMatrix->trans, vec_scale(delta, scale));
         }
      }
   }
}

// --- EnforceBoxCollision ---
static void EnforceBoxCollision(TentSim* self, PblMat4* collMatrix,
                                 float halfRight, float halfUp, float halfFwd)
{
   int numT = self->mNumTentacles;
   int bpt = self->mBonesPerTentacle;

   for (int t = 0; t < numT; t++) {
      for (int b = 1; b < bpt + 1; b++) {
         int idx = t * TENT_STRIDE + b;
         PblVec3& pos = self->tPos[idx];
         PblVec3 delta = vec_sub(pos, collMatrix->trans);

         float dotR = vec_dot(delta, collMatrix->right);
         float penR = halfRight - fabsf(dotR);
         if (penR <= 0.0f) continue;

         float dotU = vec_dot(delta, collMatrix->up);
         float penU = halfUp - fabsf(dotU);
         if (penU <= 0.0f) continue;

         float dotF = vec_dot(delta, collMatrix->fwd);
         float penF = halfFwd - fabsf(dotF);
         if (penF <= 0.0f) continue;

         // Push out along the axis with least penetration
         if (penR <= penU) {
            if (penF <= penR) {
               // forward has least penetration
               float sign = (dotF >= 0.0f) ? 1.0f : -1.0f;
               pos = vec_add(pos, vec_scale(collMatrix->fwd, sign * penF));
            }
            else {
               // right has least penetration
               float sign = (dotR >= 0.0f) ? 1.0f : -1.0f;
               pos = vec_add(pos, vec_scale(collMatrix->right, sign * penR));
            }
         }
         else {
            if (penF <= penU) {
               // forward has least penetration
               float sign = (dotF >= 0.0f) ? 1.0f : -1.0f;
               pos = vec_add(pos, vec_scale(collMatrix->fwd, sign * penF));
            }
            else {
               // up has least penetration
               float sign = (dotU >= 0.0f) ? 1.0f : -1.0f;
               pos = vec_add(pos, vec_scale(collMatrix->up, sign * penU));
            }
         }
      }
   }
}

// --- EnforceCylinderCollision ---
static void EnforceCylinderCollision(TentSim* self, PblMat4* collMatrix,
                                      float halfHeight, float radius)
{
   int numT = self->mNumTentacles;
   int bpt = self->mBonesPerTentacle;

   for (int t = 0; t < numT; t++) {
      for (int b = 1; b < bpt + 1; b++) {
         int idx = t * TENT_STRIDE + b;
         PblVec3& pos = self->tPos[idx];
         PblVec3 delta = vec_sub(pos, collMatrix->trans);

         float dotUp = vec_dot(delta, collMatrix->up);
         float penUp = (halfHeight + halfHeight) - fabsf(dotUp);
         if (penUp <= 0.0f) continue;

         float dotR = vec_dot(delta, collMatrix->right);
         float dotF = vec_dot(delta, collMatrix->fwd);
         float radialDist = sqrtf(dotR * dotR + dotF * dotF);
         float penRadial = radius - radialDist;
         if (penRadial <= 0.0f) continue;

         if (penUp <= penRadial) {
            // Push along up axis
            pos = vec_add(pos, vec_scale(collMatrix->up, penUp));
         }
         else {
            // Push radially outward
            float scale = penRadial / radialDist;
            PblVec3 radialDir = vec_add(vec_scale(collMatrix->right, dotR),
                                         vec_scale(collMatrix->fwd, dotF));
            pos = vec_add(pos, vec_scale(radialDir, scale));
         }
      }
   }
}

// --- EnforceCollisions ---
static void EnforceCollisions(TentSim* self, RedPose* pose)
{
   PblMat4* ribcage = (PblMat4*)ht_find(pose->tableData, HT_TABLE_SIZE, g_hash_bone_ribcage);
   if (!ribcage) return;

   // Copy ribcage matrix and build collision center
   PblMat4 collMat = *ribcage;

   float upOffset = 0.0f;
   if (self->mCollType == 1) upOffset = SPHERE_UP_OFFSET;

   // collMat.trans += up * upOffset + right * 0 + fwd * 0 (only up matters)
   collMat.trans.x += collMat.up.x * upOffset;
   collMat.trans.y += collMat.up.y * upOffset;
   collMat.trans.z += collMat.up.z * upOffset;

   switch (self->mCollType) {
   case 0:
      EnforceBoxCollision(self, &collMat, BOX_HALF_RIGHT, BOX_HALF_UP, BOX_HALF_FWD);
      break;
   case 1:
      EnforceSphereCollision(self, &collMat, SPHERE_RADIUS);
      break;
   case 2:
      EnforceCylinderCollision(self, &collMat, CYLINDER_HALF_HEIGHT, CYLINDER_RADIUS);
      break;
   }
}

// --- UpdatePose ---
static void UpdatePose(TentSim* self, RedPose* pose, PblMat4* parentMatrix,
                       PblMat4** bonePtrs, PblMat4* targetMatrices)
{
   if (!targetMatrices) return;

   int numT = self->mNumTentacles;
   int bpt = self->mBonesPerTentacle;
   float cosAngleClamp = cosf(ANGLE_CLAMP);

   for (int t = 0; t < numT; t++) {
      for (int b = 0; b < bpt; b++) {
         int boneIdx = t * BONE_PTR_STRIDE + b;
         PblMat4* refBone = bonePtrs[boneIdx];
         PblMat4* target = &targetMatrices[bpt * t + b];

         // Copy reference bone matrix to target
         *target = *refBone;

         // If not root bone: multiply by inverse of parent bone for parent-local space
         if (b > 0) {
            PblMat4 parentInv = mat_inverse_fast(*bonePtrs[boneIdx - 1]);
            *target = mat_multiply(*target, parentInv);
         }

         // Compute forward direction from tPos[b+1] - tPos[b]
         int curPosIdx = t * TENT_STRIDE + b;
         int nextPosIdx = curPosIdx + 1;
         PblVec3 rawFwd = vec_sub(self->tPos[nextPosIdx], self->tPos[curPosIdx]);
         PblVec3 newFwd = vec_normalize(rawFwd);

         // Angle clamp: if dot(newFwd, target.fwd) < cos(30deg), clamp via axis-angle
         float dotFwd = vec_dot(newFwd, target->fwd);
         if (dotFwd < cosAngleClamp) {
            PblVec3 axis = vec_cross(newFwd, target->fwd);
            float axisLen = vec_length(axis);

            // Compute the rotation angle via atan2
            float angle = atan2f(axisLen, dotFwd);

            // Clamp the angle
            if (axisLen * angle < 0.0f) angle = -angle;
            if (angle > ANGLE_CLAMP) {
               angle = ANGLE_CLAMP - angle;
            }
            else if (angle < -ANGLE_CLAMP) {
               angle = angle + ANGLE_CLAMP;
            }
            else {
               goto skip_clamp;
            }

            // Apply axis-angle rotation to fwd
            // Rodrigues: v' = v*cos(a) + (axis x v)*sin(a) + axis*(axis.v)*(1-cos(a))
            {
               axis = vec_normalize(axis);
               float ca = cosf(angle);
               float sa = sinf(angle);
               PblVec3 cross = vec_cross(axis, target->fwd);
               float dot = vec_dot(axis, target->fwd);
               newFwd.x = target->fwd.x * ca + cross.x * sa + axis.x * dot * (1.0f - ca);
               newFwd.y = target->fwd.y * ca + cross.y * sa + axis.y * dot * (1.0f - ca);
               newFwd.z = target->fwd.z * ca + cross.z * sa + axis.z * dot * (1.0f - ca);
               newFwd = vec_normalize(newFwd);
            }
         }
skip_clamp:

         // Rebuild orthonormal basis
         target->fwd = newFwd;
         target->fw = 0.0f;

         // right = normalize(cross(fwd, up))
         target->right = vec_normalize(vec_cross(target->fwd, target->up));
         target->rw = 0.0f;

         // up = normalize(cross(right, fwd))
         target->up = vec_normalize(vec_cross(target->right, target->fwd));
         target->uw = 0.0f;

         // Update pose hash table: remove + store (with numEntries bookkeeping)
         uint32_t boneKey = g_bone_hashes[bpt * t + b];
         if (ht_remove(pose->tableData, HT_TABLE_SIZE, boneKey)) {
            pose->numEntries--;
         }
         if (ht_store(pose->tableData, HT_TABLE_SIZE, boneKey, (void*)target)) {
            pose->numEntries++;
         }
      }
   }
}

// ============================================================================
// 7. Hooked Constructor
// ============================================================================

typedef void (__thiscall* fn_ctor)(void*, int, int, int);
static fn_ctor original_ctor = nullptr;

static void __fastcall hooked_ctor(TentSim* self, void* /*edx*/, int numT, int bonesPerT, int collType)
{
   // Zero entire struct
   memset(self, 0, sizeof(TentSim));

   // Set scalars
   self->mNumTentacles = numT;
   self->mBonesPerTentacle = bonesPerT;
   self->mCollType = collType;
   self->mFirstUpdate = 1;
}

// ============================================================================
// 8. Hooked DoTentacles
// ============================================================================

typedef void (__thiscall* fn_doTentacles)(void*, void*, void*, void*, void*, float);
static fn_doTentacles original_doTentacles = nullptr;

// Guard hash: bone_string_1 = 0x24CB9E5E
static constexpr uint32_t HASH_BONE_STRING_1 = 0x24CB9E5E;

static void __fastcall hooked_doTentacles(TentSim* self, void* /*edx*/,
                                           RedPose* pose, PblMat4* parentMatrix,
                                           PblVec3* velocity, PblMat4* targetMatrices, float dt)
{
   // Timing: always use offline path — dt = min(mInternalTimer, MAX_DT)
   dt = self->mInternalTimer;
   if (dt > MAX_DT) dt = MAX_DT;

   if (!pose) return;

   self->mInternalTimer = 0.0f;

   // Guard: bail if bone_string_1 not found in pose
   if (!ht_find(pose->tableData, HT_TABLE_SIZE, HASH_BONE_STRING_1))
      return;

   int numT = self->mNumTentacles;
   int bpt = self->mBonesPerTentacle;

   // Bone lookup: for each tent/bone, look up the bone matrix pointer in the pose hash table
   PblMat4* bonePtrs[TOTAL_BONES]; // 45 entries max
   for (int t = 0; t < numT; t++) {
      for (int b = 0; b < bpt; b++) {
         uint32_t key = g_bone_hashes[bpt * t + b];
         bonePtrs[t * BONE_PTR_STRIDE + b] = (PblMat4*)ht_find(pose->tableData, HT_TABLE_SIZE, key);
      }
   }

   // mFirstUpdate: init tPos/oldPos from bone matrices' translation
   if (self->mFirstUpdate) {
      for (int t = 0; t < numT; t++) {
         for (int b = 0; b < bpt; b++) {
            int posIdx = t * TENT_STRIDE + b;
            PblMat4* bone = bonePtrs[t * BONE_PTR_STRIDE + b];
            if (bone) {
               self->tPos[posIdx] = bone->trans;
               self->oldPos[posIdx] = bone->trans;
            }
         }
      }
      self->mFirstUpdate = 0;
   }

   // Simulate if dt > 0
   if (dt > 0.0f) {
      UpdatePositions(self, dt, velocity, pose, parentMatrix, bonePtrs);
      EnforceCollisions(self, pose);
   }

   // Always update pose
   UpdatePose(self, pose, parentMatrix, bonePtrs, targetMatrices);
}

// ============================================================================
// 9. Simplified per-build address table + MODTOOLS/STEAM/GOG tables
// Only binary patches remain (pool size, bitfield, render stacks, setProperty).
// Constructor and DoTentacles are hooked via Detours — no code caves needed.
// ============================================================================

struct tentacle_addrs {
   // Executable identification
   uintptr_t id_file_offset;
   uint64_t  id_expected;

   // Detours hook targets (file offsets, add exe_base for VA)
   uintptr_t fn_constructor;
   uintptr_t fn_doTentacles;

   // Bone hash table VA (for verification only)
   uintptr_t bone_hash_table_va;

   // SetProperty tentacle limit: CMP EAX,4 → CMP EAX,9 (0 if not present)
   uintptr_t setProperty_limit;

   // sMemoryPool element size: 0x268 → 0x778
   uintptr_t memPool1;
   uintptr_t memPool2;
   uintptr_t memPool3; // 0 if not present

   // Render stack sizes
   struct render_patch {
      uintptr_t file_offset;
      uint32_t old_val;
      uint32_t new_val;
   };
   render_patch renderSoldier;
   render_patch renderAddon1;
   render_patch renderAddon2;

   // Bitfield widening: numTentacles 3-bit → 4-bit
   static const int NUM_BF_PATCHES = 17;
   struct bf_patch {
      uintptr_t file_offset;
      uint8_t   size; // 1 or 4 bytes
      uint32_t  old_val;
      uint32_t  new_val;
   };
   bf_patch bitfield[NUM_BF_PATCHES];
};

// clang-format off
static const tentacle_addrs MODTOOLS = {
   .id_file_offset = 0x62b59c,
   .id_expected    = 0x746163696c707041,

   .fn_constructor  = 0x16d090,
   .fn_doTentacles  = 0x16f4e0,
   .bone_hash_table_va = 0xa442f0,

   .setProperty_limit = 0x141cd5,

   .memPool1 = 0x2745cd,
   .memPool2 = 0x617181,
   .memPool3 = 0x1347ca,

   .renderSoldier = {0x135d98, 0xb84, 0x228c},
   .renderAddon1  = {0x16fe88, 0xa24, 0x1e6c},
   .renderAddon2  = {0x274898, 0xa34, 0x1e9c},

   .bitfield = {
      // EntitySoldierClass default ctor
      {0x13E0FA, 4, 0xFFFF0056, 0xFFFE0056},
      // EntitySoldierClass copy ctor
      {0x13F136, 4, 0x00000380, 0x00000780},
      {0x13F120, 4, 0x00003C00, 0x00007800},
      {0x13F14B, 4, 0x0000C000, 0x00018000},
      // SetProperty numTentacles write
      {0x141D13, 4, 0x00000380, 0x00000780},
      // SetProperty bonesPerTentacle write
      {0x13FC2E, 1, 0x0A, 0x0B},
      {0x13FC35, 4, 0x00003C00, 0x00007800},
      // SetProperty collisionType write
      {0x1420D9, 1, 0x0E, 0x0F},
      {0x1420E0, 4, 0x0000C000, 0x00018000},
      // EntitySoldier ctor — bitfield extraction
      {0x1347AD, 4, 0x00000380, 0x00000780},
      {0x1347EC, 1, 0x0E, 0x0F},
      {0x1347F5, 1, 0x0A, 0x0B},
      {0x1347FF, 1, 0x07, 0x0F},
      // DisplaySoldier::Setup — bitfield extraction
      {0x2745A4, 4, 0x00000380, 0x00000780},
      {0x2745EF, 1, 0x0E, 0x0F},
      {0x2745F8, 1, 0x0A, 0x0B},
      {0x274602, 1, 0x07, 0x0F},
   },
};

static const tentacle_addrs STEAM = {
   .id_file_offset = 0x39f834,
   .id_expected    = 0x746163696c707041,

   .fn_constructor  = 0x255770,
   .fn_doTentacles  = 0x2558f0,
   .bone_hash_table_va = 0x78b630,

   .setProperty_limit = 0,

   .memPool1 = 0x0df909,
   .memPool2 = 0x0069d1,
   .memPool3 = 0x08da5e,

   .renderSoldier = {0x0e23ed, 0xbac, 0x230c},
   .renderAddon1  = {0x043ca8, 0xa18, 0x1e48},
   .renderAddon2  = {0x08dcad, 0xa4c, 0x1eec},

   .bitfield = {
      {0x0f51a5, 4, 0xFFFF0056, 0xFFFE0056},
      {0x0f6004, 4, 0x00000380, 0x00000780},
      {0x0f5fef, 4, 0x00003C00, 0x00007800},
      {0x0f601a, 4, 0x0000C000, 0x00018000},
      {0x0fa2ee, 4, 0x00000380, 0x00000780},
      {0x0f84c3, 1, 0x0A, 0x0B},
      {0x0f84cb, 4, 0x00003C00, 0x00007800},
      {0x0fa613, 1, 0x0E, 0x0F},
      {0x0fa61b, 4, 0x0000C000, 0x00018000},
      {0x0df8ed, 4, 0x00000380, 0x00000780},
      {0x0df92f, 1, 0x0E, 0x0F},
      {0x0df935, 1, 0x0A, 0x0B},
      {0x0df940, 1, 0x07, 0x0F},
      {0x08da35, 4, 0x00000380, 0x00000780},
      {0x08da84, 1, 0x0E, 0x0F},
      {0x08da8a, 1, 0x0A, 0x0B},
      {0x08da95, 1, 0x07, 0x0F},
   },
};

static const tentacle_addrs GOG = {
   .id_file_offset = 0x3a0698,
   .id_expected    = 0x746163696c707041,

   .fn_constructor  = 0x256810,
   .fn_doTentacles  = 0x256990,
   .bone_hash_table_va = 0x78c5d0,

   .setProperty_limit = 0,

   .memPool1 = 0x0df909,
   .memPool2 = 0x0069d1,
   .memPool3 = 0x08da5e,

   .renderSoldier = {0x0e23ed, 0xbac, 0x230c},
   .renderAddon1  = {0x043c88, 0xa18, 0x1e48},
   .renderAddon2  = {0x08dcad, 0xa4c, 0x1eec},

   .bitfield = {
      {0x0f51a5, 4, 0xFFFF0056, 0xFFFE0056},
      {0x0f6004, 4, 0x00000380, 0x00000780},
      {0x0f5fef, 4, 0x00003C00, 0x00007800},
      {0x0f601a, 4, 0x0000C000, 0x00018000},
      {0x0fa2ee, 4, 0x00000380, 0x00000780},
      {0x0f84c3, 1, 0x0A, 0x0B},
      {0x0f84cb, 4, 0x00003C00, 0x00007800},
      {0x0fa613, 1, 0x0E, 0x0F},
      {0x0fa61b, 4, 0x0000C000, 0x00018000},
      {0x0df8ed, 4, 0x00000380, 0x00000780},
      {0x0df92f, 1, 0x0E, 0x0F},
      {0x0df935, 1, 0x0A, 0x0B},
      {0x0df940, 1, 0x07, 0x0F},
      {0x08da35, 4, 0x00000380, 0x00000780},
      {0x08da84, 1, 0x0E, 0x0F},
      {0x08da8a, 1, 0x0A, 0x0B},
      {0x08da95, 1, 0x07, 0x0F},
   },
};
// clang-format on

// ============================================================================
// 10. Binary patch application (pool/bitfield/render/setProperty)
// ============================================================================

static bool apply_binary_patches(uintptr_t exe_base, const tentacle_addrs& addrs, cfile& log)
{
   int total = 0;
   bool ok = true;

   // SetProperty tentacle limit: 4 → 9
   if (addrs.setProperty_limit != 0) {
      uintptr_t addr = addrs.setProperty_limit + exe_base;
      uint8_t* p = (uint8_t*)addr;
      if (*p == 0x04) {
         *p = 0x09;
         total++;
         log.printf("Tentacle:   SetProperty limit: 4 -> 9\n");
      }
      else {
         log.printf("Tentacle: SetProperty limit at %08x: expected 04, found %02x\n", addr, *p);
      }
   }

   // sMemoryPool element size: 0x268 → 0x778
   uintptr_t pool_sites[] = {
      addrs.memPool1 + exe_base,
      addrs.memPool2 + exe_base,
      addrs.memPool3 ? addrs.memPool3 + exe_base : 0,
   };
   for (int i = 0; i < 3; i++) {
      if (pool_sites[i] == 0) continue;
      uint32_t* p = (uint32_t*)pool_sites[i];
      if (*p == 0x268) {
         *p = 0x778;
         total++;
         log.printf("Tentacle:   MemoryPool site %d: 0x268 -> 0x778\n", i + 1);
      }
      else {
         log.printf("Tentacle: MemoryPool site %d at %08x: expected 268, found %x\n",
                    i + 1, pool_sites[i], *p);
         ok = false;
      }
   }

   // Render stack sizes
   const tentacle_addrs::render_patch renders[] = {
      addrs.renderSoldier, addrs.renderAddon1, addrs.renderAddon2
   };
   const char* render_names[] = {
      "EntitySoldierClass::Render", "AnimatedAddon::Render (1)", "AnimatedAddon::Render (2)"
   };
   for (int i = 0; i < 3; i++) {
      uintptr_t addr = renders[i].file_offset + exe_base;
      uint32_t* p = (uint32_t*)addr;
      if (*p == renders[i].old_val) {
         *p = renders[i].new_val;
         total++;
         log.printf("Tentacle:   %s: 0x%x -> 0x%x\n", render_names[i], renders[i].old_val, renders[i].new_val);
      }
      else {
         log.printf("Tentacle: %s at %08x: expected %x, found %x\n",
                    render_names[i], addr, renders[i].old_val, *p);
         ok = false;
      }
   }

   // Bitfield widening
   for (int i = 0; i < tentacle_addrs::NUM_BF_PATCHES; i++) {
      const auto& bp = addrs.bitfield[i];
      uintptr_t addr = bp.file_offset + exe_base;

      if (bp.size == 1) {
         uint8_t* p = (uint8_t*)addr;
         if (*p == (uint8_t)bp.old_val) {
            *p = (uint8_t)bp.new_val;
            total++;
         }
         else {
            log.printf("Tentacle: bitfield[%d] at %08x: expected %02x, found %02x\n",
                       i, addr, bp.old_val, *p);
         }
      }
      else {
         uint32_t* p = (uint32_t*)addr;
         if (*p == bp.old_val) {
            *p = bp.new_val;
            total++;
         }
         else {
            log.printf("Tentacle: bitfield[%d] at %08x: expected %08x, found %08x\n",
                       i, addr, bp.old_val, *p);
            ok = false;
         }
      }
   }
   log.printf("Tentacle:   Bitfield widening: %d patches\n", tentacle_addrs::NUM_BF_PATCHES);

   log.printf("Tentacle: binary patches applied: %d (ok=%d)\n", total, (int)ok);
   return ok;
}

// ============================================================================
// 11. Detours install/uninstall
// ============================================================================

static bool g_hooks_installed = false;

static bool install_hooks(uintptr_t exe_base, const tentacle_addrs& addrs, cfile& log)
{
   original_ctor = (fn_ctor)(addrs.fn_constructor + exe_base);
   original_doTentacles = (fn_doTentacles)(addrs.fn_doTentacles + exe_base);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_ctor, hooked_ctor);
   DetourAttach(&(PVOID&)original_doTentacles, hooked_doTentacles);
   LONG result = DetourTransactionCommit();

   if (result != NO_ERROR) {
      log.printf("Tentacle: Detours commit FAILED (%ld)\n", result);
      return false;
   }

   g_hooks_installed = true;
   log.printf("Tentacle: Detours hooks installed (ctor=%08x, doTentacles=%08x)\n",
              addrs.fn_constructor + exe_base, addrs.fn_doTentacles + exe_base);
   return true;
}

static void uninstall_hooks()
{
   if (!g_hooks_installed) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_ctor)
      DetourDetach(&(PVOID&)original_ctor, hooked_ctor);
   if (original_doTentacles)
      DetourDetach(&(PVOID&)original_doTentacles, hooked_doTentacles);
   DetourTransactionCommit();

   g_hooks_installed = false;
}

// ============================================================================
// 12. Public API
// ============================================================================

bool patch_tentacle_limit(uintptr_t exe_base)
{
   cfile log{"BF2GameExt.log", "a"};
   if (!log) return false;

   log.printf("\n--- Tentacle Bone Limit Patch (Detours rewrite) ---\n");

   static const tentacle_addrs* builds[] = { &MODTOOLS, &STEAM, &GOG };

   for (const tentacle_addrs* build : builds) {
      char* id_addr = (char*)(build->id_file_offset + exe_base);
      if (memcmp(id_addr, &build->id_expected, sizeof(build->id_expected)) != 0)
         continue;

      log.printf("Tentacle: identified build, applying patches\n");

      // Initialize bone hashes (needed by hooks)
      uintptr_t hash_table_runtime = build->bone_hash_table_va - 0x400000 + exe_base;
      if (!init_bone_hashes(hash_table_runtime, log)) {
         log.printf("Tentacle: bone hash verification FAILED\n");
         return false;
      }
      log.printf("Tentacle: bone hash table initialized (45 entries, verified against game)\n");

      // Apply binary patches (pool, bitfield, render, setProperty)
      if (!apply_binary_patches(exe_base, *build, log))
         return false;

      // Install Detours hooks (constructor + DoTentacles)
      if (!install_hooks(exe_base, *build, log))
         return false;

      return true;
   }

   log.printf("Tentacle: no matching build found, skipping\n");
   return true;
}

void unpatch_tentacle_limit()
{
   uninstall_hooks();
}
