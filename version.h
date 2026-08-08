#pragma once
//
// Single source of truth for the BF2GameExt version number.
//
// Consumed by:
//   BF2GameExt.rc            VERSIONINFO for BF2GameExt.exe (patcher)
//   PatcherDLL/Resource.rc   VERSIONINFO for BF2GameExt.dll
//   DInput8Proxy/Resource.rc VERSIONINFO for dinput8.dll
//   PatcherDLL/src/lua/...   GameExt.version, the string mission scripts read
//   generate_ini.py          header comment stamped into dist/BF2GameExt.ini
//
// To cut a release, change the three numbers below and nothing else. This file
// is plain ASCII on purpose: the .rc files are UTF-16LE, but rc.exe detects
// the encoding of each included file separately, so an ASCII header is fine.
//
// Bump rules:
//   MAJOR   breaking change to the Lua API, ODF properties or INI keys
//   MINOR   new features, backwards compatible
//   PATCH   fixes only
//

#define GAMEEXT_VERSION_MAJOR 1
#define GAMEEXT_VERSION_MINOR 0
#define GAMEEXT_VERSION_PATCH 0

// Indirection so the argument is macro-expanded before stringification.
#define GAMEEXT_STRINGIFY_(x) #x
#define GAMEEXT_STRINGIFY(x)  GAMEEXT_STRINGIFY_(x)

// "1.0.0" - the only version string anyone sees. Used for GameExt.version and
// for the FileVersion / ProductVersion text in the VERSIONINFO blocks.
#define GAMEEXT_VERSION_STRING          \
   GAMEEXT_STRINGIFY(GAMEEXT_VERSION_MAJOR) "." \
   GAMEEXT_STRINGIFY(GAMEEXT_VERSION_MINOR) "." \
   GAMEEXT_STRINGIFY(GAMEEXT_VERSION_PATCH)

// Comma form for the FILEVERSION / PRODUCTVERSION statements. Win32 hardcodes
// these as four 16-bit fields, so the trailing 0 is a format requirement, not
// a version component. It is not tracked and never appears in a version string.
#define GAMEEXT_VERSION_COMMA  \
   GAMEEXT_VERSION_MAJOR,      \
   GAMEEXT_VERSION_MINOR,      \
   GAMEEXT_VERSION_PATCH,      \
   0

// Copyright line shared by every VERSIONINFO block.
#define GAMEEXT_LEGAL_COPYRIGHT "Copyright (C) 2025-2026 S1thK3nny and BF2GameExt contributors"
