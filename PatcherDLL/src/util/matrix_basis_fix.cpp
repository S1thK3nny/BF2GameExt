#include "pch.h"
#include "matrix_basis_fix.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"
#include "util/install_log.hpp"
#include "game/PebbleFL/Common/PblMatrix.h"
#include "game/PebbleFL/Common/PblVector.h"

#include <detours.h>
#include <math.h>

// See matrix_basis_fix.hpp.

// Plain cdecl on every build. It calls D3DXVec3Normalize, so LTCG callers
// cannot keep anything live in volatile registers across it.
typedef PblMatrix*(__cdecl* fn_FromFwdUp_t)(PblMatrix* out, const PblVector3* fwd,
                                            const PblVector3* up, const PblVector3* pos);

static fn_FromFwdUp_t g_origFromFwdUp = nullptr;

// sin^2 of the fwd/up angle at or below which the right axis is unusable.
// Steam's frozen -90 aimer measured 1.42e-16 and broke; the debug build's
// 1.7e-14 for the same ODF worked.
static constexpr float kParallelSinSq = 1e-14f;

static PblMatrix* __cdecl hooked_FromFwdUp(PblMatrix* out, const PblVector3* fwd,
                                           const PblVector3* up, const PblVector3* pos)
{
   // Same cross the engine takes for the right axis.
   const float cx = up->y * fwd->z - up->z * fwd->y;
   const float cy = up->z * fwd->x - up->x * fwd->z;
   const float cz = up->x * fwd->y - up->y * fwd->x;
   const float crossSq = cx * cx + cy * cy + cz * cz;
   const float fwdSq   = fwd->x * fwd->x + fwd->y * fwd->y + fwd->z * fwd->z;
   const float upSq    = up->x * up->x + up->y * up->y + up->z * up->z;

   if (fwdSq == 0.0f || crossSq > kParallelSinSq * fwdSq * upSq)
      return g_origFromFwdUp(out, fwd, up, pos);

   const float ax = fabsf(fwd->x), ay = fabsf(fwd->y), az = fabsf(fwd->z);
   PblVector3 alt = { 0.0f, 0.0f, 0.0f };
   if (ax <= ay && ax <= az)  alt.x = 1.0f;
   else if (ay <= az)         alt.y = 1.0f;
   else                       alt.z = 1.0f;

   return g_origFromFwdUp(out, fwd, &alt, pos);
}

void matrix_basis_fix_install(uintptr_t exe_base)
{
   if (g_addr->pbl_matrix_from_fwd_up == 0) return;

   g_origFromFwdUp = reinterpret_cast<fn_FromFwdUp_t>(resolve(exe_base, g_addr->pbl_matrix_from_fwd_up));

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   const LONG rd = DetourAttach(reinterpret_cast<PVOID*>(&g_origFromFwdUp), hooked_FromFwdUp);
   const LONG rc = DetourTransactionCommit();

   if (rd != NO_ERROR || rc != NO_ERROR) {
      g_origFromFwdUp = nullptr;
      install_log("[MatrixBasisFix] detour failed (%ld/%ld)", rd, rc);
      return;
   }
   install_log("[MatrixBasisFix] installed (basis builder %08X)",
               (unsigned)g_addr->pbl_matrix_from_fwd_up);
}

void matrix_basis_fix_uninstall()
{
   if (!g_origFromFwdUp) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourDetach(reinterpret_cast<PVOID*>(&g_origFromFwdUp), hooked_FromFwdUp);
   DetourTransactionCommit();
   g_origFromFwdUp = nullptr;
}
