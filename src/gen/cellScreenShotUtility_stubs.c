/* Auto-generated HLE stubs for cellScreenShotUtility -- do not edit by hand. */

#include "cellScreenShotUtility_stubs.h"
#include <stdio.h>

/* NID 0x9E33AB8F */
int32_t cellScreenShotEnable_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellScreenShotUtility::cellScreenShotEnable\n");
    return CELL_OK;
}

/* NID 0xD3AD63E4 */
int32_t cellScreenShotSetParameter_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellScreenShotUtility::cellScreenShotSetParameter\n");
    return CELL_OK;
}


/* NID table */
const nid_entry_cellScreenShotUtility cellScreenShotUtility_nid_table[] = {
    { 0x9E33AB8Fu, (void*)cellScreenShotEnable_stub, "cellScreenShotEnable" },
    { 0xD3AD63E4u, (void*)cellScreenShotSetParameter_stub, "cellScreenShotSetParameter" },
};
const int cellScreenShotUtility_nid_table_size = sizeof(cellScreenShotUtility_nid_table) / sizeof(cellScreenShotUtility_nid_table[0]);
