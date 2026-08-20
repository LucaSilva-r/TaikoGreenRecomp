/* Auto-generated HLE stubs for cellScreenShotUtility -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for cellScreenShotUtility */
/* NID 0x9E33AB8F */ int32_t cellScreenShotEnable_stub(ppu_context* ctx);
/* NID 0xD3AD63E4 */ int32_t cellScreenShotSetParameter_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_cellScreenShotUtility;
extern const nid_entry_cellScreenShotUtility cellScreenShotUtility_nid_table[];
extern const int cellScreenShotUtility_nid_table_size;
