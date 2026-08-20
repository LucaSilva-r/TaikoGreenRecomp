/* Auto-generated HLE stubs for sceNp -- do not edit by hand. */

#include "sceNp_stubs.h"
#include <stdio.h>

/* NID 0x4885AA18 */
int32_t sceNpTerm_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sceNp::sceNpTerm\n");
    return CELL_OK;
}

/* NID 0xAD218FAF */
int32_t sceNpDrmIsAvailable_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sceNp::sceNpDrmIsAvailable\n");
    return CELL_OK;
}

/* NID 0xBD28FDBF */
int32_t sceNpInit_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: sceNp::sceNpInit\n");
    return CELL_OK;
}


/* NID table */
const nid_entry_sceNp sceNp_nid_table[] = {
    { 0x4885AA18u, (void*)sceNpTerm_stub, "sceNpTerm" },
    { 0xAD218FAFu, (void*)sceNpDrmIsAvailable_stub, "sceNpDrmIsAvailable" },
    { 0xBD28FDBFu, (void*)sceNpInit_stub, "sceNpInit" },
};
const int sceNp_nid_table_size = sizeof(sceNp_nid_table) / sizeof(sceNp_nid_table[0]);
