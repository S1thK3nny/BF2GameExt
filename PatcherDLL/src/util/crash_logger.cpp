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
// LIMIT — this handler can NEVER see a __fastfail (`int 29h`).  That
// instruction is a kernel fail-fast: it bypasses vectored handlers, SEH and
// the unhandled-exception filter by design, so the process dies with no
// in-process handler running at all.  Only an attached debugger sees it.
// Shader Patch's d3d9.dll aborts this way (its CRT `abort()` at RVA 0x2a6321
// does `IsProcessorFeaturePresent(PF_FASTFAIL_AVAILABLE)` then
// `__fastfail(FAST_FAIL_FATAL_APP_EXIT)`), which is why an SP terminate shows
// up as a silent disappearance and not as a report here.  Do not "fix" that by
// widening the filter further — the filter is not what stops it.
//
// Note for anyone chasing one: that same `abort()` falls through to
// `RaiseException(STATUS_FATAL_APP_EXIT)` when bit 1 of SP's `_abort_behavior`
// global is clear, and THAT is catchable here.  Clearing the bit means writing
// into another module's CRT state, so it is a debugging measure, not something
// to ship.
//
// Output: BF2GameExt_crash.log beside the game executable, appended.
// Each report: exception code, faulting EIP as module+offset, access-violation
// details, registers, and a scan of the stack for return addresses into any
// loaded module (poor man's backtrace — includes false positives, but the
// module+offset values can be checked against disassembly).
// =============================================================================

static LONG s_reports = 0;          // cap so a crash loop can't flood the log
static constexpr LONG kMaxReports = 16;

// Resolve an address to "module.dll+0xOFFSET". Returns false if the address
// is not inside any loaded module.
//
// For the game executable it also prints the UNRELOCATED address
// (offset + 0x400000), because that is the form every address in
// game_addrs.hpp and every Ghidra database uses. Without it each report needs
// hand arithmetic against a base nobody recorded, which is exactly how a
// report ends up unreadable.
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

    const unsigned off = (unsigned)(addr - (uintptr_t)mod);
    if (mod == GetModuleHandleW(nullptr))
        _snprintf(out, outLen, "%s+0x%X (va %08X)", name, off, off + 0x400000u);
    else
        _snprintf(out, outLen, "%s+0x%X", name, off);
    out[outLen - 1] = '\0';
    return true;
}

// Is [addr, addr+size) committed and readable? Used to keep the frame walk
// from faulting inside the exception handler.
static bool mem_readable(uintptr_t addr, size_t size)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return addr >= (uintptr_t)mbi.BaseAddress && addr + size <= end;
}

// Write next to the game executable, NOT to the process working directory.
//
// This used to be a bare relative "BF2GameExt_crash.log", which silently put
// the report wherever the CWD happened to point — and since nothing in the
// engine guarantees the CWD stays at GameData, a missing report was
// indistinguishable from "no crash was caught". An absolute path built from the
// exe's own location cannot go missing.
static void append_log(const char* buf, size_t len)
{
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(GetModuleHandleW(nullptr), path, MAX_PATH))
        return;
    char* slash = nullptr;
    for (char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    if (!slash) return;
    slash[1] = '\0';
    strncat(path, "BF2GameExt_crash.log", MAX_PATH - strlen(path) - 1);

    HANDLE h = CreateFileA(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(h, buf, (DWORD)len, &written, nullptr);
    FlushFileBuffers(h);   // the process is about to die; don't rely on close
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

    // Report anything the NTSTATUS encoding marks as a hard error, rather than
    // matching a hand-written list of codes.
    //
    // The list this replaces held six EXCEPTION_* constants and therefore threw
    // away every fatal status that is not one of them — most importantly
    // STATUS_STACK_BUFFER_OVERRUN (0xC0000409), which is what __fastfail and
    // every /GS and CRT invalid-parameter abort raises. That is not a corner
    // case in this process: Shader Patch __fastfails by design when its shader
    // substitution is fought, so the one failure mode most likely to look like
    // "the game just vanished" was also the one guaranteed to log nothing.
    // Also missed: STATUS_HEAP_CORRUPTION (0xC0000374) and
    // STATUS_IN_PAGE_ERROR (0xC0000006).
    //
    //   severity bits 11 -> error   (code & 0xC0000000) == 0xC0000000
    //   customer bit set -> not an OS status; that is how C++ exceptions
    //                       (0xE06D7363) and vendor codes are tagged, and those
    //                       are routine first-chance traffic, not crashes.
    //
    // Breakpoints, single-steps and guard-page hits carry severity 10 or 01 and
    // are filtered out by the same test, so the previous exclusions still hold.
    if ((code & 0xC0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    if ((code & 0x20000000u) != 0)           return EXCEPTION_CONTINUE_SEARCH;

    if (InterlockedIncrement(&s_reports) > kMaxReports)
        return EXCEPTION_CONTINUE_SEARCH;

    // Static, not a 16 KB stack frame: on EXCEPTION_STACK_OVERFLOW there is by
    // definition no room left to build the report in, so the handler used to
    // re-fault and the one crash that most needs a trace produced none.
    static char buf[16384];
    size_t len = 0;
    CONTEXT* c = xp->ContextRecord;

    char loc[MAX_PATH + 32];
    if (!format_module_offset(c->Eip, loc, sizeof(loc)))
        _snprintf(loc, sizeof(loc), "<no module>");

    const char* codeName = "";
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      codeName = " ACCESS_VIOLATION";     break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:   codeName = " ILLEGAL_INSTRUCTION";  break;
    case EXCEPTION_PRIV_INSTRUCTION:      codeName = " PRIV_INSTRUCTION";     break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    codeName = " INT_DIVIDE_BY_ZERO";   break;
    case EXCEPTION_STACK_OVERFLOW:        codeName = " STACK_OVERFLOW";       break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: codeName = " ARRAY_BOUNDS";         break;
    case EXCEPTION_IN_PAGE_ERROR:         codeName = " IN_PAGE_ERROR";        break;
    case 0xC0000409:                      codeName = " STACK_BUFFER_OVERRUN (__fastfail)"; break;
    case 0xC0000374:                      codeName = " HEAP_CORRUPTION";      break;
    default: break;
    }

    // Local wall-clock stamp.  The log is opened for APPEND and survives across
    // sessions, so without this there is no telling today's crash from one three
    // weeks old -- which is the first question asked of every report.  GetLocalTime
    // is safe here: a kernel call with no allocation and no CRT locks, so it cannot
    // deadlock in a handler that may have interrupted the CRT mid-operation.
    SYSTEMTIME st;
    GetLocalTime(&st);
    appendf(buf, sizeof(buf), len,
            "=== EXCEPTION %08X%s at EIP=%08X (%s)  [%04u-%02u-%02u %02u:%02u:%02u.%03u]\r\n",
            code, codeName, (unsigned)c->Eip, loc,
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds);

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

    // Frame walk via the EBP chain. This is the call stack proper — the raw
    // scan below is only a fallback, because it cannot tell a live return
    // address from a dead one left over by an earlier call at the same depth.
    // Frame-pointer-omitted frames end the walk early; that is still far more
    // than the scan alone tells you.
    {
        appendf(buf, sizeof(buf), len, "    frames (EBP chain):\r\n");
        uintptr_t ebp = c->Ebp;
        int shown = 0;
        for (int i = 0; i < 40 && shown < 32; ++i) {
            if (!mem_readable(ebp, 8)) break;
            const uintptr_t next = ((const uintptr_t*)ebp)[0];
            const uintptr_t ret  = ((const uintptr_t*)ebp)[1];

            char sym[MAX_PATH + 48];
            if (ret >= 0x10000 && format_module_offset(ret, sym, sizeof(sym))) {
                appendf(buf, sizeof(buf), len, "      #%02d  %s\r\n", shown, sym);
                ++shown;
            }
            // The chain must ascend, or we are following garbage in a loop.
            if (next <= ebp) break;
            ebp = next;
        }
        if (shown == 0)
            appendf(buf, sizeof(buf), len, "      (no usable frame pointer)\r\n");
    }

    // Raw stack scan: any dword in the next 0x800 bytes that lands in a
    // module. Includes stale values from returned calls — treat as hints only.
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        uintptr_t esp = c->Esp;
        size_t scanBytes = 0x800;
        if (VirtualQuery((void*)esp, &mbi, sizeof(mbi)) &&
            (mbi.State == MEM_COMMIT) &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (esp + scanBytes > regionEnd) scanBytes = regionEnd - esp;
        } else {
            scanBytes = 0;
        }

        appendf(buf, sizeof(buf), len, "    stack scan:\r\n");
        const uintptr_t* sp = (const uintptr_t*)esp;
        int shown = 0;
        for (size_t i = 0; i < scanBytes / 4 && shown < 40; ++i) {
            uintptr_t v = sp[i];
            char sym[MAX_PATH + 48];
            if (v >= 0x10000 && format_module_offset(v, sym, sizeof(sym))) {
                appendf(buf, sizeof(buf), len, "      [esp+%03X] %s\r\n",
                        (unsigned)(i * 4), sym);
                ++shown;
            }
        }
    }

    append_log(buf, len);
    return EXCEPTION_CONTINUE_SEARCH;
}

void crash_logger_install()
{
    AddVectoredExceptionHandler(1 /*first*/, crash_veh);
}
