#include "pch.h"

#include "door_limit_patch.hpp"
#include "cfile.hpp"
#include <detours.h>

// ============================================================================
// Door Limit Increase
//
// PblHashTable<EntityDoor, 32> doubled to 64 buckets.
// Two global tables: msHostDoors (singleplayer/host) and msClientDoors (client).
// Active table selected by mspDoors pointer.
//
// Strategy:
//   - Relocate both tables + iterator to DLL static buffers
//   - Byte patches for bucket count (0x20 → 0x40, fits imm8)
//   - 32-bit patches for value displacement (0x84 → 0x104) and sentinel (0x104 → 0x204)
//   - Detours hooks on _Find/_Store to fix tableParam (0x40 → 0x80)
//     because PUSH 0x40 (imm8) can't become PUSH 0x80 (needs imm32, different size)
// ============================================================================

// ---------------------------------------------------------------------------
// Static buffers — zero-initialized (sentinel = 0, which works because
// valid EntityDoor pointers are never zero)
// ---------------------------------------------------------------------------
static const int NEW_BUCKETS = 64;
static const int NEW_TABLE_SIZE = 4 + NEW_BUCKETS * 4 * 2;  // 0x204
static const int NEW_TABLE_PARAM = NEW_BUCKETS * 2;          // 0x80

alignas(4) static char g_hostDoors[NEW_TABLE_SIZE + 4];      // +4 for sentinel
alignas(4) static char g_clientDoors[NEW_TABLE_SIZE + 4];
alignas(4) static char g_doorIter[8];                         // _table(4) + _pos(4)

// ---------------------------------------------------------------------------
// Per-build addresses
// ---------------------------------------------------------------------------
struct door_addrs {
    uintptr_t id_rva;
    uint64_t  id_expected;

    // PblHashTableCode functions (for Detours)
    uintptr_t find_func;
    uintptr_t store_func;

    // Original table addresses (for expected-value validation)
    uint32_t host_header;       // msHostDoors base
    uint32_t host_uiTable;      // msHostDoors + 4
    uint32_t client_header;     // msClientDoors base
    uint32_t client_uiTable;    // msClientDoors + 4
    uint32_t iter_table;        // msDoorIter._table
    uint32_t iter_pos;          // msDoorIter._pos

    // --- Patch sites: VA relocation (address, old_value) ---
    // EnterClient: sets mspDoors = &msClientDoors
    uintptr_t enter_client_val;       // 4-byte value to patch

    // LeaveClient: sets mspDoors = &msHostDoors
    uintptr_t leave_client_val;       // 4-byte value to patch

    // ResetDoorList
    uintptr_t reset_push_host_table;  // PUSH host_uiTable operand
    uintptr_t reset_mov_msp_host;     // MOV [mspDoors], host_header value
    uintptr_t reset_push_client_table;// PUSH client_uiTable operand
    uintptr_t reset_clr_host;         // MOV [host_header], 0 — address operand
    uintptr_t reset_clr_client;       // MOV [client_header], 0 — address operand
    uintptr_t reset_push_host_count;  // PUSH 0x20 for host _Clear
    uintptr_t reset_push_client_count;// PUSH 0x20 for client _Clear

    // Static initializers
    uintptr_t sinit_host_push_table;
    uintptr_t sinit_host_clr_header;
    uintptr_t sinit_host_push_count;
    uintptr_t sinit_client_push_table;
    uintptr_t sinit_client_clr_header;
    uintptr_t sinit_client_push_count;

    // mspDoors initial value in .data
    uintptr_t mspDoors_data;

    // BeginIter
    uintptr_t beginiter_cmp_count;     // CMP reg, 0x20
    uintptr_t beginiter_mov_table;     // MOV [iter._table], reg
    uintptr_t beginiter_mov_pos;       // MOV [iter._pos], reg

    // IncrementIter
    uintptr_t inciter_read_pos;        // MOV EAX, [iter._pos]
    uintptr_t inciter_cmp1_count;      // CMP EAX, 0x20 (first)
    uintptr_t inciter_write_pos1;      // MOV [iter._pos], EAX (first)
    uintptr_t inciter_read_table;      // MOV ECX, [iter._table]
    uintptr_t inciter_cmp2_count;      // CMP EAX, 0x20 (second)
    uintptr_t inciter_write_pos2;      // MOV [iter._pos], EAX (second)

    // GetCurrentIter
    uintptr_t getiter_read_table;      // MOV ECX, [iter._table]
    uintptr_t getiter_read_pos;        // MOV EAX, [iter._pos]
    uintptr_t getiter_val_disp;        // [reg + reg*4 + 0x84] displacement
    uintptr_t getiter_sentinel;        // [reg + 0x104] displacement

    // PblHashTable::Begin (internal, uses output struct not globals)
    uintptr_t begin_cmp_count;         // CMP EDX, 0x20

    // operator* (uses ECX struct, not globals)
    uintptr_t deref_val_disp;          // [reg + reg*4 + 0x84] displacement
};

static const door_addrs MODTOOLS = {
    .id_rva       = 0x62b59c,
    .id_expected  = 0x746163696c707041,  // "Applicat"
    .find_func    = 0x3E1A40,  // offset from base
    .store_func   = 0x3E1A90,

    .host_header   = 0xB7D580,
    .host_uiTable  = 0xB7D584,
    .client_header = 0xB7D688,
    .client_uiTable= 0xB7D68C,
    .iter_table    = 0xB7D78C,
    .iter_pos      = 0xB7D790,

    .enter_client_val       = 0x4D8B6F,
    .leave_client_val       = 0x4D8B8F,

    .reset_push_host_table  = 0x4D8FB3,
    .reset_mov_msp_host     = 0x4D8FBD,
    .reset_push_client_table= 0x4D8FC9,
    .reset_clr_host         = 0x4D8FCF,
    .reset_clr_client       = 0x4D8FE1,
    .reset_push_host_count  = 0x4D8FB1,
    .reset_push_client_count= 0x4D8FC7,

    .sinit_host_push_table  = 0xA16753,
    .sinit_host_clr_header  = 0xA16761,
    .sinit_host_push_count  = 0xA16751,
    .sinit_client_push_table= 0xA16773,
    .sinit_client_clr_header= 0xA16781,
    .sinit_client_push_count= 0xA16771,

    .mspDoors_data          = 0xACD6DC,

    .beginiter_cmp_count    = 0x4DA303,
    .beginiter_mov_table    = 0x4DA308,
    .beginiter_mov_pos      = 0x4DA30D,

    .inciter_read_pos       = 0x4D9021,
    .inciter_cmp1_count     = 0x4D9028,
    .inciter_write_pos1     = 0x4D902A,
    .inciter_read_table     = 0x4D9032,
    .inciter_cmp2_count     = 0x4D9041,
    .inciter_write_pos2     = 0x4D9043,

    .getiter_read_table     = 0x4DA322,
    .getiter_read_pos       = 0x4DA327,
    .getiter_val_disp       = 0x4DA32E,
    .getiter_sentinel       = 0x4DA33B,

    .begin_cmp_count        = 0x4D930E,
    .deref_val_disp         = 0x4D8E08,
};

static const door_addrs STEAM = {
    .id_rva       = 0x39f834,
    .id_expected  = 0x746163696c707041,
    .find_func    = 0x326E00,
    .store_func   = 0x326F60,

    .host_header   = 0x1EBB9A0,
    .host_uiTable  = 0x1EBB9A4,
    .client_header = 0x1EBBAA8,
    .client_uiTable= 0x1EBBAAC,
    .iter_table    = 0x1EBBBAC,
    .iter_pos      = 0x1EBBBB0,

    .enter_client_val       = 0x498E18,
    .leave_client_val       = 0x498E38,

    .reset_push_host_table  = 0x498DD3,
    .reset_mov_msp_host     = 0x498DDD,
    .reset_push_client_table= 0x498DE9,
    .reset_clr_host         = 0x498DEF,
    .reset_clr_client       = 0x498E01,
    .reset_push_host_count  = 0x498DD1,
    .reset_push_client_count= 0x498DE7,

    .sinit_host_push_table  = 0x4029E3,
    .sinit_host_clr_header  = 0x4029F1,
    .sinit_host_push_count  = 0x4029E1,
    .sinit_client_push_table= 0x402A03,
    .sinit_client_clr_header= 0x402A11,
    .sinit_client_push_count= 0x402A01,

    .mspDoors_data          = 0x7E6338,

    .beginiter_cmp_count    = 0x498E8B,
    .beginiter_mov_table    = 0x498E90,
    .beginiter_mov_pos      = 0x498E96,

    .inciter_read_pos       = 0x498ED1,
    .inciter_cmp1_count     = 0x498EDD,
    .inciter_write_pos1     = 0x498ED7,
    .inciter_read_table     = 0x498EE2,
    .inciter_cmp2_count     = 0x498EF5,
    .inciter_write_pos2     = 0x498EEF,

    .getiter_read_table     = 0x498EA7,  // Retail reads pos first, table second (opposite of modtools)
    .getiter_read_pos       = 0x498EA2,
    .getiter_val_disp       = 0x498EAE,
    .getiter_sentinel       = 0x498EBC,

    .begin_cmp_count        = 0x498E8B,  // Same function as beginiter on retail (merged)
    .deref_val_disp         = 0,         // operator* not a separate function on retail
};

static const door_addrs GOG = {
    .id_rva       = 0x3a0698,
    .id_expected  = 0x746163696c707041,
    .find_func    = 0x327ED0,
    .store_func   = 0x328030,

    .host_header   = 0x1EBCE58,
    .host_uiTable  = 0x1EBCE5C,
    .client_header = 0x1EBCF60,
    .client_uiTable= 0x1EBCF64,
    .iter_table    = 0x1EBD064,
    .iter_pos      = 0x1EBD068,

    .enter_client_val       = 0x498E18,
    .leave_client_val       = 0x498E38,

    .reset_push_host_table  = 0x498DD3,
    .reset_mov_msp_host     = 0x498DDD,
    .reset_push_client_table= 0x498DE9,
    .reset_clr_host         = 0x498DEF,
    .reset_clr_client       = 0x498E01,
    .reset_push_host_count  = 0x498DD1,
    .reset_push_client_count= 0x498DE7,

    .sinit_host_push_table  = 0x4029E3,
    .sinit_host_clr_header  = 0x4029F1,
    .sinit_host_push_count  = 0x4029E1,
    .sinit_client_push_table= 0x402A03,
    .sinit_client_clr_header= 0x402A11,
    .sinit_client_push_count= 0x402A01,

    .mspDoors_data          = 0x7E7338,

    .beginiter_cmp_count    = 0x498E8B,
    .beginiter_mov_table    = 0x498E90,
    .beginiter_mov_pos      = 0x498E96,

    .inciter_read_pos       = 0x498ED1,
    .inciter_cmp1_count     = 0x498EDD,
    .inciter_write_pos1     = 0x498ED7,
    .inciter_read_table     = 0x498EE2,
    .inciter_cmp2_count     = 0x498EF5,
    .inciter_write_pos2     = 0x498EEF,

    .getiter_read_table     = 0x498EA7,  // Retail reads pos first, table second (opposite of modtools)
    .getiter_read_pos       = 0x498EA2,
    .getiter_val_disp       = 0x498EAE,
    .getiter_sentinel       = 0x498EBC,

    .begin_cmp_count        = 0x498E8B,
    .deref_val_disp         = 0,
};

// ---------------------------------------------------------------------------
// Detours hooks on PblHashTableCode::_Find and _Store
// ---------------------------------------------------------------------------
typedef int  (__cdecl* fn_PblFind)(void* tableData, int tableParam, uint32_t hash);
typedef int  (__cdecl* fn_PblStore)(void* tableData, int tableParam, uint32_t key, uint32_t value);

static fn_PblFind  original_Find  = nullptr;
static fn_PblStore original_Store = nullptr;

static bool is_door_table(void* tableData)
{
    return tableData == (g_hostDoors + 4) || tableData == (g_clientDoors + 4);
}

static int __cdecl hooked_Find(void* tableData, int tableParam, uint32_t hash)
{
    if (is_door_table(tableData))
        tableParam = NEW_TABLE_PARAM;
    return original_Find(tableData, tableParam, hash);
}

static int __cdecl hooked_Store(void* tableData, int tableParam, uint32_t key, uint32_t value)
{
    if (is_door_table(tableData))
        tableParam = NEW_TABLE_PARAM;
    return original_Store(tableData, tableParam, key, value);
}

// ---------------------------------------------------------------------------
// Patch helpers — sections are already PAGE_READWRITE when called
// ---------------------------------------------------------------------------
static uintptr_t s_base = 0;

static uint8_t* va(uintptr_t unrelocated)
{
    return (uint8_t*)((unrelocated - 0x400000) + s_base);
}

static bool patch_u8(uintptr_t addr, uint8_t expected, uint8_t replacement, cfile& log)
{
    uint8_t* p = va(addr);
    if (*p != expected) {
        log.printf("[DoorLimit] FAIL u8 at %08X: got %02X expected %02X\n", addr, *p, expected);
        return false;
    }
    *p = replacement;
    return true;
}

static bool patch_u32(uintptr_t addr, uint32_t expected, uint32_t replacement, cfile& log)
{
    uint32_t* p = (uint32_t*)va(addr);
    if (*p != expected) {
        log.printf("[DoorLimit] FAIL u32 at %08X: got %08X expected %08X\n", addr, *p, expected);
        return false;
    }
    *p = replacement;
    return true;
}

// For VA-valued patches: expected is an unrelocated VA that needs resolving
static bool patch_va(uintptr_t addr, uint32_t expected_va, uint32_t replacement, cfile& log)
{
    uint32_t expected = (expected_va - 0x400000) + (uint32_t)s_base;
    return patch_u32(addr, expected, replacement, log);
}

// ---------------------------------------------------------------------------
// Main patch function
// ---------------------------------------------------------------------------
void patch_door_limit(uintptr_t exe_base)
{
    s_base = exe_base;
    cfile log("BF2GameExt.log", "a");

    // Identify build
    auto check_id = [&](const door_addrs& a) -> bool {
        uint64_t val = *(uint64_t*)(exe_base + a.id_rva);
        return val == a.id_expected;
    };

    const door_addrs* a = nullptr;
    const char* build = nullptr;

    if (check_id(MODTOOLS))    { a = &MODTOOLS; build = "modtools"; }
    else if (check_id(STEAM))  { a = &STEAM;    build = "Steam"; }
    else if (check_id(GOG))    { a = &GOG;      build = "GOG"; }
    else {
        log.printf("[DoorLimit] Build not recognized, skipping\n");
        return;
    }

    log.printf("[DoorLimit] Identified %s build\n", build);

    uint32_t new_host     = (uint32_t)&g_hostDoors[0];
    uint32_t new_host_tbl = (uint32_t)&g_hostDoors[4];
    uint32_t new_cli      = (uint32_t)&g_clientDoors[0];
    uint32_t new_cli_tbl  = (uint32_t)&g_clientDoors[4];
    uint32_t new_iter_tbl = (uint32_t)&g_doorIter[0];
    uint32_t new_iter_pos = (uint32_t)&g_doorIter[4];

    bool ok = true;

    // --- VA relocations ---
    // EnterClient / LeaveClient: value written to mspDoors
    ok &= patch_va(a->enter_client_val, a->client_header, new_cli, log);
    ok &= patch_va(a->leave_client_val, a->host_header,   new_host, log);

    // ResetDoorList
    ok &= patch_va(a->reset_push_host_table,   a->host_uiTable,   new_host_tbl, log);
    ok &= patch_va(a->reset_mov_msp_host,      a->host_header,    new_host,     log);
    ok &= patch_va(a->reset_push_client_table,  a->client_uiTable, new_cli_tbl,  log);
    ok &= patch_va(a->reset_clr_host,          a->host_header,    new_host,     log);
    ok &= patch_va(a->reset_clr_client,        a->client_header,  new_cli,      log);

    // Static initializers
    ok &= patch_va(a->sinit_host_push_table,   a->host_uiTable,   new_host_tbl, log);
    ok &= patch_va(a->sinit_host_clr_header,   a->host_header,    new_host,     log);
    ok &= patch_va(a->sinit_client_push_table,  a->client_uiTable, new_cli_tbl,  log);
    ok &= patch_va(a->sinit_client_clr_header,  a->client_header,  new_cli,      log);

    // mspDoors initial value in .data
    ok &= patch_va(a->mspDoors_data, a->host_header, new_host, log);

    // BeginIter: writes to msDoorIter globals
    ok &= patch_va(a->beginiter_mov_table, a->iter_table, new_iter_tbl, log);
    ok &= patch_va(a->beginiter_mov_pos,   a->iter_pos,   new_iter_pos, log);

    // IncrementIter: reads/writes msDoorIter globals
    ok &= patch_va(a->inciter_read_pos,   a->iter_pos,   new_iter_pos, log);
    ok &= patch_va(a->inciter_write_pos1, a->iter_pos,   new_iter_pos, log);
    ok &= patch_va(a->inciter_read_table, a->iter_table, new_iter_tbl, log);
    ok &= patch_va(a->inciter_write_pos2, a->iter_pos,   new_iter_pos, log);

    // GetCurrentIter: reads msDoorIter globals
    ok &= patch_va(a->getiter_read_table, a->iter_table, new_iter_tbl, log);
    ok &= patch_va(a->getiter_read_pos,   a->iter_pos,   new_iter_pos, log);

    // --- Bucket count patches: 0x20 → 0x40 ---
    ok &= patch_u8(a->reset_push_host_count,    0x20, 0x40, log);
    ok &= patch_u8(a->reset_push_client_count,   0x20, 0x40, log);
    ok &= patch_u8(a->sinit_host_push_count,     0x20, 0x40, log);
    ok &= patch_u8(a->sinit_client_push_count,    0x20, 0x40, log);
    ok &= patch_u8(a->beginiter_cmp_count,        0x20, 0x40, log);
    ok &= patch_u8(a->inciter_cmp1_count,         0x20, 0x40, log);
    ok &= patch_u8(a->inciter_cmp2_count,         0x20, 0x40, log);

    // begin_cmp_count may overlap with beginiter_cmp_count on retail (same function)
    if (a->begin_cmp_count != a->beginiter_cmp_count) {
        ok &= patch_u8(a->begin_cmp_count, 0x20, 0x40, log);
    }

    // --- Value displacement patches: 0x84 → 0x104 ---
    ok &= patch_u32(a->getiter_val_disp, 0x00000084, 0x00000104, log);
    if (a->deref_val_disp != 0) {
        ok &= patch_u32(a->deref_val_disp, 0x00000084, 0x00000104, log);
    }

    // --- Sentinel offset patch: 0x104 → 0x204 ---
    ok &= patch_u32(a->getiter_sentinel, 0x00000104, 0x00000204, log);

    if (!ok) {
        log.printf("[DoorLimit] Some patches failed! Door limit NOT increased.\n");
        return;
    }

    log.printf("[DoorLimit] Binary patches applied successfully\n");

    // --- Detours hooks on _Find and _Store ---
    original_Find  = (fn_PblFind)(exe_base + a->find_func);
    original_Store = (fn_PblStore)(exe_base + a->store_func);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)original_Find,  hooked_Find);
    DetourAttach(&(PVOID&)original_Store, hooked_Store);
    LONG result = DetourTransactionCommit();

    log.printf("[DoorLimit] _Find/_Store hooks %s (result=%ld)\n",
               result == NO_ERROR ? "installed" : "FAILED", result);
    log.printf("[DoorLimit] hostDoors=%p clientDoors=%p iter=%p\n",
               g_hostDoors, g_clientDoors, g_doorIter);
}

void unpatch_door_limit()
{
    if (!original_Find) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)original_Find,  hooked_Find);
    DetourDetach(&(PVOID&)original_Store, hooked_Store);
    DetourTransactionCommit();
}
