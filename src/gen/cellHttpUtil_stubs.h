/* Auto-generated HLE stubs for cellHttpUtil -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for cellHttpUtil */
/* NID 0x32FAAF58 */ int32_t cellHttpUtilParseUri_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_cellHttpUtil;
extern const nid_entry_cellHttpUtil cellHttpUtil_nid_table[];
extern const int cellHttpUtil_nid_table_size;
