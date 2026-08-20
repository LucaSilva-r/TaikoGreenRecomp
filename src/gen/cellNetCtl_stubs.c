/* Auto-generated HLE stubs for cellNetCtl -- do not edit by hand. */

#include "cellNetCtl_stubs.h"
#include <stdio.h>

/* NID 0x0CE13C6B */
int32_t cellNetCtlAddHandler_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellNetCtl::cellNetCtlAddHandler\n");
    return CELL_OK;
}

/* NID 0x105EE2CB */
int32_t cellNetCtlTerm_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellNetCtl::cellNetCtlTerm\n");
    return CELL_OK;
}

/* NID 0x1E585B5D */
int32_t cellNetCtlGetInfo_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellNetCtl::cellNetCtlGetInfo\n");
    return CELL_OK;
}

/* NID 0x8B3EBA69 */
int32_t cellNetCtlGetState_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellNetCtl::cellNetCtlGetState\n");
    return CELL_OK;
}

/* NID 0x901815C3 */
int32_t cellNetCtlDelHandler_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellNetCtl::cellNetCtlDelHandler\n");
    return CELL_OK;
}

/* NID 0xBD5A59FC */
int32_t cellNetCtlInit_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellNetCtl::cellNetCtlInit\n");
    return CELL_OK;
}


/* NID table */
const nid_entry_cellNetCtl cellNetCtl_nid_table[] = {
    { 0x0CE13C6Bu, (void*)cellNetCtlAddHandler_stub, "cellNetCtlAddHandler" },
    { 0x105EE2CBu, (void*)cellNetCtlTerm_stub, "cellNetCtlTerm" },
    { 0x1E585B5Du, (void*)cellNetCtlGetInfo_stub, "cellNetCtlGetInfo" },
    { 0x8B3EBA69u, (void*)cellNetCtlGetState_stub, "cellNetCtlGetState" },
    { 0x901815C3u, (void*)cellNetCtlDelHandler_stub, "cellNetCtlDelHandler" },
    { 0xBD5A59FCu, (void*)cellNetCtlInit_stub, "cellNetCtlInit" },
};
const int cellNetCtl_nid_table_size = sizeof(cellNetCtl_nid_table) / sizeof(cellNetCtl_nid_table[0]);
