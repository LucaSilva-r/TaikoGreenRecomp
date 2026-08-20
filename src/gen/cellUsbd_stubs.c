/* Auto-generated HLE stubs for cellUsbd -- do not edit by hand. */

#include "cellUsbd_stubs.h"
#include <stdio.h>

/* NID 0x254289AC */
int32_t cellUsbdOpenPipe_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellUsbd::cellUsbdOpenPipe\n");
    return CELL_OK;
}

/* NID 0x2FB08E1E */
int32_t nid_0x2FB08E1E_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellUsbd::nid_0x2FB08E1E\n");
    return CELL_OK;
}

/* NID 0x359BEFBA */
int32_t cellUsbdRegisterLdd_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellUsbd::cellUsbdRegisterLdd\n");
    return CELL_OK;
}

/* NID 0x35F22AC3 */
int32_t cellUsbdEnd_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellUsbd::cellUsbdEnd\n");
    return CELL_OK;
}

/* NID 0x5C832BD7 */
int32_t nid_0x5C832BD7_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellUsbd::nid_0x5C832BD7\n");
    return CELL_OK;
}

/* NID 0x9763E962 */
int32_t cellUsbdClosePipe_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellUsbd::cellUsbdClosePipe\n");
    return CELL_OK;
}

/* NID 0x97CF128E */
int32_t cellUsbdControlTransfer_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellUsbd::cellUsbdControlTransfer\n");
    return CELL_OK;
}

/* NID 0xAC77EB78 */
int32_t cellUsbdBulkTransfer_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellUsbd::cellUsbdBulkTransfer\n");
    return CELL_OK;
}

/* NID 0xD0E766FE */
int32_t cellUsbdInit_stub(ppu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "UNIMPLEMENTED: cellUsbd::cellUsbdInit\n");
    return CELL_OK;
}


/* NID table */
const nid_entry_cellUsbd cellUsbd_nid_table[] = {
    { 0x254289ACu, (void*)cellUsbdOpenPipe_stub, "cellUsbdOpenPipe" },
    { 0x2FB08E1Eu, (void*)nid_0x2FB08E1E_stub, "nid_0x2FB08E1E" },
    { 0x359BEFBAu, (void*)cellUsbdRegisterLdd_stub, "cellUsbdRegisterLdd" },
    { 0x35F22AC3u, (void*)cellUsbdEnd_stub, "cellUsbdEnd" },
    { 0x5C832BD7u, (void*)nid_0x5C832BD7_stub, "nid_0x5C832BD7" },
    { 0x9763E962u, (void*)cellUsbdClosePipe_stub, "cellUsbdClosePipe" },
    { 0x97CF128Eu, (void*)cellUsbdControlTransfer_stub, "cellUsbdControlTransfer" },
    { 0xAC77EB78u, (void*)cellUsbdBulkTransfer_stub, "cellUsbdBulkTransfer" },
    { 0xD0E766FEu, (void*)cellUsbdInit_stub, "cellUsbdInit" },
};
const int cellUsbd_nid_table_size = sizeof(cellUsbd_nid_table) / sizeof(cellUsbd_nid_table[0]);
