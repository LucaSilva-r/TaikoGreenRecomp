/* Auto-generated HLE stubs for sys_io -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for sys_io */
/* NID 0x1CF98800 */ int32_t cellPadInit_stub(ppu_context* ctx);
/* NID 0x1F71ECBE */ int32_t nid_0x1F71ECBE_stub(ppu_context* ctx);
/* NID 0x2073B7F6 */ int32_t cellKbClearBuf_stub(ppu_context* ctx);
/* NID 0x2F1774D5 */ int32_t cellKbGetInfo_stub(ppu_context* ctx);
/* NID 0x3138E632 */ int32_t cellMouseGetData_stub(ppu_context* ctx);
/* NID 0x3F72C56E */ int32_t cellKbSetLEDStatus_stub(ppu_context* ctx);
/* NID 0x433F6EC0 */ int32_t cellKbInit_stub(ppu_context* ctx);
/* NID 0x4AB1FA77 */ int32_t cellKbCnvRawCode_stub(ppu_context* ctx);
/* NID 0x4D9B75D5 */ int32_t cellPadEnd_stub(ppu_context* ctx);
/* NID 0x578E3C98 */ int32_t cellPadSetPortSetting_stub(ppu_context* ctx);
/* NID 0x5BAF30FB */ int32_t cellMouseGetInfo_stub(ppu_context* ctx);
/* NID 0x8B72CDA1 */ int32_t cellPadGetData_stub(ppu_context* ctx);
/* NID 0xA5F85E4D */ int32_t cellKbSetCodeType_stub(ppu_context* ctx);
/* NID 0xA703A51D */ int32_t cellPadGetInfo2_stub(ppu_context* ctx);
/* NID 0xBFCE3285 */ int32_t cellKbEnd_stub(ppu_context* ctx);
/* NID 0xC9030138 */ int32_t cellMouseInit_stub(ppu_context* ctx);
/* NID 0xDEEFDFA7 */ int32_t cellKbSetReadMode_stub(ppu_context* ctx);
/* NID 0xE10183CE */ int32_t cellMouseEnd_stub(ppu_context* ctx);
/* NID 0xF65544EE */ int32_t cellPadSetActDirect_stub(ppu_context* ctx);
/* NID 0xFF0A21B7 */ int32_t cellKbRead_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_sys_io;
extern const nid_entry_sys_io sys_io_nid_table[];
extern const int sys_io_nid_table_size;
