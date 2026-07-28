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
