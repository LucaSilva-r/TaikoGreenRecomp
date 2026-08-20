/* Auto-generated HLE stubs for cellHttpUtil -- do not edit by hand. */

#include "cellHttpUtil_stubs.h"
#include <stdio.h>

/* NID 0x32FAAF58 */
int32_t cellHttpUtilParseUri_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellHttpUtil::cellHttpUtilParseUri\n");
    return CELL_OK;
}


/* NID table */
const nid_entry_cellHttpUtil cellHttpUtil_nid_table[] = {
    { 0x32FAAF58u, (void*)cellHttpUtilParseUri_stub, "cellHttpUtilParseUri" },
};
const int cellHttpUtil_nid_table_size = sizeof(cellHttpUtil_nid_table) / sizeof(cellHttpUtil_nid_table[0]);
