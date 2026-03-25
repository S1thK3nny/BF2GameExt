#include "pch.h"
#include "class_limit.hpp"

static const uintptr_t unrelocated_base = 0x400000;

// All locations inside BF2_modtools that hold the hardcoded class count limit.
static const uintptr_t class_limit_addrs[] = {
   0x0068a5cf, // 83 FB 0A  — cmp ebx, 0A
   0x0068a5ef, // 6A 0A     — push 0A
   0x0068a5fc, // C7 44 24 30 0A 00 00 00  — mov [esp+30], 0A
   0x0068a6c9, // 83 FB 0A  — cmp ebx, 0A
   0x0068a6ce, // BF 0A 00 00 00  — mov edi, 0A
};

void patch_class_limit(uintptr_t exe_base, uint8_t new_limit)
{
   for (uintptr_t unrelocated_addr : class_limit_addrs) {
      uint8_t* target = (uint8_t*)((unrelocated_addr - unrelocated_base) + exe_base);
      *target = new_limit;
   }
}
