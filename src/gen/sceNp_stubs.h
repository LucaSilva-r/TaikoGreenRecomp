/* Auto-generated HLE stubs for sceNp -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for sceNp */
/* NID 0x4885AA18 */ int32_t sceNpTerm_stub(ppu_context* ctx);
/* NID 0xAD218FAF */ int32_t sceNpDrmIsAvailable_stub(ppu_context* ctx);
/* NID 0xBD28FDBF */ int32_t sceNpInit_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_sceNp;
extern const nid_entry_sceNp sceNp_nid_table[];
extern const int sceNp_nid_table_size;
