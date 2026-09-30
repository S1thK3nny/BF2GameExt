#pragma once

// PblVector3 derives from D3DXVECTOR3.
struct PblVector3 {
   float x, y, z;
};
static_assert(sizeof(PblVector3) == 0xC, "PblVector3");
