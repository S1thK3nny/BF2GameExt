#pragma once

struct PblAngle {
   float mCos, mSin;
};
static_assert(sizeof(PblAngle) == 0x8, "PblAngle");
