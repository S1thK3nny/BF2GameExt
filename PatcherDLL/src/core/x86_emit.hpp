#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// =============================================================================
// Tiny x86 encoders for the hand-placed patches: CALL/JMP rel32 at a site or at
// the end of a code cave, and turning a 6-byte Jcc rel32 into an unconditional
// JMP. Pure byte arithmetic, no memory protection: callers write inside the
// install RW window, into their own cave, or into a staging buffer they later
// hand to protected_write().
//
// Branch targets are taken as any pointer type, so a function can be passed as
// `&my_hook` without a cast.
// =============================================================================

namespace x86 {

constexpr uint8_t kCall = 0xE8; // CALL rel32
constexpr uint8_t kJmp  = 0xE9; // JMP rel32
constexpr uint8_t kNop  = 0x90;

// rel32 operand of a branch whose NEXT instruction starts at `next`.
template<class T>
int32_t rel32(const void* next, T* target)
{
   return (int32_t)((uintptr_t)target - (uintptr_t)next);
}

// Encode `opcode rel32` into `buf` as if the instruction lived at `at`, then
// fill the rest of `len` bytes with NOPs so the site keeps its instruction
// boundaries. `buf` is usually `at` itself; a staging buffer is fine as long
// as `at` is where the bytes will end up. `len` must be >= 5.
template<class T>
void encode_branch(uint8_t* buf, const void* at, uint8_t opcode, T* target, size_t len = 5)
{
   const int32_t rel = rel32((const uint8_t*)at + 5, target);
   buf[0] = opcode;
   memcpy(buf + 1, &rel, 4);
   if (len > 5) memset(buf + 5, kNop, len - 5);
}

// encode_branch in place.
template<class T>
void write_branch(uint8_t* at, uint8_t opcode, T* target, size_t len = 5)
{
   encode_branch(at, at, opcode, target, len);
}

// Append `JMP target` to a code cave at cave[o] and return the new length.
template<class T>
int emit_jmp(uint8_t* cave, int o, T* target)
{
   write_branch(cave + o, kJmp, target);
   return o + 5;
}

// Rewrite a 6-byte `0F 8x rel32` conditional jump as `E9 rel32` + NOP with the
// same destination: the JMP is one byte shorter, so its rel32 grows by one.
// `out` may alias `jcc`.
inline void jcc32_as_jmp(const uint8_t* jcc, uint8_t* out)
{
   int32_t rel;
   memcpy(&rel, jcc + 2, 4);
   rel += 1;
   out[0] = kJmp;
   memcpy(out + 1, &rel, 4);
   out[5] = kNop;
}

} // namespace x86
