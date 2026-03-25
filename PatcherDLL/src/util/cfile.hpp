#pragma once

#include <stdio.h>
#include <stdarg.h>

struct cfile {
   cfile(const char* filename, const char* mode);

   ~cfile();

   void printf(char const* const format, ...) const;
   void vprintf(char const* const format, va_list args) const;

   explicit operator bool() const noexcept;

private:
   FILE* file = nullptr;
};