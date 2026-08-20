/* Auto-generated HLE stubs for cellGame -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for cellGame */
/* NID 0x3A5D726A */ int32_t cellGameGetParamString_stub(ppu_context* ctx);
/* NID 0x70ACEC67 */ int32_t cellGameContentPermit_stub(ppu_context* ctx);
/* NID 0xF52639EA */ int32_t cellGameBootCheck_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_cellGame;
extern const nid_entry_cellGame cellGame_nid_table[];
extern const int cellGame_nid_table_size;
