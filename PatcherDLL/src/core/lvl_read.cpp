#include "pch.h"
#include "lvl_read.hpp"
#include "resolve.hpp"

// =============================================================================
// LoadUtil::ReadDataFile — modtools 0x004538b0 / steam 0x00579c30 / gog 0x0057a9a0
// =============================================================================
//   bool ReadDataFile(name, heapArg, hashes, hashCount, lvlNames, lvlCount);
//
// The calling conventions differ per build. On modtools it is __cdecl with six
// stack args, all zero for a plain whole-file read. On the release builds it is
// LTCG: the name arrives in ECX, four caller-cleaned stack args follow (the
// first of which the callee never reads), and it RETs 0. That needs the naked
// thunk below — declaring it __fastcall would make the compiler assume callee
// cleanup and leak 16 bytes of stack per call.
//
// This shim lives here, once, on purpose: it used to be duplicated per caller,
// and an ABI shim with more than one copy is an ABI shim that will eventually
// disagree with itself.

typedef bool (__cdecl* fn_read_data_file_mt_t)(
    const char* name, int a2, int a3, void* a4, unsigned a5, void* a6);

static uintptr_t s_read_data_file = 0;

static uintptr_t read_data_file_fn()
{
    static bool s_resolved = false;
    if (!s_resolved) {
        s_resolved = true;
        if (g_addr && g_addr->load_util_read_data_file)
            s_read_data_file = (uintptr_t)resolve(g_addr->load_util_read_data_file);
    }
    return s_read_data_file;
}

__declspec(naked) static bool __cdecl call_read_data_file_release(const char* /*name*/)
{
    __asm {
        push ebp
        mov  ebp, esp
        push 0                          // lvlNames
        push 0                          // lvlCount
        push 0                          // hashes
        push 0                          // (unused by the callee)
        mov  ecx, [ebp + 8]             // name
        call dword ptr [s_read_data_file]
        add  esp, 16                    // caller-cleaned
        mov  esp, ebp
        pop  ebp
        ret
    }
}

bool lvl_read_data_file(const char* name)
{
    if (!name || !*name) return false;
    if (!read_data_file_fn()) return false;

    if (g_build == GameBuild::Modtools)
        return ((fn_read_data_file_mt_t)s_read_data_file)(name, 0, 0, nullptr, 0, nullptr);

    return call_read_data_file_release(name);
}

// =============================================================================
// lvl_resolve_data_path — see the header for the accepted forms
// =============================================================================

typedef const char* (__cdecl* GetContentDirectory_t)();

static void set_reason(char* dst, size_t n, const char* fmt, ...)
{
    if (!dst || !n) return;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(dst, n, _TRUNCATE, fmt, ap);
    va_end(ap);
}

LvlPathStatus lvl_resolve_data_path(const char* path,
                                    char* outStem,   size_t stemSize,
                                    char* outFull,   size_t fullSize,
                                    char* outReason, size_t reasonSize)
{
    if (outStem   && stemSize)   outStem[0]   = '\0';
    if (outFull   && fullSize)   outFull[0]   = '\0';
    if (outReason && reasonSize) outReason[0] = '\0';

    if (!path || !*path) {
        set_reason(outReason, reasonSize, "empty path.");
        return LvlPathStatus::Empty;
    }

    const bool wantDC = (_strnicmp(path, "dc:", 3) == 0);

    char work[260];
    strncpy_s(work, sizeof(work), wantDC ? path + 3 : path, _TRUNCATE);

    const size_t len = strlen(work);
    if (len > 4 && _stricmp(work + len - 4, ".lvl") == 0)
        work[len - 4] = '\0';

    if (!work[0]) {
        set_reason(outReason, reasonSize, "empty path.");
        return LvlPathStatus::Empty;
    }

    char resolved[260];
    if (wantDC) {
        const char* dir = nullptr;
        if (g_addr->dlc_get_content_directory)
            dir = ((GetContentDirectory_t)resolve(g_addr->dlc_get_content_directory))();

        if (!dir || !*dir) {
            set_reason(outReason, reasonSize,
                       "no addon content is active, so the \"dc:\" prefix cannot "
                       "be resolved.");
            return LvlPathStatus::NoContentDir;
        }

        // GetContentDirectory returns an ABSOLUTE path on real installs, e.g.
        // "D:\...\GameData\AddOn\VTR". ReadDataFile can use that as-is because it
        // prepends nothing, but both consumers of this stem prepend
        // "data\_lvl_pc\". So make the addon directory relative to the working
        // directory (always GameData, the dir every "data\_lvl_pc\..." path is
        // already resolved against) and then climb back out with "..\..\".
        const char* rel = dir;
        char  cwd[MAX_PATH];
        DWORD cwdLen = GetCurrentDirectoryA(sizeof(cwd), cwd);

        if (cwdLen > 0 && cwdLen < sizeof(cwd) && _strnicmp(dir, cwd, cwdLen) == 0) {
            rel = dir + cwdLen;
            while (*rel == '\\' || *rel == '/') ++rel;
        } else if (dir[0] && dir[1] == ':') {
            set_reason(outReason, reasonSize,
                       "addon directory \"%s\" is outside the working directory "
                       "\"%s\", so it cannot be reached from the loading screen's "
                       "data path.", dir, cwd);
            return LvlPathStatus::OutsideWorkingDir;
        }

        _snprintf_s(resolved, sizeof(resolved), _TRUNCATE,
                    "..\\..\\%s\\Data\\_lvl_pc\\%s", rel, work);
    } else {
        strncpy_s(resolved, sizeof(resolved), work, _TRUNCATE);
    }

    // Verify against the exact name LoadUtil::MakeFullName will build. PblFile's
    // own Exists is just FindFirstFileA, so a plain attribute query matches it and
    // keeps this build-agnostic.
    char full[300];
    _snprintf_s(full, sizeof(full), _TRUNCATE, "data\\_lvl_pc\\%s.lvl", resolved);
    if (outFull && fullSize)
        strncpy_s(outFull, fullSize, full, _TRUNCATE);

    const DWORD attrs = GetFileAttributesA(full);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        set_reason(outReason, reasonSize, "\"%s\" not found.", full);
        return LvlPathStatus::NotFound;
    }

    if (outStem && stemSize)
        strncpy_s(outStem, stemSize, resolved, _TRUNCATE);
    return LvlPathStatus::Ok;
}
