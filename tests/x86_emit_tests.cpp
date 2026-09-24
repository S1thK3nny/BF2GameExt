// Standalone tests for core/x86_emit.hpp: every encoder must produce the same
// bytes as the hand-written arithmetic it replaced.

#include "../PatcherDLL/src/core/x86_emit.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

static int32_t read_rel(const uint8_t* p)
{
   int32_t r;
   memcpy(&r, p, 4);
   return r;
}

int main()
{
   uint8_t buf[64];
   uint8_t* site = buf + 8;
   const uint8_t* fwd = buf + 40;
   const uint8_t* back = buf + 2;

   // In place, forward and backward, against the old spelling
   // (int32_t)((uintptr_t)target - ((uintptr_t)site + 5)).
   for (const uint8_t* target : {fwd, back}) {
      memset(buf, 0xCC, sizeof(buf));
      x86::write_branch(site, x86::kJmp, target);
      assert(site[0] == 0xE9);
      assert(read_rel(site + 1) == (int32_t)((uintptr_t)target - ((uintptr_t)site + 5)));
      assert(site + 5 + read_rel(site + 1) == target);
      assert(site[5] == 0xCC); // len 5 writes exactly five bytes
   }

   // NOP padding.
   memset(buf, 0xCC, sizeof(buf));
   x86::write_branch(site, x86::kCall, fwd, 9);
   assert(site[0] == 0xE8);
   for (int i = 5; i < 9; ++i) assert(site[i] == 0x90);
   assert(site[9] == 0xCC);

   // Staging buffer: rel32 is relative to where the bytes will live, not the buffer.
   uint8_t staged[12];
   x86::encode_branch(staged, site, x86::kJmp, fwd, sizeof(staged));
   assert(read_rel(staged + 1) == (int32_t)(fwd - (site + 5)));
   assert(staged[11] == 0x90);

   // Far target: a DLL function relative to an exe site wraps through 32 bits.
   const void* far_target = (const void*)(uintptr_t)0x10001000u;
   const uint8_t* exe_site = (const uint8_t*)(uintptr_t)0x00515E39u;
   x86::encode_branch(staged, exe_site, x86::kJmp, far_target);
   assert((uint32_t)read_rel(staged + 1) == 0x10001000u - (0x00515E39u + 5));

   // Cave append, against cave[o++]=0xE9; *(int32_t*)(cave+o)=target-(cave+o+4); o+=4.
   memset(buf, 0xCC, sizeof(buf));
   int o = x86::emit_jmp(buf, 3, fwd);
   assert(o == 8 && buf[3] == 0xE9);
   assert(read_rel(buf + 4) == (int32_t)(fwd - (buf + 4 + 4)));

   // Jcc rel32 -> JMP rel32 + NOP, same destination, in place.
   uint8_t jcc[6] = {0x0F, 0x8E, 0, 0, 0, 0};
   const int32_t jrel = -0x1234;
   memcpy(jcc + 2, &jrel, 4);
   const uintptr_t dest = (uintptr_t)jcc + 6 + jrel;
   x86::jcc32_as_jmp(jcc, jcc);
   assert(jcc[0] == 0xE9 && jcc[5] == 0x90);
   assert((uintptr_t)jcc + 5 + read_rel(jcc + 1) == dest);

   // Function pointers work as targets without a cast.
   x86::write_branch(site, x86::kCall, &read_rel);
   assert(site + 5 + read_rel(site + 1) == (const uint8_t*)&read_rel);

   std::puts("x86 emit tests passed.");
   return 0;
}
