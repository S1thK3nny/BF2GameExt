#include "pch.h"

#include "cfile.hpp"

cfile::cfile(const char* filename, const char* mode)
{
   if (fopen_s(&file, filename, mode) != errno) file = nullptr;
}

cfile::~cfile()
{
   if (file) fclose(file);
}

void cfile::printf(char const* const format, ...) const
{
   if (not file) return;

   va_list args;
   va_start(args, format);

   vfprintf(file, format, args);
   fflush(file);

   va_end(args);
}

void cfile::vprintf(char const* const format, va_list args) const
{
   if (not file) return;
   vfprintf(file, format, args);
   fflush(file);
}

cfile::operator bool() const noexcept
{
   return file;
}
