#include "pch.h"
#include "particle_density.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <cstring>

// See particle_density.hpp for what this dial does and why the LOD numerator is
// repointed rather than edited.

int g_particleDensity = 0;

// ---------------------------------------------------------------------------
// Bucket masks
//
// A row is "how many of the four buckets survive at this LOD".  The bucket sets
// match the engine's own choices, so level 0 reproduces the stock table exactly
// and the restore path is a straight byte compare.
// ---------------------------------------------------------------------------

static constexpr uint32_t kRow4 = 0x01010101; // {0,1,2,3}
static constexpr uint32_t kRow3 = 0x00010101; // {0,1,2}
static constexpr uint32_t kRow2 = 0x00010001; // {0,2}
static constexpr uint32_t kRow1 = 0x00000001; // {0}

// mask rows per LOD level, indexed [density][lod]
static constexpr uint32_t kMaskRows[3][4] = {
   {kRow4, kRow3, kRow2, kRow1}, // 0 stock:    4/3/2/1 buckets
   {kRow4, kRow4, kRow3, kRow2}, // 1 balanced: one step softer
   {kRow4, kRow4, kRow4, kRow4}, // 2 maximum:  nothing culled
};

// LOD curve numerator.  Stock is 4.0; halving it doubles the distance at which
// each LOD level begins.  Level 2 leaves it stock — with nothing masked, moving
// the curve changes only the fade factor, and the fades are all cleared anyway.
static constexpr float kNumerator[3] = {4.0f, 2.0f, 4.0f};

// Per-emitter lifetime budget.  0 means "leave the engine's clamp alone".
static constexpr uint32_t kMaxParticles[3] = {0, 1024, 1024};

// The float the repointed operand reads.  Must outlive the process; a DLL
// global does.
static float s_lodNumerator = 4.0f;

// ---------------------------------------------------------------------------
// Saved originals for uninstall
// ---------------------------------------------------------------------------
static bool     s_installed      = false;
static uint8_t  s_origTables[32] = {};
static uint8_t* s_tableAddr      = nullptr;
static uint32_t s_origNumOperand = 0;
static uint8_t* s_numOperandAddr = nullptr;
static uint32_t s_origMax1 = 0, s_origMax2 = 0;
static uint8_t *s_max1Addr = nullptr, *s_max2Addr = nullptr;
static bool     s_maxIs16 = false;

// fade[L][b] = mask[L][b] && !mask[L+1][b]; the last LOD culls nothing further.
static uint32_t fade_row(uint32_t maskRow, uint32_t nextRow)
{
   return maskRow & ~nextRow;
}

void particle_density_install(uintptr_t exe_base)
{
   const int level = g_particleDensity;
   if (level <= 0) return;
   if (level > 2) return;
   if (g_build == GameBuild::Unknown) return;
   if (g_addr->lod_mask_table == 0) return;

   // --- LOD bucket masks + the fade table generated from them ---------------
   auto* table = reinterpret_cast<uint8_t*>(resolve(exe_base, g_addr->lod_mask_table));
   std::memcpy(s_origTables, table, sizeof(s_origTables));
   s_tableAddr = table;

   auto* mask = reinterpret_cast<uint32_t*>(table);
   auto* fade = reinterpret_cast<uint32_t*>(table + 0x10);
   for (int lod = 0; lod < 4; ++lod) {
      const uint32_t row  = kMaskRows[level][lod];
      const uint32_t next = (lod < 3) ? kMaskRows[level][lod + 1] : row;
      mask[lod] = row;
      fade[lod] = fade_row(row, next);
   }

   // --- LOD curve: repoint the numerator read at our own float --------------
   if (g_addr->lod_numerator_operand != 0) {
      s_lodNumerator = kNumerator[level];

      auto* operand = reinterpret_cast<uint8_t*>(resolve(exe_base, g_addr->lod_numerator_operand));
      std::memcpy(&s_origNumOperand, operand, sizeof(s_origNumOperand));

      // Sanity: the operand must currently point at a float reading 4.0, or
      // this is not the instruction we think it is.
      const float* stock = reinterpret_cast<const float*>(
         resolve(exe_base, s_origNumOperand));
      if (*stock == 4.0f) {
         const uint32_t ours = (uint32_t)(uintptr_t)&s_lodNumerator;
         std::memcpy(operand, &ours, sizeof(ours));
         s_numOperandAddr = operand;
      } else {
         get_gamelog()("[ParticleDensity] LOD numerator at %p reads %f, expected 4.0 -- "
                       "leaving the distance curve stock\n", (void*)stock, (double)*stock);
      }
   }

   // --- Per-emitter lifetime budget ----------------------------------------
   if (kMaxParticles[level] != 0 && g_addr->emitter_max_particles_op1 != 0) {
      s_maxIs16 = (g_build == GameBuild::Modtools); // imm16 there, imm32 on retail
      const uint32_t want = kMaxParticles[level];
      const size_t   w    = s_maxIs16 ? 2 : 4;

      auto* op1 = reinterpret_cast<uint8_t*>(resolve(exe_base, g_addr->emitter_max_particles_op1));
      std::memcpy(&s_origMax1, op1, w);
      if (s_origMax1 == 0x80) {
         std::memcpy(op1, &want, w);
         s_max1Addr = op1;
      }

      if (g_addr->emitter_max_particles_op2 != 0) {
         auto* op2 = reinterpret_cast<uint8_t*>(resolve(exe_base, g_addr->emitter_max_particles_op2));
         std::memcpy(&s_origMax2, op2, w);
         if (s_origMax2 == 0x80) {
            std::memcpy(op2, &want, w);
            s_max2Addr = op2;
         }
      }
   }

   s_installed = true;
}

void particle_density_uninstall()
{
   if (!s_installed) return;

   // Sections are re-protected by now, so these cannot be plain stores.
   if (s_tableAddr) protected_write(s_tableAddr, s_origTables, sizeof(s_origTables));
   if (s_numOperandAddr) protected_write(s_numOperandAddr, &s_origNumOperand, sizeof(s_origNumOperand));

   const size_t w = s_maxIs16 ? 2 : 4;
   if (s_max1Addr) protected_write(s_max1Addr, &s_origMax1, w);
   if (s_max2Addr) protected_write(s_max2Addr, &s_origMax2, w);

   s_tableAddr = s_numOperandAddr = s_max1Addr = s_max2Addr = nullptr;
   s_installed = false;
}
