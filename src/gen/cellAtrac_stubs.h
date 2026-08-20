/* Auto-generated HLE stubs for cellAtrac -- do not edit by hand. */
#pragma once

#include <stdint.h>

/* Forward declarations */
typedef struct ppu_context ppu_context;

/* Return type alias */
#ifndef CELL_OK
#define CELL_OK 0
#endif

/* NID -> stub mappings for cellAtrac */
/* NID 0x06DDB53E */ int32_t nid_0x06DDB53E_stub(ppu_context* ctx);
/* NID 0x0F9667B6 */ int32_t nid_0x0F9667B6_stub(ppu_context* ctx);
/* NID 0x2642D4CC */ int32_t nid_0x2642D4CC_stub(ppu_context* ctx);
/* NID 0x2BFFF084 */ int32_t nid_0x2BFFF084_stub(ppu_context* ctx);
/* NID 0x46CFC013 */ int32_t nid_0x46CFC013_stub(ppu_context* ctx);
/* NID 0x5F62D546 */ int32_t nid_0x5F62D546_stub(ppu_context* ctx);
/* NID 0x66AFC68E */ int32_t nid_0x66AFC68E_stub(ppu_context* ctx);
/* NID 0x761CB9BE */ int32_t nid_0x761CB9BE_stub(ppu_context* ctx);
/* NID 0x7772EB2B */ int32_t nid_0x7772EB2B_stub(ppu_context* ctx);
/* NID 0x78BA5C41 */ int32_t nid_0x78BA5C41_stub(ppu_context* ctx);
/* NID 0x8EB0E65F */ int32_t nid_0x8EB0E65F_stub(ppu_context* ctx);
/* NID 0x99EFE171 */ int32_t nid_0x99EFE171_stub(ppu_context* ctx);
/* NID 0x99FB73D1 */ int32_t nid_0x99FB73D1_stub(ppu_context* ctx);
/* NID 0xAB6B6DBF */ int32_t nid_0xAB6B6DBF_stub(ppu_context* ctx);
/* NID 0xB5C11938 */ int32_t nid_0xB5C11938_stub(ppu_context* ctx);
/* NID 0xBE07F05E */ int32_t nid_0xBE07F05E_stub(ppu_context* ctx);
/* NID 0xC9A95FCB */ int32_t nid_0xC9A95FCB_stub(ppu_context* ctx);

/* NID table for runtime registration */
typedef struct { uint32_t nid; void* func; const char* name; } nid_entry_cellAtrac;
extern const nid_entry_cellAtrac cellAtrac_nid_table[];
extern const int cellAtrac_nid_table_size;
