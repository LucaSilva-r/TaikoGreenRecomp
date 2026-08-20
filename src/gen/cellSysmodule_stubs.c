/* Auto-generated HLE stubs for cellSysmodule -- do not edit by hand. */

#include "cellSysmodule_stubs.h"
#include <stdio.h>

/* NID 0x112A5EE9 */
int32_t cellSysmoduleUnloadModule_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellSysmodule::cellSysmoduleUnloadModule\n");
    return CELL_OK;
}

/* NID 0x32267A31 */
int32_t cellSysmoduleLoadModule_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellSysmodule::cellSysmoduleLoadModule\n");
    return CELL_OK;
}


/* NID table */
const nid_entry_cellSysmodule cellSysmodule_nid_table[] = {
    { 0x112A5EE9u, (void*)cellSysmoduleUnloadModule_stub, "cellSysmoduleUnloadModule" },
    { 0x32267A31u, (void*)cellSysmoduleLoadModule_stub, "cellSysmoduleLoadModule" },
};
const int cellSysmodule_nid_table_size = sizeof(cellSysmodule_nid_table) / sizeof(cellSysmodule_nid_table[0]);
