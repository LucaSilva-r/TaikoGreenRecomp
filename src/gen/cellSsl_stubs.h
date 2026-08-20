/* Auto-generated HLE stubs for cellSsl -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for cellSsl */
/* NID 0x218B64DA */ int32_t cellSslCertGetNotAfter_stub(ppu_context* ctx);
/* NID 0x31D9BA8D */ int32_t cellSslCertGetNotBefore_stub(ppu_context* ctx);
/* NID 0xFB02C9D2 */ int32_t cellSslInit_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_cellSsl;
extern const nid_entry_cellSsl cellSsl_nid_table[];
extern const int cellSsl_nid_table_size;
