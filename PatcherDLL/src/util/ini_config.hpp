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
      // GetPrivateProfileIntA doesn't handle "true"/"false" strings — read as
      // string first and check for those, then fall back to integer parsing.
      char buf[16];
      GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
      if (buf[0] == '\0') return default_val;
      if (_stricmp(buf, "true") == 0 || _stricmp(buf, "yes") == 0) return true;
      if (_stricmp(buf, "false") == 0 || _stricmp(buf, "no") == 0) return false;
      return atoi(buf) != 0;
   }

   int get_int(const char* section, const char* key, int default_val) const
   {
      if (!path || !path[0]) return default_val;
      return GetPrivateProfileIntA(section, key, default_val, path);
   }

   float get_float(const char* section, const char* key, float default_val) const
   {
      if (!path || !path[0]) return default_val;
      char buf[32];
      GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
      if (buf[0] == '\0') return default_val;
      return (float)atof(buf);
   }

   DWORD get_string(const char* section, const char* key, const char* default_val,
                     char* buf, DWORD buf_size) const
   {
      if (!path || !path[0]) {
         if (default_val && buf && buf_size > 0)
            strncpy_s(buf, buf_size, default_val, _TRUNCATE);
         else if (buf && buf_size > 0)
            buf[0] = '\0';
         return (buf && buf_size > 0) ? (DWORD)strlen(buf) : 0;
      }
      return GetPrivateProfileStringA(section, key, default_val ? default_val : "",
                                      buf, buf_size, path);
   }
};
