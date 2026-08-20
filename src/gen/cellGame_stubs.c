/* Auto-generated HLE stubs for cellGame -- do not edit by hand. */

#include "cellGame_stubs.h"
#include <stdio.h>

/* NID 0x3A5D726A */
int32_t cellGameGetParamString_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellGame::cellGameGetParamString\n");
    return CELL_OK;
}

/* NID 0x70ACEC67 */
int32_t cellGameContentPermit_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellGame::cellGameContentPermit\n");
    return CELL_OK;
}

/* NID 0xF52639EA */
int32_t cellGameBootCheck_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellGame::cellGameBootCheck\n");
    return CELL_OK;
}


/* NID table */
const nid_entry_cellGame cellGame_nid_table[] = {
    { 0x3A5D726Au, (void*)cellGameGetParamString_stub, "cellGameGetParamString" },
    { 0x70ACEC67u, (void*)cellGameContentPermit_stub, "cellGameContentPermit" },
    { 0xF52639EAu, (void*)cellGameBootCheck_stub, "cellGameBootCheck" },
};
const int cellGame_nid_table_size = sizeof(cellGame_nid_table) / sizeof(cellGame_nid_table[0]);
