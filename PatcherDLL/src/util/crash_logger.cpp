#include "pch.h"
#include "crash_logger.hpp"

#pragma warning(disable: 4996) // _snprintf/_vsnprintf deprecation (matches anim_bank_append.cpp)

#include <stdio.h>

// =============================================================================
// Crash logger — vectored exception handler (VEH).
//
// VEH runs BEFORE any frame-based SEH, so we see the true first-chance
// exception even when the game's top-level handler (or a __try in our own
// hooks) later swallows it.  We only LOG and return CONTINUE_SEARCH — normal
// exception dispatch is unaffected.
//
// Output: BF2GameExt_crash.log in the working directory (game dir), appended.
// Each report: exception code, faulting EIP as module+offset, access-violation
// details, registers, and a scan of the stack for return addresses into any
// loaded module (poor man's backtrace — includes false positives, but the
// module+offset values can be checked against disassembly).
// =============================================================================

static LONG s_reports = 0;          // cap so a crash loop can't flood the log
static constexpr LONG kMaxReports = 16;

// Resolve an address to "module.dll+0xOFFSET". Returns false if the address
// is not inside any loaded module.
static bool format_module_offset(uintptr_t addr, char* out, size_t outLen)
{
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)addr, &mod) || !mod)
        return false;

    char path[MAX_PATH] = {};
    GetModuleFileNameA(mod, path, MAX_PATH);
    const char* name = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') name = p + 1;

    _snprintf(out, outLen, "%s+0x%X", name, (unsigned)(addr - (uintptr_t)mod));
    out[outLen - 1] = '\0';
    return true;
}

static void append_log(const char* buf, size_t len)
{
    HANDLE h = CreateFileA("BF2GameExt_crash.log", FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(h, buf, (DWORD)len, &written, nullptr);
    CloseHandle(h);
}

// _snprintf-append that never lets len run past the buffer.
static void appendf(char* buf, size_t cap, size_t& len, const char* fmt, ...)
{
    if (len >= cap - 1) return;
    va_list va;
    va_start(va, fmt);
    int n = _vsnprintf(buf + len, cap - len - 1, fmt, va);
    va_end(va);
    if (n < 0) { len = cap - 1; buf[len] = '\0'; return; }
    len += (size_t)n;
}

// Expected-fault suppression (see expected_fault_scope in the header).
static volatile LONG  s_expectDepth  = 0;
static volatile DWORD s_expectThread = 0;

void crash_logger_begin_expected_fault()
{
    s_expectThread = GetCurrentThreadId();
    InterlockedIncrement(&s_expectDepth);
}

void crash_logger_end_expected_fault()
{
    if (InterlockedDecrement(&s_expectDepth) <= 0)
        s_expectThread = 0;
}

static LONG CALLBACK crash_veh(PEXCEPTION_POINTERS xp)
{
    const DWORD code = xp->ExceptionRecord->ExceptionCode;

    // A fault inside a guarded probe is expected and already handled by the
    // __try around it — don't report a crash the game is going to survive.
    if (s_expectDepth > 0 && s_expectThread == GetCurrentThreadId())
        return EXCEPTION_CONTINUE_SEARCH;

    // Only fatal-looking codes. Skip C++ exceptions (0xE06D7363), debug
    // breaks, guard pages and other routine first-chance noise.
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (InterlockedIncrement(&s_reports) > kMaxReports)
        return EXCEPTION_CONTINUE_SEARCH;

    char buf[4096];
    size_t len = 0;
    CONTEXT* c = xp->ContextRecord;

    char loc[MAX_PATH + 32];
    if (!format_module_offset(c->Eip, loc, sizeof(loc)))
        _snprintf(loc, sizeof(loc), "<no module>");

    appendf(buf, sizeof(buf), len, "=== EXCEPTION %08X at EIP=%08X (%s)\r\n",
            code, (unsigned)c->Eip, loc);

    if (code == EXCEPTION_ACCESS_VIOLATION &&
        xp->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR rw = xp->ExceptionRecord->ExceptionInformation[0];
        appendf(buf, sizeof(buf), len, "    AV: %s addr %08X\r\n",
                rw == 0 ? "READ" : (rw == 1 ? "WRITE" : "EXEC"),
                (unsigned)xp->ExceptionRecord->ExceptionInformation[1]);
    }

    appendf(buf, sizeof(buf), len,
            "    EAX=%08X EBX=%08X ECX=%08X EDX=%08X\r\n"
            "    ESI=%08X EDI=%08X EBP=%08X ESP=%08X\r\n",
            (unsigned)c->Eax, (unsigned)c->Ebx, (unsigned)c->Ecx, (unsigned)c->Edx,
            (unsigned)c->Esi, (unsigned)c->Edi, (unsigned)c->Ebp, (unsigned)c->Esp);

    // Stack scan: any dword in the next 0x400 bytes that lands in a module.
    // Clamp the scan to the committed, readable region containing ESP.
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        uintptr_t esp = c->Esp;
        size_t scanBytes = 0x400;
        if (VirtualQuery((void*)esp, &mbi, sizeof(mbi)) &&
            (mbi.State == MEM_COMMIT) &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (esp + scanBytes > regionEnd) scanBytes = regionEnd - esp;
        } else {
            scanBytes = 0;
        }

        appendf(buf, sizeof(buf), len, "    stack:");
        const uintptr_t* sp = (const uintptr_t*)esp;
        int shown = 0;
        for (size_t i = 0; i < scanBytes / 4 && shown < 12; ++i) {
            uintptr_t v = sp[i];
            char sym[MAX_PATH + 32];
            if (v >= 0x10000 && format_module_offset(v, sym, sizeof(sym))) {
                appendf(buf, sizeof(buf), len, " [esp+%X]=%s", (unsigned)(i * 4), sym);
                ++shown;
            }
        }
        appendf(buf, sizeof(buf), len, "\r\n");
    }

    append_log(buf, len);
    return EXCEPTION_CONTINUE_SEARCH;
}

void crash_logger_install()
{
    AddVectoredExceptionHandler(1 /*first*/, crash_veh);
}
