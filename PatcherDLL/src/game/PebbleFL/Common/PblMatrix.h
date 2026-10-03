#pragma once

// PblMatrix derives from D3DXMATRIXA16: row major, rows are right, up,
// forward, position.
struct alignas(16) PblMatrix {
   float _11, _12, _13, _14;
   float _21, _22, _23, _24;
   float _31, _32, _33, _34;
   float _41, _42, _43, _44;
};
static_assert(sizeof(PblMatrix) == 0x40, "PblMatrix");
