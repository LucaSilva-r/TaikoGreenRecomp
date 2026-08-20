/* Auto-generated HLE stubs for cellNetCtl -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for cellNetCtl */
/* NID 0x0CE13C6B */ int32_t cellNetCtlAddHandler_stub(ppu_context* ctx);
/* NID 0x105EE2CB */ int32_t cellNetCtlTerm_stub(ppu_context* ctx);
/* NID 0x1E585B5D */ int32_t cellNetCtlGetInfo_stub(ppu_context* ctx);
/* NID 0x8B3EBA69 */ int32_t cellNetCtlGetState_stub(ppu_context* ctx);
/* NID 0x901815C3 */ int32_t cellNetCtlDelHandler_stub(ppu_context* ctx);
/* NID 0xBD5A59FC */ int32_t cellNetCtlInit_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_cellNetCtl;
extern const nid_entry_cellNetCtl cellNetCtl_nid_table[];
extern const int cellNetCtl_nid_table_size;
