#pragma once

#include "slim_vector.hpp"

#include <stdint.h>

struct section_info {
   char* memory_start = nullptr;

   uint32_t file_start = 0;
   uint32_t file_end = 0;
};

bool apply_patches(const uintptr_t relocated_executable_base, const slim_vector<section_info>& sections);
