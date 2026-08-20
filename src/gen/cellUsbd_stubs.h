/* Auto-generated HLE stubs for cellUsbd -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for cellUsbd */
/* NID 0x254289AC */ int32_t cellUsbdOpenPipe_stub(ppu_context* ctx);
/* NID 0x2FB08E1E */ int32_t nid_0x2FB08E1E_stub(ppu_context* ctx);
/* NID 0x359BEFBA */ int32_t cellUsbdRegisterLdd_stub(ppu_context* ctx);
/* NID 0x35F22AC3 */ int32_t cellUsbdEnd_stub(ppu_context* ctx);
/* NID 0x5C832BD7 */ int32_t nid_0x5C832BD7_stub(ppu_context* ctx);
/* NID 0x9763E962 */ int32_t cellUsbdClosePipe_stub(ppu_context* ctx);
/* NID 0x97CF128E */ int32_t cellUsbdControlTransfer_stub(ppu_context* ctx);
/* NID 0xAC77EB78 */ int32_t cellUsbdBulkTransfer_stub(ppu_context* ctx);
/* NID 0xD0E766FE */ int32_t cellUsbdInit_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_cellUsbd;
extern const nid_entry_cellUsbd cellUsbd_nid_table[];
extern const int cellUsbd_nid_table_size;
