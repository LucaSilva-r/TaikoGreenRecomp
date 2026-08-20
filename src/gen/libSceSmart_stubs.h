/* Auto-generated HLE stubs for libSceSmart -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for libSceSmart */
/* NID 0x14E993DD */ int32_t nid_0x14E993DD_stub(ppu_context* ctx);
/* NID 0x1D617490 */ int32_t nid_0x1D617490_stub(ppu_context* ctx);
/* NID 0x2650869B */ int32_t nid_0x2650869B_stub(ppu_context* ctx);
/* NID 0x2D5AA778 */ int32_t nid_0x2D5AA778_stub(ppu_context* ctx);
/* NID 0x2F5CD187 */ int32_t nid_0x2F5CD187_stub(ppu_context* ctx);
/* NID 0x66ACD98B */ int32_t nid_0x66ACD98B_stub(ppu_context* ctx);
/* NID 0x6A7495E5 */ int32_t nid_0x6A7495E5_stub(ppu_context* ctx);
/* NID 0x993DFCDC */ int32_t nid_0x993DFCDC_stub(ppu_context* ctx);
/* NID 0xA6F4F2A5 */ int32_t nid_0xA6F4F2A5_stub(ppu_context* ctx);
/* NID 0xC20EBF17 */ int32_t nid_0xC20EBF17_stub(ppu_context* ctx);
/* NID 0xEAD34FE9 */ int32_t nid_0xEAD34FE9_stub(ppu_context* ctx);
/* NID 0xF9A8EF67 */ int32_t nid_0xF9A8EF67_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_libSceSmart;
extern const nid_entry_libSceSmart libSceSmart_nid_table[];
extern const int libSceSmart_nid_table_size;
