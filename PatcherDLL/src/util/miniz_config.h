#pragma once

// miniz with only the zlib-style deflate/inflate. The prefixed mz_ names stay,
// so nothing clashes with a zlib elsewhere. Path is relative so tests resolve it too.
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../../../third_party/miniz/miniz.h"
