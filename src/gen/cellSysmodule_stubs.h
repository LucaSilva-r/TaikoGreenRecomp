/* Auto-generated HLE stubs for cellSysmodule -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for cellSysmodule */
/* NID 0x112A5EE9 */ int32_t cellSysmoduleUnloadModule_stub(ppu_context* ctx);
/* NID 0x32267A31 */ int32_t cellSysmoduleLoadModule_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_cellSysmodule;
extern const nid_entry_cellSysmodule cellSysmodule_nid_table[];
extern const int cellSysmodule_nid_table_size;
