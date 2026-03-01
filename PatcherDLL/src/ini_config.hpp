#pragma once

#include <windows.h>

// Thin wrapper around GetPrivateProfileIntA for reading BF2GameExt.ini.
// If ini_path is null or the file doesn't exist, all queries return their defaults,
// so the mod behaves identically to the non-INI build.

struct ini_config {
   const char* path;

   explicit ini_config(const char* ini_path) : path(ini_path) {}

   bool get_bool(const char* section, const char* key, bool default_val) const
   {
      if (!path || !path[0]) return default_val;
      return GetPrivateProfileIntA(section, key, default_val ? 1 : 0, path) != 0;
   }

   int get_int(const char* section, const char* key, int default_val) const
   {
      if (!path || !path[0]) return default_val;
      return GetPrivateProfileIntA(section, key, default_val, path);
   }
};
